// QEMU memory and behaviour test of the Matter component (tools/qemu_memtest.sh, docs/MATTER.md): how the controller
// shows itself in Matter, and Matter -> Orcon control, in Espressif QEMU as a proxy for the board. The real component
// (idf/components/orcon_matter, with its placeholder identity: QEMU's isolated user network only, never a board), the
// real CHIP clusters (Basic Information, Bridged Device Basic Information, Fan Control, On/Off), Matter over the
// emulated Ethernet (no Wi-Fi). Orcon's admission and its unit are simulated (setupweb::matterControl answers like
// ctlAdmit; sim::unit is the unit's own report). Orcon's loop is loop(): serviceControl() every 10 ms, as the
// component's net::loop() hook. A "Home write" is the cluster call an Interaction Model write or command ends in
// (FanControlCluster::SetFanMode / SetPercentSetting, OnOffServer::setOnOffValue); a controller's label write goes
// through the data model provider, as an IM write does after its access check.
// Two boots on one flash: boot 1 checks the defaults and the control, then writes "a user's" labels; boot 2 (same flash,
// the probe confirmed its image) checks that the labels were kept and no default replaced them.
// Each step prints "PROBE <step> PASS|FAIL <facts>"; "PROBE heap ..." the free internal RAM; "PROBE DONE boot=N
// pass=N fail=M" ends the run.
// The identity lives on the device (orcon_matter_identity.h): a boot without one makes it, as the owner's "Matter
// activeren" does. tools/qemu_memtest.sh picks each boot's scenario with one byte in the unused middle of spiffs:
//   N (or erased) normal; I an activation cut off before its digest; D one stored byte changed; X the boot after that
//   (damaged: Matter off, the image still confirms); R the owner's new identity after damage; F Orcon's factory wipe.
// A boot that makes or keeps an identity prints its facts and certificates (throwaway QEMU identities only), so the
// runner can compare devices and boots and check the chain with CHIP's own chip-cert.
#include <Arduino.h>
#include <WebServer.h>
#include <esp_eth.h>
#include <esp_eth_mac_openeth.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <initializer_list>
#include <string.h>
#include <utility>

#include <esp_matter.h>
#include <access/SubjectDescriptor.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app/AttributeValueDecoder.h>
#include <app/InteractionModelEngine.h>
#include <app/clusters/fan-control-server/CodegenIntegration.h>
#include <app/clusters/fan-control-server/FanControlCluster.h>
#include <app/clusters/on-off-server/on-off-server.h>
#include <app/data-model-provider/OperationTypes.h>
#include <app/persistence/AttributePersistenceProviderInstance.h>
#include <lib/core/TLV.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/DeviceInstanceInfoProvider.h>

#include <esp_partition.h>
#include <esp_random.h>
#include <mbedtls/sha256.h>
#include <nvs.h>

#include "orcon_matter.h"
#include "orcon_matter_identity.h"
#include "orcon_matter_nvs.h"

extern "C" bool orcon_matter_factory_wipe(void);   // orcon_matter_nvs.cpp, Orcon's factory reset step
namespace mid = orcon_matter::identity;

using namespace chip::app::Clusters;
using FanModeEnum = FanControl::FanModeEnum;

// Stand-ins for the Orcon symbols the component refers to (no Orcon here).
namespace net { void begin() {} void loop() {} void restartSoon() {} bool restartQueued() { return false; } }
namespace ota { bool writerBusy() { return false; } void begin(WebServer&) {} }
namespace radio { namespace sx1276 { bool txBegin(const uint8_t*, size_t) { return true; } } }
namespace updsvc { bool updateBoot() { return false; } }

namespace sim {   // the simulated unit and Orcon's admission; written by the test task, read by loop()
volatile uint8_t unit = 0xFF;   // the unit's own last report (31D9), 0xFF none
volatile bool txBusy = false;   // an open RF transaction (ctlAdmit admitted; the unit has not answered yet)
volatile int calls = 0;
char lastKey[12] = "";
int codes[8];
const char* errs[8];
volatile int nScript = 0, iScript = 0;   // scripted refusals first, then admitted
void script(std::initializer_list<std::pair<int, const char*>> l) {
    int n = 0;
    for (auto& p : l) { codes[n] = p.first; errs[n] = p.second; ++n; }
    iScript = 0; nScript = n;
}
}  // namespace sim

