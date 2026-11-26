// Matter for the Orcon controller (experimental: docs/MATTER.md lists what is proven on hardware).
//
// * Link-time wraps (see CMakeLists.txt), so Orcon's sources stay byte-identical: setup() first starts the
//   telemetry task (and the heap monitor), before Orcon touches Wi-Fi or NVS; Matter starts after net::begin(),
//   or from net::loop() once Orcon is connected to the home network (a router that came up after the board).
// * Device model (CONFIG_ORCON_MATTER_BRIDGE): endpoint 1 Aggregator, endpoint 2 the bridged "Orcon ventilator":
//   Bridged Node + Fan (Fan Control: Off/Low/Medium/High/Auto, MultiSpeed 3, the Fan's optional On/Off). Apple Home
//   names a bridged device after its Bridged Device Basic Information NodeLabel; a device without a DCL entry is
//   otherwise always "Matter-accessoire" (docs/MATTER.md). Orcon has no "off": the fan switched off
//   (FanMode Off, percent/speed 0, or On/Off off) is Afwezig; on again returns to the last stand. No outlet endpoint.
//   Without the option the same fan is endpoint 1 (no bridge): only for the RAM comparison in the QEMU probe.
// * Control (CONFIG_ORCON_MATTER_CONTROL): a request goes to a one-slot mailbox (the latest wins); Orcon's loop
//   task hands it to the same admission as the web page and Siri (setupweb::matterControl -> ctlAdmit, the only RF
//   entry). What Matter shows follows the unit's own reports (mirror()), so a refused or unconfirmed request falls
//   back to the unit's real mode. Without the option the same path runs as a dry run: logged, never admitted.
//   Fan Control is code-driven in this SDK (FanControlCluster.cpp): its writes reach g_fanDelegate, never
//   attributeCb; the fan's On/Off still arrives in attributeCb (PRE_UPDATE). Nothing counts before Matter is
//   ready: the clusters' own restore at boot is no request.
// * Telemetry: every ORCON_MATTER_TELEMETRY_MS one "[mem]" line with internal heap free / minimum /
//   largest block, DMA-capable free, Matter's state, the stack high-water mark of every long-lived task,
//   and the NVS entries used/total of "nvs" and (when configured) the dedicated Matter NVS "chipkvs".
//   With CONFIG_ORCON_MATTER_HEAPMON the same task samples the heap monitor every 20 ms.
// * Matter's state (orcon_matter_state.h): waiting -> starting -> ready | failed, once per boot. The diagnostics build
//   starts Matter only when its telemetry runs (and, with the monitor, the measurement is complete); a definitive
//   fault stays "failed" with its step and error until a reboot.
// * Storage: with config/e18-matter-nvs.defaults all CHIP/ESP-Matter data goes to a dedicated NVS
//   (orcon_matter_nvs.cpp); a region holding foreign bytes is refused and Matter stays off.
#include "orcon_matter.h"
#include "orcon_matter_control.h"
#include "orcon_matter_identity.h"
#include "orcon_matter_nvs.h"
#include "orcon_matter_heapmon.h"
#include "orcon_matter_state.h"

#include <atomic>
#include <stdio.h>

#include <WiFi.h>
#include <nvs.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_endpoint.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <esp_app_desc.h>
#include <esp_mac.h>

#include <access/SubjectDescriptor.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app/AttributeValueDecoder.h>
#include <app/InteractionModelEngine.h>
#include <app/data-model-provider/OperationTypes.h>
#include <app/persistence/AttributePersistenceProviderInstance.h>
#include <lib/core/TLV.h>
#include <app/clusters/fan-control-server/CodegenIntegration.h>
#include <app/clusters/fan-control-server/FanControlCluster.h>
#include <app/clusters/fan-control-server/fan-control-delegate.h>
#include <platform/CHIPDeviceLayer.h>

#if CONFIG_ORCON_MATTER_CONTROL && CONFIG_ORCON_MATTER_FIVE_SWITCHES
#error "Matter control and its mirror are built for the fan model only"
#endif

namespace {
constexpr bool streq(const char* a, const char* b) { return *a == *b && (*a == '\0' || streq(a + 1, b + 1)); }
}
// The names come from components/orcon_matter/orcon_chip_config.h (CONFIG_CHIP_PROJECT_CONFIG): CHIP's Basic
// Information and the bridged fan must show the same identity, so this file has to see the project config too.
static_assert(streq(CHIP_DEVICE_CONFIG_DEVICE_NAME, orcon_matter::kDeviceLabel), "CONFIG_CHIP_PROJECT_CONFIG not applied");
static_assert(sizeof(CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME) - 1 <= orcon_matter::kMaxLabel &&
              sizeof(CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME) - 1 <= orcon_matter::kMaxLabel, "Matter limits names to 32");

namespace updsvc { bool updateBoot(); }   // Orcon's src/fw_update_esp.h (update restart)
namespace setupweb {   // Orcon's src/setup_web.h (Matter control), loop task only
int matterControl(const char* key, const char*& e, uint32_t& waitMs);
uint8_t matterUnitMode(bool& txBusy);
}

#ifndef ORCON_MATTER_TELEMETRY_MS
#define ORCON_MATTER_TELEMETRY_MS 10000
#endif

