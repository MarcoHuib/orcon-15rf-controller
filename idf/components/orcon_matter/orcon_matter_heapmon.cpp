// Heap monitor for hardware measurements (diagnostics: CONFIG_ORCON_MATTER_HEAPMON, config/diagnostics.defaults).
//
// Starts with Orcon's setup() (orcon_matter.cpp wraps it), so before Wi-Fi and Matter: heapMonitorStart() registers
// the hooks and takes the first sample in setup()'s own task; the telemetry task created right after it confirms
// that it runs and samples every 20 ms from then on (heapMonitorSample() is only ever called by one task at a time:
// setup()'s first call happens before that task exists).
//   free8  heap_caps_get_free_size(MALLOC_CAP_8BIT): the byte-addressable heap of all regions at ONE moment (O(1)).
//   block  heap_caps_get_largest_free_block(MALLOC_CAP_8BIT): a walk of each region under its heap lock (interrupts
//          on this core masked meanwhile; another core that allocates from that region waits), so only every
//          100 ms, at each new overall low and at the start of each phase window. minblk is the lowest block any
//          walk saw (independent of free8's low); loblk is the block at free8's lowest sample.
// Orcon's heap_min (ESP.getMinFreeHeap()) is not free8: it adds per-region low-water marks from different
// moments, and it includes the IRAM heap, which only takes 32-bit accesses (no TLS buffer, no string).
//
// Phases (they may overlap; "rest" = none of the others). The Matter ones come from the events the CHIP task delivers
// to the esp_matter::start() callback; this task never reads CHIP objects. A phase that is switched on is latched
// until the next sample, so a window shorter than one period still appears (its peak may lie between samples).
//   mstart      orcon_matter::begin(): Matter NVS, data model, esp_matter::start(), Wi-Fi hand-over
//   window      commissioning window open (kCommissioningWindowOpened .. kCommissioningWindowClosed)
//   commission  a commissioner is at work: PASE up (kCommissioningSessionStarted) until kCommissioningComplete,
//               kFailSafeTimerExpired or kCommissioningSessionStopped (a failed attempt)
//   session     within 5 s after a secure session (PASE or CASE) came up; its handshake lies just BEFORE the event
//   wifi        within 10 s of a Wi-Fi connect/disconnect/scan/setup-AP start-stop or any IP event
//   rf-tx       within 2 s of radio::sx1276::txBegin() (link-time wrap; Orcon's sources stay unchanged)
//   ota         ota::writerBusy(): an image is being written (upload or download) or waits for the restart
//   update      Orcon's check or download job: within 2 s of the creation of its "fw_update" task (link-time wrap of
//               xTaskCreatePinnedToCore, so a job that fails at DNS within milliseconds is still seen), and while that
//               task exists (looked up by name every 100 ms, never dereferenced: it deletes itself)
//   rest        none of the above
// Serial lines: "[mem-mon]" start (with the result of Arduino's nvs_flash_init(): NO_FREE_PAGES or NEW_VERSION_FOUND
// means Arduino erased Orcon's NVS before setup()), event registration, failure; "[mem-low]" a new overall low (at
// most one per second; the last one always follows); "[mem-phase]" at the start and end of a window and with every
// [mem] line while it lasts, with its sample count; "[mem-fail]" plus a backtrace for the first 20 failed
// allocations. A log of these lines can be summarised offline (docs/MATTER.md).
//
// Cost (part of what it measures): see docs/MATTER.md. Static RAM, the telemetry task's 4 KB stack
// (3 KB without the monitor). Per [mem] period: walk_us = the longest single heap walk (wall time; it bounds the
// interrupt masking a walk causes, which is per region and shorter), mon_us = all time in heapMonitorSample() (walks
// included), rep_us = the [mem] report's own stack and NVS inspection (orcon_matter.cpp). The CPU share is
// (mon_us + rep_us) / period, an upper bound: preemption meanwhile counts too. Printing is not included.
#include "orcon_matter_heapmon.h"

#include <atomic>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <esp_debug_helpers.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_matter.h>   // esp_matter's DeviceEventType additions (kCommissioningSessionStarted, ...)
#include <esp_netif_types.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <esp_wifi_types.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <platform/CHIPDeviceEvent.h>

namespace ota { bool writerBusy(); }   // Orcon v1.0.1 ota_signed.h