namespace setupweb {
bool authorize(bool, bool) { return false; }   // the pairing routes are not exercised here
char* responseBuffer(size_t& cap) { static char b[3072]; cap = sizeof b; return b; }   // Orcon's shared JSON buffer
int matterControl(const char* key, const char*& e, uint32_t& waitMs) {   // as ctlAdmit, from Orcon's loop task
    ++sim::calls;
    strlcpy(sim::lastKey, key ? key : "", sizeof sim::lastKey);
    waitMs = 0;
    if (sim::iScript < sim::nScript) { e = sim::errs[sim::iScript]; return sim::codes[sim::iScript++]; }
    e = "invalid";   // ctlAdmit leaves e at "invalid" when it admits
    sim::txBusy = true;
    return 202;
}
uint8_t matterUnitMode(bool& busy) { busy = sim::txBusy; return sim::unit; }
}  // namespace setupweb

static volatile bool g_netUp = false;
static esp_netif_t* g_netif = nullptr;
static void onEth(void*, esp_event_base_t, int32_t id, void*) {
    if (id == ETHERNET_EVENT_CONNECTED) esp_netif_create_ip6_linklocal(g_netif);   // Matter needs IPv6 link-local
}
static void onGotIp(void*, esp_event_base_t, int32_t, void*) { g_netUp = true; }
static void startEthernet() {   // as ESP-IDF's protocol_examples_common with OpenETH
    ESP_ERROR_CHECK(esp_netif_init());
    const esp_err_t ev = esp_event_loop_create_default();
    if (ev != ESP_OK && ev != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(ev);
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    g_netif = esp_netif_new(&cfg);
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.autonego_timeout_ms = 100;
    esp_eth_mac_t* mac = esp_eth_mac_new_openeth(&mac_config);
    esp_eth_phy_t* phy = esp_eth_phy_new_dp83848(&phy_config);
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = nullptr;
    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth));
    ESP_ERROR_CHECK(esp_netif_attach(g_netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_CONNECTED, onEth, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, onGotIp, nullptr));
    ESP_ERROR_CHECK(esp_eth_start(eth));
}