using namespace chip::app::Clusters;
using namespace esp_matter;

namespace {
const char* TAG = "orcon_matter";

using orcon_matter::Req;

uint16_t g_fanEp = 0;
QueueHandle_t g_mailbox = nullptr;     // one open request, the latest wins. No telemetry task: no queue, no requests
// Written by the CHIP task only: queued requests, refusals (fan "off"), and requests replaced by a newer one before
// Orcon's loop took them (still "busy" in the [mem] line, tools/memlog.py).
volatile uint32_t g_refused = 0, g_busy = 0, g_queued = 0;
std::atomic<bool> g_mirroring{false};   // the loop task changes the fan cluster itself (stack locked): no request
volatile int g_lastEvent = -1;
uint32_t g_startHeap = 0, g_afterStartHeap = 0;
std::atomic<bool> g_telemetryUp{false};   // set by the telemetry task itself, after its first sample
orcon_matter::Telemetry g_telemetry = orcon_matter::Telemetry::NotStarted;   // earlyBegin() result (loop task only)
orcon_matter::MatterState g_mtr;
constexpr uint32_t kServerReadyLimitMs = 30000;   // esp_matter::start() -> kServerReady; seconds in practice

// Unconditional (printf, not ESP_LOG, so also below the log level): tools/memlog.py counts the starts per boot.
void stateLine(const char* what) {
    printf("[orcon-matter] %s state=%s step=%s err=0x%x t_ms=%u\n", what, orcon_matter::MatterState::name(g_mtr.state()),
           g_mtr.step(), unsigned(g_mtr.err()), unsigned(millis()));
}

void post(Req r) {   // CHIP task: the latest request replaces one that Orcon's loop has not taken yet
    if (!g_mailbox) return;
    if (uxQueueMessagesWaiting(g_mailbox)) g_busy = g_busy + 1;
    xQueueOverwrite(g_mailbox, &r);
    g_queued = g_queued + 1;
}

// Fan Control's delegate (CHIP task, stack locked), set through the cluster config: called after every committed
// change of FanMode / PercentSetting / SpeedSetting with the resulting mode (the cluster maps percent to a mode).
struct FanDelegate final : FanControl::Delegate {
    chip::Protocols::InteractionModel::Status HandleStep(FanControl::StepDirectionEnum, bool, bool) override {
        return chip::Protocols::InteractionModel::Status::UnsupportedCommand;   // no Step feature on this fan
    }
    void OnFanDriveStateChanged(const FanControl::FanDriveState& s) override {
        if (g_mirroring.load() || !orcon_matter::matterReady()) return;   // our own view update, or the boot restore
        const Req r = orcon_matter::reqOfFanMode(uint8_t(s.mode));      // off is Afwezig
        if (r == Req::None) { g_refused = g_refused + 1; return; }      // an unknown mode: no request, the view returns
        post(r);
    }
};
FanDelegate g_fanDelegate;

#if CONFIG_ORCON_MATTER_FIVE_SWITCHES
// Model B: five mutually exclusive On/Off plug-in units "Stand 1/2/3", "Auto", "Afwezig" -- the
// device type Apple lists as supported (outlets/switches). ON requests that mode; a stand cannot be
// switched OFF (Orcon has no "off"); "Afwezig" OFF asks for the last confirmed non-away mode.
uint16_t g_switchEp[5] = {};
constexpr Req kSwitchReq[5] = {Req::Low, Req::Medium, Req::High, Req::Auto, Req::AwayOn};

bool switchRequest(uint16_t ep, uint32_t cluster, uint32_t attr, const esp_matter_attr_val_t* v, Req& out,
                   bool& refuse) {
    if (cluster != OnOff::Id || attr != OnOff::Attributes::OnOff::Id) return false;
    for (int i = 0; i < 5; ++i) {
        if (ep != g_switchEp[i]) continue;
        if (v->val.b) { out = kSwitchReq[i]; return true; }
        if (kSwitchReq[i] == Req::AwayOn) { out = Req::AwayOff; return true; }
        refuse = true;
        return false;
    }
    return false;
}

esp_err_t createSwitches(node_t* node) {
    for (int i = 0; i < 5; ++i) {
        endpoint::on_off_plug_in_unit::config_t cfg;
        cfg.on_off.on_off = false;
        endpoint_t* ep = endpoint::on_off_plug_in_unit::create(node, &cfg, ENDPOINT_FLAG_NONE, nullptr);
        if (!ep) return ESP_ERR_NO_MEM;
        g_switchEp[i] = endpoint::get_id(ep);
    }
    return ESP_OK;
}
#endif

esp_err_t attributeCb(attribute::callback_type_t type, uint16_t ep, uint32_t cluster, uint32_t attr,
                      esp_matter_attr_val_t* val, void*) {
    // CHIP task. Nothing before Matter is ready: the clusters' own init at boot (StartUpOnOff) is no request.
    if (type != attribute::PRE_UPDATE || !val || !orcon_matter::matterReady()) return ESP_OK;
    Req r;
#if CONFIG_ORCON_MATTER_FIVE_SWITCHES
    bool refuse = false;
    if (!switchRequest(ep, cluster, attr, val, r, refuse)) {
        if (!refuse) return ESP_OK;
        g_refused = g_refused + 1;               // a stand switched "off": no Orcon meaning, no request
        return ESP_ERR_INVALID_ARG;
    }
#else
    // Only the fan's own On/Off arrives here (Fan Control through g_fanDelegate): off is Afwezig, on leaves it.
    // The mirror writes it with attribute::report(), which skips this callback, so every call is a controller's.
    if (ep != g_fanEp || cluster != OnOff::Id || attr != OnOff::Attributes::OnOff::Id) return ESP_OK;
    r = orcon_matter::reqOfOnOff(val->val.b);
#endif
    post(r);
    return ESP_OK;
}

esp_err_t identifyCb(identification::callback_type_t, uint16_t, uint8_t, uint8_t, void*) { return ESP_OK; }

// Long-lived tasks only, as created by Orcon, Arduino 3.3.12, ESP-IDF 5.5.5 and CHIP (verified in the sources).
// Never "fw_update": it deletes itself, so its handle can be freed between lookup and use (Codex M1). Its stack
// minimum is in Orcon's /api/update (mem.stack_free_job).
const char* const kTasks[] = {"loopTask", "ramses_rx", "loop_watch", "CHIP", "mdns", "tiT",
                              "wifi", "sys_evt", "esp_timer", "arduino_events", "uart_event_task",
                              "async_udp", "ipc0", "ipc1", "IDLE0", "IDLE1", "orcon_mtr"};

#if CONFIG_ORCON_MATTER_CONTROL
constexpr bool kControlBuild = true;
#else
constexpr bool kControlBuild = false;
#endif

#if CONFIG_ORCON_MATTER_HEAPMON
constexpr bool kMonitorBuild = true;   // a disabled bool option is not defined at all, so never use it in an expression
constexpr uint32_t kWaitMs = orcon_matter::kHeapMonitorPeriodMs;
constexpr uint32_t kStack = 4096;   // with the monitor the QEMU runs left 1,788 B of it free (docs/MATTER.md)
#else
constexpr bool kMonitorBuild = false;
constexpr uint32_t kWaitMs = ORCON_MATTER_TELEMETRY_MS;
constexpr uint32_t kStack = 3072;   // the telemetry line alone
#endif

void telemetryTask(void*) {
    orcon_matter::heapMonitorSample();   // at once: no hole between setup()'s first sample and the periodic ones
    g_telemetryUp.store(true);           // the explicit "running" earlyBegin() waits for (Codex review 2, H3)
    uint32_t lastLine = millis();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kWaitMs));   // the mailbox is Orcon's loop task's (serviceControl)
        orcon_matter::heapMonitorSample();
        if (g_mtr.expire(millis(), kServerReadyLimitMs)) stateLine("timeout");   // no kServerReady within 30 s
        if (millis() - lastLine < ORCON_MATTER_TELEMETRY_MS) continue;
        lastLine = millis();
        const int64_t r0 = esp_timer_get_time();   // rep_us: this report's own cost (stack and NVS inspection)
        char line[768];
        const orcon_matter::MatterState::S st = g_mtr.state();
        int n = snprintf(line, sizeof line,
                         "[mem] t=%lu free=%u min=%u largest=%u dma=%u start=%u after=%u mtr=%s mtr_step=%s mtr_err=0x%x "
                         "mtr_ms=%u ev=%d q=%lu ref=%lu busy=%lu",
                         (unsigned long)(millis() / 1000),
                         unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                         unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                         unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                         unsigned(heap_caps_get_free_size(MALLOC_CAP_DMA)), unsigned(g_startHeap),
                         unsigned(g_afterStartHeap), orcon_matter::MatterState::name(st), g_mtr.step(),
                         unsigned(g_mtr.err()), unsigned(g_mtr.sinceMs()), g_lastEvent, (unsigned long)g_queued,
                         (unsigned long)g_refused, (unsigned long)g_busy);
        if (n > 0 && n < int(sizeof line)) n += orcon_matter::heapMonitorReport(line + n, sizeof line - n);
        for (const char* name : kTasks) {
            TaskHandle_t h = xTaskGetHandle(name);
            if (h && n > 0 && n < int(sizeof line) - 24)
                n += snprintf(line + n, sizeof line - n, " %s=%u", name, unsigned(uxTaskGetStackHighWaterMark(h)));
        }
        for (const char* part : {"nvs", orcon_matter::kMatterNvsLabel}) {   // NVS entries used/total per partition
            nvs_stats_t ns;
            if (nvs_get_stats(part, &ns) == ESP_OK && n > 0 && n < int(sizeof line) - 32)
                n += snprintf(line + n, sizeof line - n, " %s=%u/%u", part, unsigned(ns.used_entries),
                              unsigned(ns.total_entries));
        }
        if (n > 0 && n < int(sizeof line) - 16)
            n += snprintf(line + n, sizeof line - n, " rep_us=%u", unsigned(esp_timer_get_time() - r0));
        printf("%s\n", line);   // stdout works before Orcon's Serial.begin(), like the monitor's own lines
        orcon_matter::memWebPublish(line);   // B1-web: the same line for GET /api/mem
    }
}