namespace {
namespace Ev = chip::DeviceLayer::DeviceEventType;
enum Phase : uint8_t { kMStart, kWindow, kCommission, kSession, kWifi, kRfTx, kOta, kUpdate, kRest, kPhases };
const char* const kName[kPhases] = {"mstart", "window", "commission", "session", "wifi", "rf-tx", "ota", "update", "rest"};
constexpr uint32_t kNone = UINT32_MAX;
constexpr uint32_t kMaxFailLines = 20;
constexpr uint32_t kGapMs = 5 * orcon_matter::kHeapMonitorPeriodMs;   // a later sample than this counts as a gap

// Written by other tasks (CHIP, event loop, RF, any allocating task), read by the sampler: atomics, no torn or
// stale-forever values across the two cores.
std::atomic<bool> g_flag[3];                 // MonPhase::MatterStart, Window, Commission: current state
std::atomic<uint32_t> g_latch{0};            // MonPhase bits switched on since the last sample
std::atomic<uint32_t> g_wifiEvMs{0}, g_txMs{0}, g_sessionMs{0}, g_updMs{0};   // marks: 0 = never (now | 1)
std::atomic<uint32_t> g_fails{0};
std::atomic<bool> g_started{false}, g_evReg{false};
std::atomic<uint32_t> g_evRegMs{0};
std::atomic<int32_t> g_evErr{0};             // a registration error other than "no default event loop yet"
std::atomic<int32_t> g_nvsInit{1};           // Arduino's first nvs_flash_init() result; 1 = never called
bool g_hookOk = false;                       // written before the telemetry task exists

// The sampling task's own state:
struct Window { bool active; uint32_t startMs, min8, minBlock, samples; };
Window g_win[kPhases];
uint32_t g_low8 = kNone, g_lowBlock = kNone, g_lowMs = 0, g_lowMask = 0, g_lowLoggedMs = 0, g_walkUsMax = 0, g_tick = 0;
uint32_t g_minBlk = kNone, g_minBlkMs = 0;
uint32_t g_samples = 0, g_lastMs = 0, g_gapMax = 0, g_gaps = 0, g_monUs = 0;
bool g_lowPending = false, g_updateTask = false, g_evLogged = false;

uint32_t nowMs() { return uint32_t(esp_timer_get_time() / 1000); }
bool within(uint32_t t, uint32_t mark, uint32_t ms) { return mark && t - mark < ms; }

void onNetEvent(void*, esp_event_base_t base, int32_t id, void*) {
    if (base == WIFI_EVENT) switch (id) {   // state changes and scans, not periodic driver events
        case WIFI_EVENT_SCAN_DONE: case WIFI_EVENT_STA_START: case WIFI_EVENT_STA_CONNECTED:
        case WIFI_EVENT_STA_DISCONNECTED: case WIFI_EVENT_AP_START: case WIFI_EVENT_AP_STOP: break;
        default: return;
    }
    g_wifiEvMs.store(nowMs() | 1);
}

void registerEvents() {   // the task that creates the default event loop, or heapMonitorStart()
    if (g_evReg.load()) return;
    esp_err_t e = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onNetEvent, nullptr);
    if (e == ESP_OK) e = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, onNetEvent, nullptr);
    if (e == ESP_OK) { g_evRegMs.store(nowMs()); g_evReg.store(true); }
    else if (e != ESP_ERR_INVALID_STATE) g_evErr.store(e);   // INVALID_STATE: no loop yet, its creation registers (wrap)
}

// Runs in the failing task, whatever its stack and locks: a count, ROM printf (no newlib, no allocation, no heap walk)
// and a bounded backtrace. fn is the heap API that failed; the backtrace names the caller (decode with addr2line).
void onAllocFail(size_t size, uint32_t caps, const char* fn) {
    if (g_fails.fetch_add(1) >= kMaxFailLines) return;
    esp_rom_printf("[mem-fail] t_ms=%u size=%u caps=0x%x fn=%s free8=%u task=%s\n", unsigned(nowMs()), unsigned(size),
                   unsigned(caps), fn ? fn : "?", unsigned(heap_caps_get_free_size(MALLOC_CAP_8BIT)), pcTaskGetName(nullptr));
    esp_backtrace_print(12);
}

void phaseLine(int p, uint32_t t, const char* state) {
    const Window& w = g_win[p];
    printf("[mem-phase] t_ms=%u phase=%s state=%s dur_ms=%u min8=%u minblock=%u samples=%u\n", unsigned(t), kName[p],
           state, unsigned(t - w.startMs), unsigned(w.min8), unsigned(w.minBlock), unsigned(w.samples));
}
}  // namespace