// ---- reading and "writing like a Home app", always under CHIP's stack lock ------------------------------------------
#if CONFIG_ORCON_MATTER_BRIDGE
constexpr chip::EndpointId kAggEp = 1, kFanEp = 2;   // createModel(): root 0, Aggregator 1, bridged fan 2
#else
constexpr chip::EndpointId kFanEp = 1;               // the same fan without the bridge (RAM comparison)
#endif
struct Lock { Lock() { chip::DeviceLayer::PlatformMgr().LockChipStack(); } ~Lock() { chip::DeviceLayer::PlatformMgr().UnlockChipStack(); } };
struct Snap { int mode = -1, pct = -1, cur = -1, spd = -1, on = -1; };   // pct -2 = null
static Snap snap() {
    Snap s;
    Lock l;
    if (chip::app::Clusters::FanControlCluster* f = FanControl::FindClusterOnEndpoint(kFanEp)) {
        s.mode = int(f->GetFanMode());
        const auto p = f->GetPercentSetting();
        s.pct = p.IsNull() ? -2 : int(p.Value());
        s.cur = int(f->GetPercentCurrent());
        s.spd = int(f->GetSpeedCurrent());
    }
    esp_matter_attr_val_t v = esp_matter_invalid(nullptr);
    if (esp_matter::attribute_t* a = esp_matter::attribute::get(kFanEp, OnOff::Id, OnOff::Attributes::OnOff::Id))
        if (esp_matter::attribute::get_val(a, &v) == ESP_OK) s.on = v.val.b ? 1 : 0;
    return s;
}
static int writeFanMode(FanModeEnum m) {
    Lock l;
    chip::app::Clusters::FanControlCluster* f = FanControl::FindClusterOnEndpoint(kFanEp);
    return f ? int(f->SetFanMode(m)) : -1;
}
static int writePercent(uint8_t p) {
    Lock l;
    chip::app::Clusters::FanControlCluster* f = FanControl::FindClusterOnEndpoint(kFanEp);
    return f ? int(f->SetPercentSetting(chip::app::DataModel::MakeNullable(p))) : -1;
}
static int commandOnOff(bool on) {
    Lock l;
    return int(OnOffServer::Instance().setOnOffValue(kFanEp, on ? OnOff::Commands::On::Id : OnOff::Commands::Off::Id,
                                                       false));
}
// A controller's write of a label (after its access check), through the data model provider, as the IM write path.
static bool writeLabel(chip::EndpointId ep, chip::ClusterId cl, chip::AttributeId at, const char* text) {
    using namespace chip;
    using namespace chip::app;
    Lock l;
    uint8_t tlv[48];
    TLV::TLVWriter w;
    w.Init(tlv, sizeof tlv);
    if (w.PutString(TLV::AnonymousTag(), text) != CHIP_NO_ERROR || w.Finalize() != CHIP_NO_ERROR) return false;
    TLV::TLVReader r;
    r.Init(tlv, w.GetLengthWritten());
    if (r.Next() != CHIP_NO_ERROR) return false;
    const Access::SubjectDescriptor subject{};
    AttributeValueDecoder d(r, subject);
    DataModel::WriteAttributeRequest req(ConcreteDataAttributePath(ep, cl, at), subject);
    return InteractionModelEngine::GetInstance()->GetDataModelProvider()->WriteAttribute(req, d).IsSuccess();
}
// The root NodeLabel as CHIP stored it ("" = none stored): its persistence is a length byte + the text.
static void storedRootLabel(char* out, size_t n) {
    using namespace chip::app;
    Lock l;
    out[0] = '\0';
    uint8_t raw[48];
    chip::MutableByteSpan span(raw);
    const ConcreteAttributePath p(chip::kRootEndpointId, BasicInformation::Id, BasicInformation::Attributes::NodeLabel::Id);
    if (GetAttributePersistenceProvider()->ReadValue(p, span) != CHIP_NO_ERROR || span.size() < 1) return;
    const size_t len = raw[0] < span.size() - 1 ? raw[0] : span.size() - 1;
    snprintf(out, n, "%.*s", int(len), reinterpret_cast<const char*>(raw + 1));
}
#if CONFIG_ORCON_MATTER_BRIDGE
static void bridgedText(chip::AttributeId at, char* out, size_t n) {   // a Bridged Device Basic Information string
    Lock l;
    out[0] = '\0';
    esp_matter_attr_val_t v = esp_matter_invalid(nullptr);
    if (esp_matter::attribute_t* a = esp_matter::attribute::get(kFanEp, BridgedDeviceBasicInformation::Id, at))
        if (esp_matter::attribute::get_val(a, &v) == ESP_OK && v.val.a.b)
            snprintf(out, n, "%.*s", int(v.val.a.s), reinterpret_cast<const char*>(v.val.a.b));
}
#endif
static bool hasDeviceType(chip::EndpointId ep, uint32_t id) {
    Lock l;
    esp_matter::endpoint_t* e = esp_matter::endpoint::get(ep);
    if (!e) return false;
    for (size_t i = 0; i < esp_matter::endpoint::get_device_type_count(e); ++i) {
        uint32_t t = 0;
        uint8_t ver = 0;
        if (esp_matter::endpoint::get_device_type_at_index(e, i, t, ver) == ESP_OK && t == id) return true;
    }
    return false;
}

static int g_pass = 0, g_fail = 0;
static void check(const char* step, bool ok) {
    const Snap s = snap();
    printf("PROBE %s %s | mode=%d pct=%d cur=%d spd=%d on=%d calls=%d key=%s unit=%d busy=%d\n", step,
           ok ? "PASS" : "FAIL", s.mode, s.pct, s.cur, s.spd, s.on, sim::calls, sim::lastKey, int(sim::unit),
           int(sim::txBusy));
    ++(ok ? g_pass : g_fail);
}
static void checkText(const char* step, const char* got, const char* want) {
    const bool ok = !strcmp(got, want);
    printf("PROBE %s %s | got=\"%s\" want=\"%s\"\n", step, ok ? "PASS" : "FAIL", got, want);
    ++(ok ? g_pass : g_fail);
}
static void unitReports(uint8_t mode) { sim::unit = mode; sim::txBusy = false; }   // the unit answers: transaction done

static const char* const kUserRoot = "Mijn Orcon", * const kUserFan = "Woonkamer test";   // "a user's" labels