struct RtosOps {   // orcon_matter_state.h startTelemetry() on FreeRTOS
    bool createQueue() { return (g_mailbox = xQueueCreate(1, sizeof(Req))) != nullptr; }
    void deleteQueue() { vQueueDelete(g_mailbox); g_mailbox = nullptr; }
    bool createTask() { return xTaskCreatePinnedToCore(telemetryTask, "orcon_mtr", kStack, nullptr, 1, nullptr, 0) == pdPASS; }
    bool taskReady() { return g_telemetryUp.load(); }
    void sleepMs(int) { vTaskDelay(1); }   // one tick = 1 ms (CONFIG_FREERTOS_HZ=1000)
};
}  // namespace

namespace orcon_matter {
// The node's NodeLabel (Basic Information, root) gets kDeviceLabel once, only while NO label is stored: never
// over a label a controller or a user wrote, also not after a firmware update (CHIP keeps it in its KVS; a Matter
// factory reset clears it, and then the default comes back). Written through the cluster itself, as a controller's
// write lands (stored and reported); a local write needs no fabric and opens nothing to the network.
// CHIP task, stack locked. Returns the write's result, or the reason nothing was written (for the probe and the log).
const char* applyDefaultNodeLabel() {
    using namespace chip;
    using namespace chip::app;
    const ConcreteAttributePath path(kRootEndpointId, BasicInformation::Id, BasicInformation::Attributes::NodeLabel::Id);
    AttributePersistenceProvider* store = GetAttributePersistenceProvider();
    if (!store) return "no-store";
    uint8_t raw[kMaxLabel + 8];
    MutableByteSpan stored(raw);
    const CHIP_ERROR have = store->ReadValue(path, stored);
    if (have != CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND) return have == CHIP_NO_ERROR ? "kept" : "read-error";
    uint8_t tlv[kMaxLabel + 8];
    TLV::TLVWriter w;
    w.Init(tlv, sizeof tlv);
    if (w.PutString(TLV::AnonymousTag(), kDeviceLabel) != CHIP_NO_ERROR || w.Finalize() != CHIP_NO_ERROR) return "encode";
    TLV::TLVReader r;
    r.Init(tlv, w.GetLengthWritten());
    if (r.Next() != CHIP_NO_ERROR) return "encode";
    const Access::SubjectDescriptor subject{};   // the device itself, no fabric
    AttributeValueDecoder decoder(r, subject);
    DataModel::WriteAttributeRequest req(ConcreteDataAttributePath(path.mEndpointId, path.mClusterId, path.mAttributeId),
                                         subject);
    DataModel::Provider* dm = InteractionModelEngine::GetInstance()->GetDataModelProvider();
    if (!dm) return "no-provider";
    return dm->WriteAttribute(req, decoder).IsSuccess() ? "written" : "write-refused";
}

void onChipEvent(const chip::DeviceLayer::ChipDeviceEvent* e, intptr_t) {
    g_lastEvent = int(e->Type);
    if (e->Type == chip::DeviceLayer::DeviceEventType::kServerReady) {   // DNS-SD is up too
        g_mtr.serverReady(millis());
        const char* nl = applyDefaultNodeLabel();
        printf("[orcon-matter] node-label %s\n", nl);
    }
    heapMonitorChipEvent(e);
}

bool serverReadySeen() { return g_mtr.serverReadySeen(); }
bool matterReady() { return g_mtr.state() == MatterState::Ready; }
uint32_t g_confirmRefused = 0;   // trial-boot confirmations refused because Matter was not ready (loop task only)
// Public, read-only, no secrets: why a trial boot does or does not confirm, readable over WiFi (GET /api/matter/state).
int stateJson(char* out, size_t n) {
    return snprintf(out, n, "{\"state\":\"%s\",\"step\":\"%s\",\"err\":\"0x%x\",\"since_ms\":%u,\"server_ready\":%s,"
                    "\"confirm_refused\":%u,\"uptime_ms\":%u}", MatterState::name(g_mtr.state()), g_mtr.step(),
                    unsigned(g_mtr.err()), unsigned(g_mtr.sinceMs()), g_mtr.serverReadySeen() ? "true" : "false",
                    unsigned(g_confirmRefused), unsigned(millis()));
}
const char* matterStateName() { return MatterState::name(g_mtr.state()); }

// ---- Matter -> Orcon (Orcon's loop task only; net::loop() hook below) --------------------------------------
namespace {
Pending g_pend;                     // the request Orcon's admission has not finished with
const char* g_lastStand = nullptr;  // the last stand the unit itself reported: where "Afwezig uit" returns
struct LastCmd { Req req = Req::None; int code = 0; const char* err = ""; uint32_t ms = 0; } g_last;
uint32_t g_mirrorMs = 0;
bool g_fanChecked = false;   // mirror(): the fan endpoint's look this boot is settled (new endpoint or not)
uint8_t g_fanTries = 0;
constexpr uint8_t kFanTries = 20;   // 20 mirror passes, 10 s
// What that first look found (diagnostics: GET /api/connect, the probe): no printf in the mirror's path.
struct FanCheck { uint32_t ms; uint32_t err; int8_t len, stored, on, shown; bool fixed; } g_fanCheck{};
constexpr uint32_t kMirrorEveryMs = 500;
const char* const kReqName[] = {"-", "1", "2", "3", "auto", "away", "away-off"};

void admitPending(uint32_t now) {
    Req in;
    if (g_mailbox && xQueueReceive(g_mailbox, &in, 0) == pdTRUE) g_pend.take(in, now);   // the latest wins
    if (!g_pend.due(now)) return;
    const Req req = g_pend.req;
    const char* key = keyOfReq(req, g_lastStand);
    int code = 409;
    const char* e = "no-confirmed-stand";   // "Afwezig uit" before the unit reported any stand this boot
    uint32_t waitMs = 0;
#if CONFIG_ORCON_MATTER_CONTROL
    if (key) code = setupweb::matterControl(key, e, waitMs);
#else
    if (key) { code = 501; e = "dry-run"; }   // a build without control: logged, never admitted
#endif
    if (g_pend.retryAfter(code, e, now)) return;   // busy for a moment: the same request again in 500 ms
    // ctlAdmit leaves e at "invalid" when it admits; "already in that mode" is no error either
    g_last = {req, code, admitted(code, e) ? "" : (e ? e : "?"), now};
    ESP_LOGW(TAG, "Matter request %s -> %s: %d %s", kReqName[unsigned(req) % 7], key ? key : "-", code, g_last.err);
}

// What Matter shows = the unit's own last report, unless a request is still under way (in the mailbox, waiting for
// admission, or an open RF transaction of anyone: web, Siri, Matter). Afwezig shows as the fan off (On/Off off).
void mirror(uint32_t now) {
    if (now - g_mirrorMs < kMirrorEveryMs || !matterReady()) return;
    g_mirrorMs = now;
    bool txBusy = false;
    const uint8_t unit = setupweb::matterUnitMode(txBusy);
    if (const char* k = standKeyOfUnit(unit)) g_lastStand = k;
    View v;
    const bool known = viewOfUnit(unit, v);
    if (txBusy || g_pend.req != Req::None) return;
    int8_t setOn = -1;
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    if (uxQueueMessagesWaiting(g_mailbox) == 0) {   // a write that came in before this lock is queued: it goes first
        bool onNow = true;   // the fan's On/Off as stored (NONVOLATILE: a mirrored Afwezig survives a restart)
        esp_matter_attr_val_t cur = esp_matter_invalid(nullptr);
        attribute_t* a = attribute::get(g_fanEp, OnOff::Id, OnOff::Attributes::OnOff::Id);
        if (a && attribute::get_val(a, &cur) == ESP_OK) onNow = cur.val.b;
        bool fixOn = false;
        if (!g_fanChecked) {   // this boot's first look: a fan that never stored its FanMode is new (first start, or
            // an older firmware's endpoint 2 was an outlet, whose On/Off meant the opposite), so its stored On/Off is not ours.
            // Settled only by a definite answer (never stored / stored); no store yet or a read error: look again
            // next pass, for at most kFanTries passes (QEMU, 26-11: a one-shot look missed it 3 of 3 times).
            uint8_t raw[4];
            chip::MutableByteSpan stored(raw);
            const chip::app::ConcreteAttributePath p(g_fanEp, FanControl::Id, FanControl::Attributes::FanMode::Id);
            chip::app::AttributePersistenceProvider* store = chip::app::GetAttributePersistenceProvider();
            const CHIP_ERROR got = store ? store->ReadValue(p, stored) : CHIP_ERROR_INCORRECT_STATE;
            const bool fresh = got == CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND;
            g_fanTries = uint8_t(g_fanTries + 1);
            g_fanChecked = fresh || got == CHIP_NO_ERROR || g_fanTries >= kFanTries;
            chip::app::Clusters::FanControlCluster* f0 = FanControl::FindClusterOnEndpoint(g_fanEp);
            g_fanCheck = {millis(), uint32_t(got.AsInteger()), int8_t(stored.size()),
                          int8_t(got == CHIP_NO_ERROR && stored.size() ? raw[0] : -1), int8_t(onNow),
                          int8_t(f0 ? int(f0->GetFanMode()) : -1), fresh && !onNow};
            if (fresh && !onNow) {
                onNow = true;
                fixOn = true;
            }
        }
        chip::app::Clusters::FanControlCluster* fan = FanControl::FindClusterOnEndpoint(g_fanEp);
        if (fan) {
            const uint8_t shown = uint8_t(fan->GetFanMode());
            const uint8_t want = known ? v.fanMode : placeholderFor(shown, onNow);
            g_mirroring.store(true);   // SetFanMode tells the delegate synchronously: not a request
            if (want != kFanKeep && shown != want) fan->SetFanMode(FanControl::FanModeEnum(want));
            // shortcut: PercentCurrent/SpeedCurrent follow the stand (33/66/100, 1/2/3) and stay put in Auto; the
            // unit's real fan percentage (31DA) is not mapped yet. Upgrade when Woning shows the current speed.
            if (known) if (const uint8_t pct = percentOfFanMode(v.fanMode)) fan->SetPercentCurrent(pct);
            if (known) if (const uint8_t spd = speedOfFanMode(v.fanMode)) fan->SetSpeedCurrent(spd);
            g_mirroring.store(false);
        }
        if (known && a && onNow != v.on) setOn = v.on;
        else if (!known && fixOn) setOn = 1;   // the placeholder Auto shows the fan on, not a stale "off"
    }
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    if (setOn >= 0) {   // report(): set, store and tell the subscribers, without attributeCb (it takes the lock itself)
        esp_matter_attr_val_t nv = esp_matter_bool(setOn == 1);
        attribute::report(g_fanEp, OnOff::Id, OnOff::Attributes::OnOff::Id, &nv);
    }
}
}  // namespace

void serviceControl(uint32_t now) {
#if !CONFIG_ORCON_MATTER_FIVE_SWITCHES   // model B has neither the fan delegate nor this mirror
    if (!g_mailbox) return;
    admitPending(now);
    mirror(now);
#else
    (void)now;
#endif
}

#if CONFIG_ORCON_PROBE_CONTROL
// QEMU probe only (Matter over Ethernet, no Orcon Wi-Fi; not in firmware builds): begin()'s state steps around
// the esp_matter::start() the probe makes itself. The option exists only in the probe, never in a firmware build.
void probeStarting() { g_mtr.start(millis()); }
void probeHandedOver() { g_mtr.handedOver(millis()); }
#endif

int fanCheckJson(char* out, size_t n) {   // the fan endpoint's first look this boot (loop task)
    const FanCheck& c = g_fanCheck;
    return snprintf(out, n, "{\"ep\":%u,\"t_ms\":%lu,\"tries\":%u,\"fanmode\":\"%s\",\"err\":\"0x%lx\",\"onoff\":%d,"
                    "\"shown\":%d,\"fixed\":%s}",
                    unsigned(g_fanEp), (unsigned long)c.ms, unsigned(g_fanTries),
                    !g_fanTries ? "not-yet" : c.err == uint32_t(CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND.AsInteger())
                                                  ? "never-stored" : c.err == 0 ? "stored" : "unreadable",
                    (unsigned long)c.err, int(c.on), int(c.shown), c.fixed ? "true" : "false");
}

int controlJson(char* out, size_t n) {   // for the owner's GET /api/matter (loop task, like serviceControl)
    return snprintf(out, n, "{\"enabled\":%s,\"last\":\"%s\",\"code\":%d,\"error\":\"%s\",\"age_s\":%lu,"
                    "\"pending\":%s,\"return_to\":\"%s\",\"queued\":%lu,\"refused\":%lu,\"replaced\":%lu}",
                    kControlBuild ? "true" : "false", kReqName[unsigned(g_last.req) % 7], g_last.code,
                    g_last.err, (unsigned long)(g_last.ms ? (millis() - g_last.ms) / 1000 : 0),
                    g_pend.req != Req::None ? "true" : "false", g_lastStand ? g_lastStand : "",
                    (unsigned long)g_queued, (unsigned long)g_refused, (unsigned long)g_busy);
}

esp_err_t createModel() {
    node::config_t nodeCfg;
    node_t* node = node::create(&nodeCfg, attributeCb, identifyCb);
    if (!node) return ESP_ERR_NO_MEM;
#if CONFIG_ORCON_MATTER_FIVE_SWITCHES
    return createSwitches(node);
#endif

    endpoint::fan::config_t fanCfg;
    fanCfg.fan_control.fan_mode = uint8_t(FanControl::FanModeEnum::kAuto);   // (the cluster starts from its own store)
    // OffLowMedHighAuto: the cluster sets the Auto feature from this sequence (FanControlCluster::Config)
    fanCfg.fan_control.fan_mode_sequence = uint8_t(FanControl::FanModeSequenceEnum::kOffLowMedHighAuto);
    fanCfg.fan_control.percent_current = uint8_t(0);   // nullable<uint8_t>
    fanCfg.fan_control.percent_setting = uint8_t(0);
    fanCfg.fan_control.delegate = &g_fanDelegate;   // esp-matter sets it once the cluster's server exists
#if CONFIG_ORCON_MATTER_BRIDGE
    // Endpoint 1 Aggregator, endpoint 2 the bridged fan: the only structure whose name Apple Home takes over when the
    // product has no DCL entry (Bridged Device Basic Information NodeLabel).
    endpoint::aggregator::config_t aggCfg;
    endpoint_t* agg = endpoint::aggregator::create(node, &aggCfg, ENDPOINT_FLAG_NONE, nullptr);
    if (!agg) return ESP_ERR_NO_MEM;
    endpoint::bridged_node::config_t brCfg;
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);   // stable per board, the same after every update
    snprintf(brCfg.bridged_device_basic_information.unique_id, sizeof brCfg.bridged_device_basic_information.unique_id,
             "orcon-fan-%02x%02x%02x", mac[3], mac[4], mac[5]);
    endpoint_t* fan = endpoint::bridged_node::create(node, &brCfg, ENDPOINT_FLAG_NONE, nullptr);
    if (!fan) return ESP_ERR_NO_MEM;
    if (endpoint::fan::add(fan, &fanCfg) != ESP_OK) return ESP_ERR_NO_MEM;
    if (endpoint::set_parent_endpoint(fan, agg) != ESP_OK) return ESP_FAIL;
    if (cluster_t* bi = cluster::get(fan, BridgedDeviceBasicInformation::Id)) {
        namespace bdbi = cluster::bridged_device_basic_information::attribute;
        // NodeLabel is NONVOLATILE: a label a controller wrote is loaded from NVS at create and wins over this default
        char label[kMaxLabel + 1], vendor[] = CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME,
             product[] = CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME, serial[24] = CONFIG_USE_TEST_SERIAL_NUMBER;
        if (identity::serial()[0]) strlcpy(serial, identity::serial(), sizeof serial);   // the device's own
        strlcpy(label, kDeviceLabel, sizeof label);
        char version[33];
        strlcpy(version, esp_app_get_description()->version, sizeof version);
        if (!bdbi::create_node_label(bi, label, strlen(label)) || !bdbi::create_vendor_name(bi, vendor, strlen(vendor)) ||
            !bdbi::create_product_name(bi, product, strlen(product)) ||
            !bdbi::create_serial_number(bi, serial, strlen(serial)) ||
            !bdbi::create_software_version_string(bi, version, strlen(version)))
            return ESP_ERR_NO_MEM;
    } else {
        return ESP_FAIL;
    }
#else
    endpoint_t* fan = endpoint::fan::create(node, &fanCfg, ENDPOINT_FLAG_NONE, nullptr);
    if (!fan) return ESP_ERR_NO_MEM;
#endif
    cluster_t* fc = cluster::get(fan, FanControl::Id);
    cluster::fan_control::feature::multi_speed::config_t speeds;
    speeds.speed_max = kSpeedMax;   // Orcon's three stands
    if (!fc || cluster::fan_control::feature::multi_speed::add(fc, &speeds) != ESP_OK) return ESP_ERR_NO_MEM;
    cluster::on_off::config_t onCfg;
    onCfg.on_off = true;   // stored after the first mirrored report; Afwezig is off
    if (!cluster::on_off::create(fan, &onCfg, CLUSTER_FLAG_SERVER)) return ESP_ERR_NO_MEM;
    g_fanEp = endpoint::get_id(fan);
    return ESP_OK;
}