namespace orcon_matter {
void heapMonitorStart() {
    if (g_started.load()) return;
    g_hookOk = heap_caps_register_failed_alloc_callback(onAllocFail) == ESP_OK;
    g_started.store(true);
    registerEvents();      // works only if the default event loop already exists
    heapMonitorSample();   // the first sample, in setup()'s task, before the telemetry task exists
}

bool heapMonitorAllocHookOk() { return g_hookOk; }
bool heapMonitorEventsOk() { return g_evReg.load() && g_evErr.load() == 0; }

void heapMonitorSet(MonPhase p, bool on) {
    g_flag[unsigned(p)].store(on);
    if (on) g_latch.fetch_or(1u << unsigned(p));
}

void heapMonitorChipEvent(const chip::DeviceLayer::ChipDeviceEvent* e) {
    switch (e->Type) {
        case Ev::kCommissioningWindowOpened: heapMonitorSet(MonPhase::Window, true); break;
        case Ev::kCommissioningWindowClosed: heapMonitorSet(MonPhase::Window, false); break;
        case Ev::kCommissioningSessionStarted: heapMonitorSet(MonPhase::Commission, true); break;
        case Ev::kCommissioningSessionStopped: case Ev::kCommissioningComplete: case Ev::kFailSafeTimerExpired:
            heapMonitorSet(MonPhase::Commission, false); break;
        case Ev::kSecureSessionEstablished: g_sessionMs.store(nowMs() | 1); break;
        default: break;
    }
}

void heapMonitorSample() {
    const int64_t s0 = esp_timer_get_time();   // the monitor's own CPU time (upper bound: preemption counts too)
    const uint32_t t = uint32_t(s0 / 1000);
    if (g_samples) {
        const uint32_t dt = t - g_lastMs;
        if (dt > g_gapMax) g_gapMax = dt;
        if (dt > kGapMs) ++g_gaps;
    }
    g_lastMs = t;
    if (g_tick % 5 == 0) g_updateTask = xTaskGetHandle("fw_update") != nullptr;   // existence only, every 100 ms
    const uint32_t latch = g_latch.exchange(0);
    bool on[kPhases] = {g_flag[0].load() || (latch & 1u), g_flag[1].load() || (latch & 2u), g_flag[2].load() || (latch & 4u),
                        within(t, g_sessionMs.load(), 5000), within(t, g_wifiEvMs.load(), 10000),
                        within(t, g_txMs.load(), 2000), ota::writerBusy(),
                        g_updateTask || within(t, g_updMs.load(), 2000), false};
    bool any = false;
    for (int p = 0; p < kRest; ++p) any |= on[p];
    on[kRest] = !any;
    const uint32_t f = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    bool walk = g_tick++ % 5 == 0 || f < g_low8;
    uint32_t mask = 0;
    for (int p = 0; p < kPhases; ++p)
        if (on[p]) { mask |= 1u << p; walk |= !g_win[p].active; }
    uint32_t b = kNone;
    if (walk) {
        const int64_t w0 = esp_timer_get_time();
        b = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        const uint32_t us = uint32_t(esp_timer_get_time() - w0);
        if (us > g_walkUsMax) g_walkUsMax = us;
        if (b < g_minBlk) { g_minBlk = b; g_minBlkMs = t; }
    }
    if (!g_samples++) {   // the first sample proves that the monitor runs, and since when
        const int32_t nvs = g_nvsInit.load();   // 0x0 ok; 0x110d / 0x1110: Arduino erased Orcon's NVS; -0x1: never called
        printf("[mem-mon] state=start t_ms=%u free8=%u block=%u alloc_hook=%s nvs_init=%s0x%x\n", unsigned(t), unsigned(f),
               unsigned(b), g_hookOk ? "ok" : "FAILED", nvs == 1 ? "-" : "", unsigned(nvs == 1 ? 1 : nvs));
    }
    if (g_evReg.load() && !g_evLogged) {
        g_evLogged = true;
        printf("[mem-mon] events=registered t_ms=%u\n", unsigned(g_evRegMs.load()));
    }
    for (int p = 0; p < kPhases; ++p) {
        Window& w = g_win[p];
        if (on[p]) {
            const bool first = !w.active;
            if (first) w = Window{true, t, kNone, kNone, 0};
            ++w.samples;
            if (f < w.min8) w.min8 = f;
            if (b < w.minBlock) w.minBlock = b;
            if (first) phaseLine(p, t, "start");
        } else if (w.active) {
            w.active = false;
            phaseLine(p, t, "end");
        }
    }
    if (f < g_low8) { g_low8 = f; g_lowBlock = b; g_lowMs = t; g_lowMask = mask; g_lowPending = true; }
    if (g_lowPending && t - g_lowLoggedMs >= 1000) {
        g_lowPending = false;
        g_lowLoggedMs = t;
        char ph[96] = "-";
        int n = 0;
        for (int p = 0; p < kPhases; ++p)
            if (g_lowMask & (1u << p)) n += snprintf(ph + n, sizeof ph - n, "%s%s", n ? "," : "", kName[p]);
        printf("[mem-low] t_ms=%u free8=%u block=%u phases=%s\n", unsigned(g_lowMs), unsigned(g_low8),
               unsigned(g_lowBlock), ph);
    }
    g_monUs += uint32_t(esp_timer_get_time() - s0);
}

int heapMonitorReport(char* out, size_t cap) {
    const uint32_t t = nowMs();
    for (int p = 0; p < kPhases; ++p)
        if (g_win[p].active) phaseLine(p, t, "active");
    const int n = snprintf(out, cap,
                           " lo8=%u loblk=%u minblk=%u minblk_t=%u fails=%u walk_us=%u mon_us=%u gap_ms=%u gaps=%u "
                           "ev_reg=%d ev_err=%d",
                           unsigned(g_low8), unsigned(g_lowBlock), unsigned(g_minBlk), unsigned(g_minBlkMs),
                           unsigned(g_fails.load()), unsigned(g_walkUsMax), unsigned(g_monUs), unsigned(g_gapMax),
                           unsigned(g_gaps), g_evReg.load() ? 1 : 0, int(g_evErr.load()));
    g_walkUsMax = 0;   // per [mem] period: one early outlier does not hide the steady cost
    g_gapMax = 0;
    g_monUs = 0;
    return n;
}
}  // namespace orcon_matter