// ---- the identity on the device (before Matter starts: no stack lock, so no fan snapshot) ----------------------
static void checkPlain(const char* step, bool ok) {
    printf("PROBE %s %s\n", step, ok ? "PASS" : "FAIL");
    ++(ok ? g_pass : g_fail);
}
static char scenario() {   // tools/qemu_memtest.sh writes it at spiffs + 0x80000 (outside Matter's 48 KB and the sentinel)
    const esp_partition_t* sp = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    uint8_t b = 0xFF;
    if (sp) esp_partition_read(sp, 0x80000, &b, 1);
    return b == 0xFF ? 'N' : char(b);
}
// One printf (no other task's log line lands inside it), from a buffer that exists only meanwhile: before Matter
// starts, so the probe's own export never lowers what it measures.
static void printBlob(const char* name, const char* key) {
    constexpr size_t kMax = 600;
    uint8_t* b = static_cast<uint8_t*>(malloc(kMax + 2 * kMax + 1));
    if (!b) { printf("PROBE blob %s=none\n", name); return; }
    char* hex = reinterpret_cast<char*>(b + kMax);
    chip::MutableByteSpan s(b, kMax);
    if (mid::read(key, s) == CHIP_NO_ERROR) {
        for (size_t i = 0; i < s.size(); ++i) snprintf(hex + 2 * i, 3, "%02x", s.data()[i]);
        hex[2 * s.size()] = '\0';
        printf("PROBE blob %s=%s n=%u\n", name, hex, unsigned(s.size()));
    } else {
        printf("PROBE blob %s=none\n", name);
    }
    free(b);
}
static void printIdentity() {   // what identifies this device, as hashes; and its public certificates for chip-cert
    uint8_t pub[65], h[32] = {0}, q[32] = {0};
    chip::MutableByteSpan p(pub);
    if (mid::read(mid::kDacPub, p) == CHIP_NO_ERROR) mbedtls_sha256(p.data(), p.size(), h, 0);
    mbedtls_sha256(reinterpret_cast<const uint8_t*>(mid::qrCode()), strlen(mid::qrCode()), q, 0);
    printf("PROBE identity state=%s origin=%d serial=%s disc=%u dac=%02x%02x%02x%02x%02x%02x%02x%02x qr=%02x%02x%02x%02x%02x%02x\n",
           mid::stateName(), int(mid::origin()), mid::serial(), unsigned(mid::discriminator()), h[0], h[1], h[2], h[3], h[4], h[5],
           h[6], h[7], q[0], q[1], q[2], q[3], q[4], q[5]);
    printBlob("paa", mid::kPaaCert);
    printBlob("pai", mid::kPaiCert);
    printBlob("dac", mid::kDacCert);
    printBlob("cd", mid::kCd);
}
static void scenarioOnly(char sc, mid::State st) {   // the short boots: no Matter start
    nvs_handle_t h;
    switch (sc) {
        case 'I': {   // an activation cut off before its digest: some fields written, the digest never
            uint8_t junk[32];
            esp_fill_random(junk, sizeof junk);
            bool ok = nvs_open_from_partition("chipkvs", "orcon-id", NVS_READWRITE, &h) == ESP_OK;
            if (ok) {
                ok = nvs_erase_all(h) == ESP_OK && nvs_set_u8(h, "v", 1) == ESP_OK && nvs_set_blob(h, mid::kDacKey, junk, sizeof junk) == ESP_OK &&
                     nvs_commit(h) == ESP_OK;
                nvs_close(h);
            }
            checkPlain("interrupted-activation-left-behind", ok && st == mid::State::Absent);
            break;
        }
        case 'D': {   // one stored byte changes (a flash fault, or a hand): the record no longer matches its digest
            uint8_t salt[32];
            chip::MutableByteSpan s(salt);
            bool ok = st == mid::State::Valid && mid::read(mid::kSalt, s) == CHIP_NO_ERROR &&
                      nvs_open_from_partition("chipkvs", "orcon-id", NVS_READWRITE, &h) == ESP_OK;
            if (ok) {
                salt[0] ^= 1;
                ok = nvs_set_blob(h, mid::kSalt, salt, s.size()) == ESP_OK && nvs_commit(h) == ESP_OK;
                nvs_close(h);
            }
            checkPlain("one-stored-byte-changed", ok);
            break;
        }
        case 'X':   // damaged: Matter stays off, nothing replaces the record by itself, and the image still confirms
            checkPlain("damage-detected", st == mid::State::Damaged);
            checkPlain("damaged-record-not-used", !orcon_matter::identityPresent() && mid::passcode() == 0 && !mid::qrCode()[0]);
            checkPlain("damaged-image-still-confirms", esp_ota_mark_app_valid_cancel_rollback() == ESP_OK);
            break;
        case 'R':   // the owner's explicit choice after damage: a new identity (a new device for every controller)
            checkPlain("replace-starts-from-damaged", st == mid::State::Damaged);
            checkPlain("new-identity-after-damage", mid::erase() == ESP_OK && mid::generate() == ESP_OK && mid::state() == mid::State::Valid);
            printIdentity();
            break;
        case 'F':   // Orcon's factory reset step ran before NVS opened (setup())
            checkPlain("factory-wipe-leaves-no-identity", st == mid::State::Absent);
            checkPlain("unactivated-image-confirms", esp_ota_mark_app_valid_cancel_rollback() == ESP_OK);
            break;
        default:
            checkPlain("known-scenario", false);
    }
    printf("PROBE DONE boot=%c pass=%d fail=%d\n", sc, g_pass, g_fail);
}