void earlyBegin() {
    heapMonitorStart();   // first: hooks, event marks and the first sample, before the queue and the task below allocate
    RtosOps os;
    g_telemetry = startTelemetry(os, 100);
    if (g_telemetry != Telemetry::Ready) {   // final for this boot: begin() keeps Matter off, memlog calls it INVALID
        printf("[mem-mon] state=failed reason=%s\n", telemetryText(g_telemetry));
        g_mtr.fail(telemetryText(g_telemetry), ESP_FAIL, millis());
    }
}

// Matter only on a connected home network, never next to Orcon's setup AP (installing, PRG recovery, AP fallback).
// WiFi.SSID() is the SSID of the CURRENT association (Arduino 3.3.12 STA.cpp:665), so this is "connected".
bool homeNetworkUp() { return WiFi.getMode() == WIFI_MODE_STA && WiFi.status() == WL_CONNECTED; }
bool g_waitForNetwork = false;   // begin() ran without one: the net::loop() hook starts Matter once connected (Codex H6)

esp_err_t begin() {
    if (g_mtr.state() != MatterState::Waiting) return ESP_ERR_INVALID_STATE;   // once per boot (Codex review 2, M1)
    if (!homeNetworkUp()) {
        g_waitForNetwork = true;
        ESP_LOGW(TAG, "no connected home network (setup AP, or the router is not up yet): Matter waits for it");
        return ESP_ERR_INVALID_STATE;
    }
    g_waitForNetwork = false;
    if (updsvc::updateBoot()) {   // the owner's restart for an update check: Matter stays off this boot only
        g_mtr.fail("update-boot", ESP_OK, millis());
        stateLine("start");
        return ESP_ERR_INVALID_STATE;
    }
    // The identity lives on the device, in Matter's own NVS (orcon_matter_identity.h): open that first.
    esp_err_t nerr = matterNvsBegin();
    if (nerr != ESP_OK) {
        g_mtr.fail("nvs", nerr, millis());
        stateLine("start");
        return nerr;
    }
    identity::State id = identity::load();
    if (id == identity::State::Absent && identity::embeddedPresent() && identity::migrateEmbedded() == ESP_OK)
        id = identity::state();   // a migration build: its own identity now lives on the device
    if (id != identity::State::Valid) {   // not activated yet (the owner's page offers it), or a damaged record
        g_mtr.fail(id == identity::State::Absent ? "not-activated" : id == identity::State::Damaged ? "identity-damaged" : "identity",
                   id == identity::State::Absent ? ESP_OK : ESP_ERR_INVALID_CRC, millis());
        stateLine("start");
        return ESP_ERR_INVALID_STATE;
    }
    if (!g_mtr.start(millis())) return ESP_ERR_INVALID_STATE;
    const StartCheck chk{g_telemetry, kMonitorBuild, heapMonitorAllocHookOk(), heapMonitorEventsOk()};
    if (const char* why = startBlocker(chk)) {   // the diagnostics build measures Matter or does not start it
        g_mtr.fail(why, ESP_ERR_INVALID_STATE, millis());
        stateLine("start");
        return ESP_ERR_INVALID_STATE;
    }
    heapMonitorSet(MonPhase::MatterStart, true);
    g_startHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const char* step = "model";
    esp_err_t err = createModel();
    if (err == ESP_OK) { step = "start"; setCredentialProviders(); err = esp_matter::start(onChipEvent); }
    if (err == ESP_OK) {
        // Orcon's net layer owns Wi-Fi (connect, reconnect, setup AP). Otherwise CHIP's
        // DriveStationState() runs its own esp_wifi_connect() loop and may esp_wifi_disconnect().
        // InitWiFi() inside start() resets the mode, so set it afterwards, under the stack lock.
        step = "wifi-handover";
        chip::DeviceLayer::PlatformMgr().LockChipStack();
        const CHIP_ERROR werr = chip::DeviceLayer::ConnectivityMgr().SetWiFiStationMode(
            chip::DeviceLayer::ConnectivityManager::kWiFiStationMode_ApplicationControlled);
        chip::DeviceLayer::PlatformMgr().UnlockChipStack();
        if (werr == CHIP_NO_ERROR) {
            g_mtr.handedOver(millis());        // ready once CHIP's kServerReady is in as well
        } else {                               // Matter runs (it cannot be stopped), but Wi-Fi is not Orcon's alone
            ESP_LOGE(TAG, "Wi-Fi not handed to Orcon: SetWiFiStationMode error 0x%x", unsigned(werr.AsInteger()));
            err = ESP_FAIL;
        }
    }
    if (err != ESP_OK) g_mtr.fail(step, err, millis());
    g_afterStartHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    heapMonitorSet(MonPhase::MatterStart, false);
    stateLine("start");
    ESP_LOGI(TAG, "Matter start: %s (internal heap %u -> %u)", esp_err_to_name(err), unsigned(g_startHeap),
             unsigned(g_afterStartHeap));
    return err;
}
}  // namespace orcon_matter