// RF transmit marker: Orcon's txpoc backend (tx_poc_runtime.cpp) calls radio::sx1276::txBegin() across files.
extern "C" bool __real__ZN5radio6sx12767txBeginEPKhj(const uint8_t* onair, size_t n);
extern "C" bool __wrap__ZN5radio6sx12767txBeginEPKhj(const uint8_t* onair, size_t n) {
    g_txMs.store(nowMs() | 1);
    return __real__ZN5radio6sx12767txBeginEPKhj(onair, n);
}

// The first caller (Arduino's Network layer, or CHIP if it comes first) creates the loop; the handlers follow at once,
// before any Wi-Fi event can be posted. Later callers get ESP_ERR_INVALID_STATE, as without the wrap.
extern "C" esp_err_t __real_esp_event_loop_create_default();
extern "C" esp_err_t __wrap_esp_event_loop_create_default() {
    const esp_err_t e = __real_esp_event_loop_create_default();
    if (e == ESP_OK && g_started.load()) registerEvents();
    return e;
}

// Orcon's update job starts its worker as task "fw_update" (fw_update_esp.cpp). Every other task creation (also the
// early ones, before esp_timer runs) only pays one strcmp here.
extern "C" BaseType_t __real_xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name, uint32_t stack, void* arg,
                                                     UBaseType_t prio, TaskHandle_t* handle, BaseType_t core);
extern "C" BaseType_t __wrap_xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name, uint32_t stack, void* arg,
                                                     UBaseType_t prio, TaskHandle_t* handle, BaseType_t core) {
    if (name && strcmp(name, "fw_update") == 0) g_updMs.store(nowMs() | 1);
    return __real_xTaskCreatePinnedToCore(fn, name, stack, arg, prio, handle, core);
}

// Arduino's initArduino() calls nvs_flash_init() before setup() and ERASES Orcon's whole "nvs" partition when it
// returns ESP_ERR_NVS_NO_FREE_PAGES or ESP_ERR_NVS_NEW_VERSION_FOUND (esp32-hal-misc.c). The first result is kept for
// the "[mem-mon] state=start" line: the erase itself cannot be undone here, but it can no longer pass unseen.
extern "C" esp_err_t __real_nvs_flash_init();
extern "C" esp_err_t __wrap_nvs_flash_init() {
    const esp_err_t e = __real_nvs_flash_init();
    int32_t never = 1;
    g_nvsInit.compare_exchange_strong(never, e);
    return e;
}