static void identityChecks() {   // what a controller reads at commissioning (Basic Information, DN of _matterc._udp)
    char b[72];
    const auto read = [&b](CHIP_ERROR e) -> const char* { return e == CHIP_NO_ERROR ? b : "<read failed>"; };
    chip::DeviceLayer::DeviceInstanceInfoProvider* p = chip::DeviceLayer::GetDeviceInstanceInfoProvider();
    checkText("root-vendor-name", read(p->GetVendorName(b, sizeof b)), "Zelfbouw (niet van Orcon)");
    checkText("root-product-name", read(p->GetProductName(b, sizeof b)), "15RF-controller voor Orcon");
    checkText("root-serial", read(p->GetSerialNumber(b, sizeof b)), mid::serial());
    checkText("root-hardware", read(p->GetHardwareVersionString(b, sizeof b)), "Heltec WiFi LoRa 32 V2");
    checkText("dnssd-dn-name", read(chip::DeviceLayer::ConfigurationMgr().GetCommissionableDeviceName(b, sizeof b)),
              "Orcon ventilator");
    check("dnssd-dn-enabled", CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONABLE_DEVICE_NAME == 1);
    uint16_t vid = 0;   // the DAC's test vendor: no claim of a real vendor ID
    check("vendor-id-stays-test-0xFFF1", p->GetVendorId(vid) == CHIP_NO_ERROR && vid == 0xFFF1);
#if CONFIG_ORCON_MATTER_BRIDGE
    check("ep1-aggregator", hasDeviceType(kAggEp, 0x000E) && !hasDeviceType(kAggEp, 0x002B));
    check("ep2-bridged-node-and-fan", hasDeviceType(kFanEp, 0x0013) && hasDeviceType(kFanEp, 0x002B));
    bridgedText(BridgedDeviceBasicInformation::Attributes::VendorName::Id, b, sizeof b);
    checkText("bridged-vendor-name", b, "Zelfbouw (niet van Orcon)");
    bridgedText(BridgedDeviceBasicInformation::Attributes::ProductName::Id, b, sizeof b);
    checkText("bridged-product-name", b, "15RF-controller voor Orcon");
    bridgedText(BridgedDeviceBasicInformation::Attributes::SerialNumber::Id, b, sizeof b);
    checkText("bridged-serial", b, mid::serial());
    bridgedText(BridgedDeviceBasicInformation::Attributes::UniqueID::Id, b, sizeof b);
    check("bridged-unique-id", !strncmp(b, "orcon-fan-", 10) && strlen(b) == 16);
#else
    check("ep1-fan-no-bridge", hasDeviceType(kFanEp, 0x002B) && !hasDeviceType(kFanEp, 0x0013));
#endif
    check("no-outlet-endpoint", !hasDeviceType(1, 0x010A) && !hasDeviceType(2, 0x010A) && !hasDeviceType(3, 0x010A));
}