// What tells a Matter image apart (src/ota_policy.cpp: variant control-matter needs it, control refuses it), so no
// update adds or removes Matter by itself. Kept by the linker (-u, CMakeLists.txt); the policy holds it only encoded.
extern "C" __attribute__((used)) const char orcon_matter_marker[] = "ORCON-MATTER-FIRMWARE v1";

// The hooks. Orcon's setup() is wrapped whole: the telemetry task (and the monitor) start before its first line.
extern "C" void __real__Z5setupv();
extern "C" void __wrap__Z5setupv() {
    orcon_matter::earlyBegin();
    __real__Z5setupv();
}
// Orcon's setup() calls net::begin(); Matter follows it, fail-soft (Orcon continues on error).
extern "C" void __real__ZN3net5beginEv();
extern "C" void __wrap__ZN3net5beginEv() {
    __real__ZN3net5beginEv();
    const esp_err_t err = orcon_matter::begin();
    if (err != ESP_OK) ESP_LOGW(TAG, "Matter not running (%s); Orcon continues", esp_err_to_name(err));
}
// Owner decision 24-11 (docs/MATTER.md): an image that offers Matter confirms its trial boot only once
// Matter runs. Orcon's trial step (ota_signed.cpp tick) calls this after 60 s healthy; refused, it tries again, and
// without Matter by 300 s Orcon rolls back to the previous image. Not running at all (no home network) is already
// "unhealthy" for Orcon. Nothing else in this build confirms images (no Matter OTA requestor).
extern "C" esp_err_t __real_esp_ota_mark_app_valid_cancel_rollback(void);
// A universal image without an activated identity (or with a damaged one, which no other image fixes) confirms like
// any Orcon image: Matter is not expected to run there.
extern "C" esp_err_t __wrap_esp_ota_mark_app_valid_cancel_rollback(void) {
    const orcon_matter::identity::State id = orcon_matter::identity::state();
    const bool matterExpected = id != orcon_matter::identity::State::Absent && id != orcon_matter::identity::State::Damaged;
    if (matterExpected && !orcon_matter::matterReady()) {
        ++orcon_matter::g_confirmRefused;
        stateLine("confirm-refused");
        return ESP_ERR_INVALID_STATE;
    }
    return __real_esp_ota_mark_app_valid_cancel_rollback();
}
// Orcon's loop() calls net::loop() every pass (main.cpp:112). A board that came up before its router (Orcon's AP
// fallback, then apRecovery) starts Matter here, in the same loop task as at boot, once Orcon is back on the home
// network. Only the "no network" case waits; any other start error stays final (state failed, no retry loop).
extern "C" void __real__ZN3net4loopEv();
extern "C" void __wrap__ZN3net4loopEv() {
    __real__ZN3net4loopEv();
    orcon_matter::serviceControl(millis());   // Matter requests to Orcon's admission, the unit's state back
    orcon_matter::activationService();        // the owner's activation: restart into Matter once it is made
    static uint32_t last = 0;
    if (!orcon_matter::g_waitForNetwork || millis() - last < 5000) return;
    last = millis();
    if (!orcon_matter::homeNetworkUp()) return;
    const esp_err_t err = orcon_matter::begin();
    ESP_LOGW(TAG, "Matter started late, after the home network came up: %s", esp_err_to_name(err));
}