static void controlChecks() {
    Snap s = snap();
    check("fan-and-onoff-present", s.mode >= 0 && s.on >= 0 && s.spd >= 0);
    check("boot-restore-is-no-request", sim::calls == 0);
    delay(1200); s = snap();   // no report yet: the placeholder Auto (fan on), never "off", not the stale "off" either
    check("stale-off-ignored-shows-auto-placeholder", sim::calls == 0 && s.mode == 5 && s.pct == -2 && s.on == 1);

    commandOnOff(false); delay(300);   // Home: the fan off while the unit's stand is unknown = Afwezig
    check("onoff-off-is-away", sim::calls == 1 && !strcmp(sim::lastKey, "away"));
    unitReports(0); delay(1200); s = snap();
    check("away-shows-fan-off", s.mode == 0 && s.on == 0 && s.cur == 0 && sim::calls == 1);
    commandOnOff(true); delay(300);   // on again without any stand this boot: Auto, the unit's own default
    check("on-without-known-stand-goes-auto", sim::calls == 2 && !strcmp(sim::lastKey, "auto"));
    unitReports(4); delay(1200); s = snap();
    check("unit-confirms-auto", s.mode == 5 && s.on == 1 && s.pct == -2 && sim::calls == 2);

    unitReports(2); delay(1200); s = snap();   // the physical remote: stand 2
    check("mirror-shows-unit-medium", s.mode == 2 && s.pct == 66 && s.cur == 66 && s.spd == 2 && s.on == 1 &&
                                      sim::calls == 2);
    writeFanMode(FanModeEnum::kHigh); delay(300); s = snap();
    check("home-high-goes-to-admission-as-3", sim::calls == 3 && !strcmp(sim::lastKey, "3"));
    check("high-kept-while-transaction-open", s.mode == 3);
    unitReports(3); delay(1200); s = snap();
    check("unit-confirms-high", s.mode == 3 && s.pct == 100 && s.cur == 100 && s.spd == 3 && sim::calls == 3);

    writeFanMode(FanModeEnum::kOff); delay(300);   // the tile switched off (FanMode Off) = Afwezig
    check("fanmode-off-is-away", sim::calls == 4 && !strcmp(sim::lastKey, "away"));
    unitReports(0); delay(1200); s = snap();
    check("away-shows-off-and-onoff-off", s.mode == 0 && s.on == 0 && sim::calls == 4);
    commandOnOff(true); delay(300);   // on again: back to the last stand the unit reported (3)
    check("on-returns-to-last-stand-3", sim::calls == 5 && !strcmp(sim::lastKey, "3"));
    unitReports(3); delay(1200); s = snap();
    check("unit-back-on-high", s.mode == 3 && s.on == 1 && sim::calls == 5);

    writePercent(20); delay(300);
    check("percent-20-goes-to-admission-as-1", sim::calls == 6 && !strcmp(sim::lastKey, "1"));
    unitReports(1); delay(1200); s = snap();
    check("unit-confirms-low", s.mode == 1 && s.cur == 33 && s.spd == 1 && sim::calls == 6);
    writePercent(0); delay(300);   // the slider to 0 % = off = Afwezig
    check("percent-0-is-away", sim::calls == 7 && !strcmp(sim::lastKey, "away"));
    unitReports(0); delay(1200);
    check("away-again", snap().on == 0);

    writeFanMode(FanModeEnum::kAuto); delay(300);   // Home's Auto (the fan's Auto feature)
    check("home-auto-goes-to-admission-as-auto", sim::calls == 8 && !strcmp(sim::lastKey, "auto"));
    unitReports(4); delay(1200); s = snap();
    check("unit-confirms-auto-and-on", s.mode == 5 && s.on == 1 && sim::calls == 8);

    sim::script({{409, "session-busy"}, {429, "too-fast"}, {409, "remote-active"}});
    writeFanMode(FanModeEnum::kMedium); delay(2500);
    check("transient-refusals-retried-then-admitted", sim::calls == 12 && !strcmp(sim::lastKey, "2") && sim::txBusy);
    unitReports(2); delay(1200);
    check("unit-confirms-medium", snap().mode == 2);

    sim::script({{429, "tx-budget"}});
    writeFanMode(FanModeEnum::kHigh); delay(1500); s = snap();
    check("final-refusal-reverts-to-unit", sim::calls == 13 && s.mode == 2 && s.pct == 66);
    sim::script({{429, "tx-budget"}});
    commandOnOff(false); delay(1500); s = snap();   // a refused Afwezig: the fan is on again in Home
    check("refused-away-reverts-to-on", sim::calls == 14 && s.on == 1 && s.mode == 2);

    sim::script({{409, "session-busy"}, {409, "session-busy"}});
    writeFanMode(FanModeEnum::kHigh); delay(100);
    writeFanMode(FanModeEnum::kLow); delay(2000);
    check("latest-request-wins", sim::calls == 17 && !strcmp(sim::lastKey, "1") && sim::txBusy);
    unitReports(1); delay(1200);
    check("unit-confirms-low-again", snap().mode == 1);

    unitReports(4); delay(1200); s = snap();   // the physical remote: Auto
    check("remote-change-followed-without-request", s.mode == 5 && s.pct == -2 && s.on == 1 && sim::calls == 17);
    unitReports(6); delay(1200);                // the 15RF timer (boost) shows as High
    check("timer-shows-high", snap().mode == 3 && sim::calls == 17);
    unitReports(0xFF); delay(1200);
    check("unknown-again-keeps-view", snap().mode == 3 && sim::calls == 17);
}

static void testTask(void*) {
    for (int i = 0; i < 3000 && !orcon_matter::matterReady(); ++i) delay(10);
    const uint32_t readyMs = millis();
    // The first 1.5 s after ready, every 25 ms: how FanMode and the fan's On/Off evolve (printed afterwards, so the
    // sampling itself does not shift the order of events it observes).
    static int8_t sm[60], so[60];
    for (int i = 0; i < 60; ++i) { const Snap s = snap(); sm[i] = int8_t(s.mode); so[i] = int8_t(s.on); delay(25); }
    printf("PROBE matter=%s ready_ms=%lu\nPROBE early mode/on every 25 ms:", orcon_matter::matterStateName(),
           (unsigned long)readyMs);
    for (int i = 0; i < 60; ++i) printf(" %d/%d", sm[i], so[i]);
    char fc[160];
    orcon_matter::fanCheckJson(fc, sizeof fc);
    printf("\nPROBE fan-check=%s\n", fc);
    delay(500);
    printf("PROBE heap ready_free=%u ready_min=%u largest=%u\n", unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    const esp_err_t valid = esp_ota_mark_app_valid_cancel_rollback();   // so that boot 2 starts this image again
    printf("PROBE image-confirmed=%s\n", esp_err_to_name(valid));

    char b[48];
    storedRootLabel(b, sizeof b);
    int boot = 1;
#if CONFIG_ORCON_MATTER_BRIDGE
    char fanLabel[48];
    bridgedText(BridgedDeviceBasicInformation::Attributes::NodeLabel::Id, fanLabel, sizeof fanLabel);
    if (!strcmp(fanLabel, kUserFan)) boot = 2;
#else
    if (!strcmp(b, kUserRoot)) boot = 2;
#endif
    if (boot == 2) {   // the same flash after a restart: the labels a user chose are kept, no default replaced them
        checkText("root-label-kept-after-restart", b, kUserRoot);
#if CONFIG_ORCON_MATTER_BRIDGE
        checkText("bridged-label-kept-after-restart", fanLabel, kUserFan);
#endif
        { Lock l; checkText("default-not-reapplied", orcon_matter::applyDefaultNodeLabel(), "kept"); }
        identityChecks();
    } else {
        checkText("root-label-default-written-once", b, "Orcon ventilator");
        { Lock l; checkText("root-label-default-not-rewritten", orcon_matter::applyDefaultNodeLabel(), "kept"); }
#if CONFIG_ORCON_MATTER_BRIDGE
        checkText("bridged-label-default", fanLabel, "Orcon ventilator");
#endif
        identityChecks();
        controlChecks();
        // "A user renames it": what a controller's write leaves, kept for boot 2.
        check("user-root-label-written", writeLabel(chip::kRootEndpointId, BasicInformation::Id,
                                                    BasicInformation::Attributes::NodeLabel::Id, kUserRoot));
#if CONFIG_ORCON_MATTER_BRIDGE
        check("user-bridged-label-written", writeLabel(kFanEp, BridgedDeviceBasicInformation::Id,
                                                       BridgedDeviceBasicInformation::Attributes::NodeLabel::Id, kUserFan));
#endif
        storedRootLabel(b, sizeof b);
        checkText("user-root-label-stored", b, kUserRoot);
    }
    char j[256];
    orcon_matter::controlJson(j, sizeof j);
    printf("PROBE control=%s\n", j);
    printf("PROBE heap end_free=%u end_min=%u\n", unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    printf("PROBE DONE boot=%d pass=%d fail=%d\n", boot, g_pass, g_fail);
    vTaskDelete(nullptr);
}

void setup() {
    Serial.begin(115200);
    startEthernet();
    for (int i = 0; i < 300 && !g_netUp; ++i) delay(100);
    printf("PROBE net=%s\n", g_netUp ? "up" : "DOWN");
    const unsigned before = unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    const char sc = scenario();
    printf("PROBE scenario=%c\n", sc);
    if (sc == 'F') checkPlain("factory-wipe", orcon_matter_factory_wipe());   // as provision::resume() at boot
    esp_err_t err = orcon_matter::matterNvsBegin();
    const mid::State st = err == ESP_OK ? mid::load() : mid::State::Unknown;
    printf("PROBE identity-loaded=%s\n", mid::stateName());
    if (sc != 'N') { scenarioOnly(sc, st); return; }
    if (st == mid::State::Absent && mid::embeddedPresent()) {   // a migration build: its identity moves onto the device
        const esp_err_t m = mid::migrateEmbedded();
        printf("PROBE migrate=%s\n", esp_err_to_name(m));
        checkPlain("identity-moved-onto-device", m == ESP_OK && mid::state() == mid::State::Valid && mid::origin() == mid::Origin::Migrated);
        checkPlain("codes-equal-the-embedded-ones", mid::embeddedCodesMatch());
    } else if (st == mid::State::Absent) {   // the owner's "Matter activeren"
        const esp_err_t g = mid::generate();
        const mid::GenStats s = mid::lastGenStats();
        printf("PROBE generate=%s ms=%u heap_before=%u heap_after=%u heap_min=%u\n", esp_err_to_name(g), unsigned(s.ms),
               unsigned(s.heapBefore), unsigned(s.heapAfter), unsigned(s.heapMinAfter));
        checkPlain("identity-made-on-device", g == ESP_OK && mid::state() == mid::State::Valid && mid::origin() == mid::Origin::Generated);
    } else {
        checkPlain("identity-kept", st == mid::State::Valid);
    }
    printIdentity();
    checkPlain("codes-from-identity", strlen(orcon_matter::manualCode()) == 11 && !strncmp(orcon_matter::qrCode(), "MT:", 3));
    checkPlain("confirm-refused-before-matter-runs", esp_ota_mark_app_valid_cancel_rollback() == ESP_ERR_INVALID_STATE);
    if (err == ESP_OK && mid::state() != mid::State::Valid) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) err = orcon_matter::createModel();
    const unsigned model = unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    // Boot 1: a stale "off" in the fan's On/Off, as the first layout's outlet on endpoint 2 leaves it in NVS (there
    // "off" meant "not away"). It must not show as Afwezig: the fan never stored a FanMode, so it is new. Boot 2 keeps
    // its state.
    bool seeded = false;
    if (esp_matter::attribute_t* on = esp_matter::attribute::get(kFanEp, OnOff::Id, OnOff::Attributes::OnOff::Id)) {
        bool boot1 = true;
#if CONFIG_ORCON_MATTER_BRIDGE
        esp_matter_attr_val_t lbl = esp_matter_invalid(nullptr);   // NONVOLATILE: already loaded at create
        esp_matter::attribute::get_val(kFanEp, BridgedDeviceBasicInformation::Id,
                                       BridgedDeviceBasicInformation::Attributes::NodeLabel::Id, &lbl);
        boot1 = !(lbl.val.a.b && lbl.val.a.s == strlen(kUserFan) && !memcmp(lbl.val.a.b, kUserFan, lbl.val.a.s));
#endif
        if (boot1) {
            esp_matter_attr_val_t off = esp_matter_bool(false);
            seeded = esp_matter::attribute::set_val(on, &off, false) == ESP_OK;
        }
    }
    printf("PROBE stale-onoff-seeded=%s\n", seeded ? "yes" : "no");
    orcon_matter::probeStarting();
    if (err == ESP_OK) { orcon_matter::setCredentialProviders(); err = esp_matter::start(orcon_matter::onChipEvent); }   // as begin()
    orcon_matter::probeHandedOver();
    printf("PROBE matter-start=%s heap before_model=%u after_model=%u model_cost=%d\n", esp_err_to_name(err), before,
           model, int(before) - int(model));
    xTaskCreatePinnedToCore(testTask, "probe", 6144, nullptr, 1, nullptr, 0);
}

void loop() {   // Orcon's loop task. What the component's net::loop() hook does each pass (a --wrap never applies to a
    orcon_matter::serviceControl(millis());   // call inside the defining object, so the probe calls it directly)
    delay(10);
}
