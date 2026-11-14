// test/test_setup_web.cpp — the real setup_web.cpp + orcon.cpp behind a fake WebServer and fake
// radio. Built three ways: -DRX_ONLY=1 (Heltec V2 monitor), -DRX_ONLY=0 (TX builds), and
// -DORCON_TX_POC=1 -DORCON_WEB_CONTROL=1 -DRX_ONLY=1 (Heltec V2 web control on the txpoc layer).
// Covers: page/gzip headers, capabilities, POST guard (token, origin, content type, size,
// unknown fields), input validation, session conflicts, GET without side effects, RX
// pipeline integration, pairing statuses, virtual remote statuses and RX-only refusals.
#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "config.h"
#include "setup_web.h"
#include "remote_profiles.h"
#include "setup_flow.h"
#include "setup_page.h"
#include "orcon.h"
#include "orcon_log.h"
#include "ramses_radio.h"
#include "net.h"
#if ORCON_TX_POC
#include "tx_poc.h"
#include "tx_poc_runtime.h"
#endif
#include <map>
#include <string>
#include <vector>
using namespace ramses;
#ifndef PROVISION_SCENARIO
#define PROVISION_SCENARIO 0   // 1: a new device end to end; 2: an unfinished installation, password unreadable;
                               // 3: writes that persist but fail their read-back
#endif

static unsigned long g_ms = 100000;
unsigned long millis() { return g_ms; }
void delay(unsigned long ms) { g_ms += ms; }
static std::map<uint16_t, int> g_sent;       // frames the fake radio transmitted, per code
static bool g_radioOk = true;
static bool g_apModeFake = false;
static uint32_t g_syncs = 0;
static uint8_t g_rxState = 0;
static std::vector<radio::SyncInfo> g_notes;  // what the RX task would have queued
static uint32_t g_noteDrops = 0, g_ovf = 0, g_frames = 0;
namespace radio {
bool pollSync(SyncInfo& o) { if (g_notes.empty()) return false; o = g_notes.front(); g_notes.erase(g_notes.begin()); return true; }
uint32_t syncInfoDrops() { return g_noteDrops; }
bool sendFrame(const Frame& f) { if (RX_ONLY || !g_radioOk) return false; ++g_sent[f.cmd]; return true; }
float lastRssi() { return -61; }
RxDiagnostics rxDiagnostics() {
    RxDiagnostics d; d.coreSyncHits = g_syncs; d.rxState = g_rxState; d.uartOverflows = g_ovf; d.frames = g_frames; d.rssiNow = -101;
    return d;
}
}
#if ORCON_TX_POC
namespace radio { uint32_t lastReceivedMs() { return g_ms; } }   // rx() delivers a frame at g_ms
#endif
#if ORCON_WEB_AUTH
// Device side of the login (auth_store.cpp): NVS, PBKDF2, RNG and the OTA signature, faked.
#include "auth_store.h"
static webauth::Cred g_store;
static bool g_storeSet = false, g_kdfOk = true, g_storeUnreadable = PROVISION_SCENARIO == 2, g_saveAmbiguous = false,
            g_saveUnreadable = false;   // persisted, and from then on unreadable
static uint32_t g_rng = 0x2468ACE1u;
namespace authstore {
Status status(webauth::Cred& c) { if (g_storeUnreadable) return Status::Unreadable; if (!g_storeSet) return Status::Absent; c = g_store; return Status::Ok; }
bool load(webauth::Cred& c) { return status(c) == Status::Ok; }
bool save(const webauth::Cred& c) { g_store = c; g_storeSet = true; g_storeUnreadable = g_saveUnreadable; return !g_saveAmbiguous && !g_saveUnreadable; }   // ambiguous: persisted, read-back failed
bool kdfReady() { return g_kdfOk; }
bool kdf(const char* pw, size_t n, const uint8_t* salt, size_t sn, uint32_t it, uint8_t out[32]) {
    uint32_t h = 2166136261u ^ it;
    for (int r = 0; r < 32; ++r) {
        for (size_t i = 0; i < n; ++i) h = (h ^ uint8_t(pw[i])) * 16777619u;
        for (size_t i = 0; i < sn; ++i) h = (h ^ salt[i]) * 16777619u;
        h = (h ^ uint32_t(r)) * 16777619u; out[r] = uint8_t(h >> 13);
    }
    return g_kdfOk;
}
void random(uint8_t* b, size_t n) { for (size_t i = 0; i < n; ++i) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; b[i] = uint8_t(g_rng >> 24); } }
void ownMac(char out[18]) { strcpy(out, "02:00:00:00:00:0a"); }
bool verifySignature(const char*, size_t, const char* sig) { return !strcmp(sig, "a1b2"); }   // "a1b2": a valid signature
}
#endif
#if ORCON_TX_RF_ENABLE
static uint32_t g_restoreDiag = 0;
namespace radio { namespace sx1276 { uint32_t restoreDiag() { return g_restoreDiag; } } }
#endif
static bool g_wifiFake = true;
static int g_restarts = 0, g_forgets = 0;
#if ORCON_OWNER_MODEL
// Ownership model: the device side (owner_store.cpp, net.cpp) faked. The key-file code is the real
// owner_key.cpp; its primitives here are deterministic stand-ins (OpenSSL checks the real format in
// test_owner_key). The fake device starts migrated (Legacy), so the older tests see the old rules;
// the provisioning scenario (-DPROVISION_SCENARIO=1) starts as a new device.
#include "owner_key.h"
#include "owner_store.h"
#include "provision.h"
static provision::State g_ownState = PROVISION_SCENARIO == 1 || PROVISION_SCENARIO == 3 ? provision::State::New :
                                     PROVISION_SCENARIO == 2 ? provision::State::Setup : provision::State::Legacy;
static bool g_registerFails = false;
static bool g_markFails = false;     // markSetup refuses: a write error, or a pending factory reset (Codex OWNER-13)
static bool g_viaAp = false, g_staUp = false, g_apOnly = false, g_physical = false, g_cryptoOk = true;
static uint8_t g_ownPub[65]; static bool g_ownPubSet = false;
static std::string g_wtState = "idle", g_wtSsid, g_wtPass;
static int g_wtStarts = 0, g_identityMade = 0;
static bool fpbkdf2(const uint8_t* pw, size_t n, const uint8_t* s, size_t sn, uint32_t it, uint8_t out[32]) {
    uint32_t h = 2166136261u ^ it;
    for (int r = 0; r < 32; ++r) {
        for (size_t i = 0; i < n; ++i) h = (h ^ pw[i]) * 16777619u;
        for (size_t i = 0; i < sn; ++i) h = (h ^ s[i]) * 16777619u;
        h = (h ^ uint32_t(r)) * 16777619u; out[r] = uint8_t(h >> 11);
    }
    return true;
}
static bool faes(bool, const uint8_t key[32], const uint8_t iv[16], const uint8_t* in, size_t n, uint8_t* out) {
    for (size_t i = 0; i < n; ++i) out[i] = uint8_t(in[i] ^ key[i % 32] ^ iv[i % 16] ^ uint8_t(i * 7));   // invertible stand-in
    return n % 16 == 0;
}
static bool fpub(const uint8_t d[32], uint8_t q[65]) {
    uint8_t any = 0; for (int i = 0; i < 32; ++i) any |= d[i];
    if (!any) return false;
    q[0] = 4; for (int i = 0; i < 32; ++i) { q[1 + i] = uint8_t(d[i] ^ 0x5a); q[33 + i] = d[31 - i]; }
    return true;
}
static void fsha(const uint8_t* in, size_t n, uint8_t out[32]) { fpbkdf2(in, n, nullptr, 0, 1, out); }
static void frnd(uint8_t* o, size_t n) { authstore::random(o, n); }
namespace ownerstore {
static const ownerkey::Crypto C{fpbkdf2, faes, frnd, fpub, fsha};
provision::State state() { return g_ownState; }
bool markSetup() { if (g_markFails || !provision::installing(g_ownState)) return false; g_ownState = provision::State::Setup; return true; }
bool ownerKey(uint8_t q[65]) { if (g_ownPubSet) memcpy(q, g_ownPub, 65); return g_ownPubSet; }
bool registerKey(const uint8_t q[65]) { if (g_registerFails) return false; memcpy(g_ownPub, q, 65); g_ownPubSet = true; g_ownState = provision::State::Done; return true; }
const ownerkey::Crypto& crypto() { return C; }
bool cryptoReady() { return g_cryptoOk; }
bool generate(uint8_t d[32], uint8_t q[65]) { frnd(d, 32); d[0] |= 1; return fpub(d, q); }
bool physicalActive() { return g_physical; }
uint32_t physicalLeftMs() { return g_physical ? 600000 : 0; }
}
#endif
namespace net { bool apMode() { return g_apModeFake; } bool maskIds() { return true; } const char* ipAddr() { return "192.0.2.5"; }
                bool wifiConnected() { return g_wifiFake && !g_apModeFake; } const char* ssidName() { return "Thuis \"5G\"\x01"; }
                void restartSoon() { ++g_restarts; } void forgetWifi() { ++g_forgets; }
#if ORCON_OWNER_MODEL
                bool fromSetupAp() { return g_viaAp; } bool staUp() { return g_staUp; } bool apOnly() { return g_apOnly; }
                bool wifiTestStart(const char* s, const char* pw) { if (g_wtState == "testing") return false; g_wtSsid = s; g_wtPass = pw; g_wtState = "testing"; ++g_wtStarts; return true; }
                const char* wifiTestState() { return g_wtState.c_str(); } const char* wifiTestIp() { return "192.0.2.77"; }
                const char* storedSsid() { return "Thuis"; } bool ensureOwnIdentity() { ++g_identityMade; return true; }
#endif
}

#if ORCON_TX_POC
// Test-side stand-in for tx_poc_runtime.cpp: the real Controller/Owner with a fake RF
// backend. loopTick() below plays the radio-owner role; "onair" emulates the RF build.
namespace txpoc {
static Exchange s_ex;
static Controller s_ctl(s_ex);
struct FakeRf : Backend {
    int sent[3] = {0, 0, 0};                    // prepared frames per FrameKind
    bool healthy = true, onair = false;
    uint32_t started = 0;
    bool prepare(const Request& r, size_t& bytes) override {
        if (r.kind > (uint8_t)FrameKind::BindAck) return false;
        if (r.kind == (uint8_t)FrameKind::Mode) { Frame f; if (!makeModeCmd(r.ids, r.input.mode, r.tail, f)) return false; }
        sent[r.kind]++; bytes = 50; return true;
    }
    bool start(uint32_t now) override { started = now; return true; }
    Completion poll(uint32_t) override { return Completion::Complete; }  // the test clock is manual
    bool cleanup() override { return true; }
    bool restoreRx() override { return healthy; }
    bool onAir() const override { return onair; }
};
static FakeRf s_rf;
static Owner s_owner(s_ex, s_rf);
Controller& controller() { return s_ctl; }
bool rxReady() { return g_radioOk && s_rf.healthy; }
bool ownerBusy() { return s_owner.step() != Step::Idle; }
static uint32_t g_rfStarts = 0;                 // RF packets started this boot (set by the test)
uint32_t rfTxStarts() { return g_rfStarts; }
static uint32_t g_budgetWait = 0;                // RF budget (tx_poc_runtime.cpp), set by the test
static BudgetView g_budgetView;
uint32_t budgetWaitMs(uint8_t, size_t) { return g_budgetWait; }
BudgetView budget() { return g_budgetView; }
uint32_t lastAirUs() { return 11234; }
uint32_t lastTxStartMs() { return s_rf.started; }
}
#endif

static int failures = 0;
#define CHECK(c,m) do{ if(!(c)){printf("  [FAIL] %s (line %d)\n",m,__LINE__);failures++;} else printf("  [ ok ] %s\n",m);}while(0)

static WebServer srv;
static JsonDocument J;
static const char* TOKEN = "a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5";   // the per-boot token; the session's CSRF token after login
static std::string g_cookie, g_setCookie, g_csrf;                 // the browser's cookie jar
static const DeviceId REM(29, 101), REM2(29, 303), UNIT(29, 202), US(37, 404), UNIT32(32, 202);

// One request through the registered route. Returns the status code; J holds the JSON body.
static int call(const char* method, const char* path, const char* body = "{}", const char* token = TOKEN,
                const char* type = "application/json", const char* origin = "http://192.0.2.5") {
    srv.req.clear(); srv.sent.clear(); srv.out.clear(); srv.args.clear(); srv.code = 0;
    srv.host = "192.0.2.5"; srv.body = body;
    std::string p = path;                                   // "?a=b&c=d" -> args
    if (size_t q = p.find('?'); q != std::string::npos) {
        std::string qs = p.substr(q + 1); p.resize(q);
        for (size_t i = 0; i <= qs.size();) {
            size_t e = qs.find('&', i); if (e == std::string::npos) e = qs.size();
            std::string kv = qs.substr(i, e - i); size_t eq = kv.find('=');
            if (!kv.empty()) srv.args[kv.substr(0, eq)] = eq == std::string::npos ? "" : kv.substr(eq + 1);
            i = e + 1;
        }
    }
#if ORCON_TX_POC
    if (!strcmp(path, "/api/remote/send") && !strstr(body, "request") && srv.body.length() > 1) {
        std::string sequenced(body);
        sequenced.pop_back();
        sequenced += ",\"request\":" + std::to_string(txpoc::controller().status().nextRequest) + "}";
        srv.body = sequenced.c_str();
    }
#endif
    if (token) srv.req["X-Orcon-Token"] = token;
    if (type) srv.req["Content-Type"] = type;
    if (origin) srv.req["Origin"] = origin;
    if (!g_cookie.empty()) srv.req["Cookie"] = "theme=light; " + g_cookie;
    auto it = srv.routes.find(std::string(method) + " " + p);
    if (it == srv.routes.end()) return 404;
    it->second();
    g_setCookie.clear();
    if (auto sc = srv.sent.find("Set-Cookie"); sc != srv.sent.end()) {   // what a browser stores
        g_setCookie = sc->second;
        const std::string v = g_setCookie.substr(0, g_setCookie.find(';'));
        g_cookie = v == "orcon_sid=" ? "" : v;
    }
    J.clear();
    if (srv.type == "application/json") deserializeJson(J, srv.out);
    return srv.code;
}
static bool err(const char* e) { return J["error"] == e; }

static Frame fr(MsgType t, DeviceId src, DeviceId dst, uint16_t cmd, std::initializer_list<uint8_t> pl) {
    Frame f; f.type = t; f.addr[0] = src; f.addr[1] = dst; f.has_addr[2] = false; f.cmd = cmd;
    f.len = uint8_t(pl.size()); uint8_t i = 0; for (uint8_t b : pl) f.payload[i++] = b;
    return f;
}
// What main.cpp does for every decoded frame: orcon first, then the wizard (and the
// txpoc controller on web-control builds), same order. The RX task queues a sync note,
// then the decoded frame; main.cpp drains frames in order.
static void rxAfter(const Frame& f, uint32_t dtMs) {     // a frame dtMs after the previous event
    g_ms += dtMs; ++g_syncs; ++g_frames;
    radio::SyncInfo si; si.seq = g_syncs; si.ms = g_ms; si.rssi = -61; si.pre = 0x55; g_notes.push_back(si);
    orcon::onFrame(f); setupweb::onFrame(f, g_syncs, -61);
#if ORCON_TX_POC
    txpoc::controller().onFrame(f, g_ms, g_ms, orcon::ids());
#endif
}
static void rx(const Frame& f) { rxAfter(f, 400); }
static void garbled(int16_t rssi) {                       // a sync whose frame never decodes
    g_ms += 400; ++g_syncs;
    radio::SyncInfo si; si.seq = g_syncs; si.ms = g_ms; si.rssi = rssi; si.pre = 0xFF; g_notes.push_back(si);
}
// Reads every console event after *cur (following "more"), as "KIND|message" lines.
static std::string console(uint32_t* cur) {
    std::string all;
    for (int i = 0; i < 100; i++) {
        char q[64]; snprintf(q, sizeof(q), "/api/setup/console?after=%u", unsigned(*cur));
        if (call("GET", q) != 200) return all + "HTTP-ERROR";
        for (JsonVariant e : J["ev"].as<JsonArray>()) all += std::string(e["k"].as<const char*>()) + "|" + e["m"].as<const char*>() + "\n";
        *cur = J["next"].as<uint32_t>() - 1;
        if (!J["more"].as<bool>()) break;
    }
    return all;
}
static bool has(const std::string& s, const char* x) { return s.find(x) != std::string::npos; }
static void loopTick() {
#if ORCON_TX_POC
    // One call = one main-loop pass, but a txpoc transaction needs the owner to walk
    // Prepare..Publish; run a small burst like successive loop iterations would.
    for (int i = 0; i < 8; i++) { txpoc::s_owner.tick(g_ms, txpoc::s_rf.healthy); txpoc::controller().tick(g_ms); }
#endif
    setupweb::tick(g_ms);
}
static Frame press(uint8_t mode) { return fr(MsgType::I, REM, UNIT, 0x22F1, {0x00, mode, 0x04}); }

struct ProfileMemory : profiles::Storage {
    std::vector<uint8_t> blob;bool failWrite=false;
    bool read(uint8_t* out,size_t cap,size_t& n)override{n=blob.size();if(n>cap)return false;if(n)memcpy(out,blob.data(),n);return true;}
    bool write(const uint8_t* in,size_t n)override{if(failWrite)return false;blob.assign(in,in+n);return true;}
};
static ProfileMemory profileMemory;
#if ORCON_OWNER_MODEL && PROVISION_SCENARIO == 2
// Codex OWNER-02: an unfinished installation whose stored password cannot be read. Unreadable is
// never "no password": the first-password step stays closed (no takeover by another AP client).
static int unreadableScenario() {
    printf("Provisioning scenario: unfinished installation, stored password unreadable\n");
    g_apModeFake = true; g_apOnly = true; g_viaAp = true;
    CHECK(call("GET", "/api/setup/state", "", nullptr) == 200 && J["auth"]["password_set"] == true && J["setup"]["state"] == "setup",
          "an unreadable password still counts as set");
    CHECK(call("POST", "/api/provision/password", "{\"password\":\"overname-poging\"}", nullptr) == 403 && err("password-already-set") &&
          g_storeUnreadable, "no first-password step over an unreadable password (no takeover)");
    CHECK(call("POST", "/api/auth/login", "{\"password\":\"overname-poging\"}", nullptr) == 503 && err("auth-unavailable"),
          "login reports unavailable, not 'no password set'");
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
#endif
#if ORCON_OWNER_MODEL && PROVISION_SCENARIO == 3
// Codex OWNER-06/07: writes that persist but fail their read-back. An ambiguous first password closes
// step 1 (only a confirmed absence keeps it open); a key registration that fails half-way still has
// killed every release-key code issued before it.
static int ambiguousScenario() {
    printf("Provisioning scenario: writes that persist but fail their read-back\n");
    g_apModeFake = true; g_apOnly = true; g_viaAp = true;
    g_saveAmbiguous = true;
    CHECK(call("POST", "/api/provision/password", "{\"password\":\"eerste-wachtwoord\"}", nullptr) == 503 && err("auth-write-failed") && g_storeSet,
          "the first password persisted but its read-back failed: 503");
    g_cookie.clear(); g_saveAmbiguous = false;
    CHECK(call("POST", "/api/provision/password", "{\"password\":\"overname-poging\"}", nullptr) == 403 && err("password-already-set"),
          "no second first password after an ambiguous write (OWNER-06)");
    CHECK(call("POST", "/api/auth/login", "{\"password\":\"overname-poging\"}", nullptr) == 401, "the takeover password was never stored");
    CHECK(call("POST", "/api/auth/login", "{\"password\":\"eerste-wachtwoord\"}", nullptr) == 200, "the stored password works (RAM mirrors storage)");
    const std::string tok = J["token"] | "";
    g_ownState = provision::State::Legacy; g_physical = true;           // a migrated device in the recovery mode
    call("POST", "/api/auth/nonce", "{}", nullptr, "application/json", nullptr);
    const std::string n = J["nonce"] | "";
    JsonDocument d; d["manifest"] = "{\"v\":1,\"kind\":\"orcon-auth-reset\",\"mac\":\"02:00:00:00:00:0a\",\"nonce\":\"" + n + "\"}"; d["sig"] = "a1b2";
    std::string b; serializeJson(d, b);
    call("POST", "/api/auth/reset", b.c_str(), nullptr, "application/json", nullptr);
    const std::string stale = J["code"] | "";
    CHECK(stale.size() == 9, "Legacy: a release-key code is issued");
    CHECK(call("POST", "/api/provision/key", "{\"passphrase\":\"eigen sleutelzin\"}", tok.c_str()) == 200, "recovery mode: the owner makes a key");
    call("GET", "/api/provision/key/file", "", nullptr);
    JsonDocument v; v["pem"] = J["pem"].as<const char*>(); v["passphrase"] = "eigen sleutelzin";
    std::string vb; serializeJson(v, vb);
    g_registerFails = true;
    CHECK(call("POST", "/api/provision/key/verify", vb.c_str(), tok.c_str()) == 503 && err("key-write-failed"), "the registration write fails half-way");
    g_cookie.clear();
    CHECK(call("POST", "/api/auth/setup", ("{\"code\":\"" + stale + "\",\"password\":\"aanvaller-wachtw\"}").c_str(), nullptr) == 409 &&
          err("code-required"), "the earlier release-key code is gone anyway (OWNER-07)");
    CHECK(call("POST", "/api/auth/login", "{\"password\":\"aanvaller-wachtw\"}", nullptr) == 401, "the attacker's password never took effect");
    // OWNER-08: a password change that persists while storage turns unreadable: the old password and every
    // old session stop working at once (no cached credential, no surviving session).
    CHECK(call("POST", "/api/auth/login", "{\"password\":\"eerste-wachtwoord\"}", nullptr) == 200, "logged in again");
    const std::string t2 = J["token"] | "";
    g_saveUnreadable = true;
    CHECK(call("POST", "/api/auth/password", "{\"current\":\"eerste-wachtwoord\",\"password\":\"nieuw-wachtwoord\"}", t2.c_str()) == 503 &&
          err("auth-write-failed"), "the change persisted but storage reads back unreadable: 503");
    CHECK(call("GET", "/api/profiles", "", nullptr) == 401, "the old session no longer authorises anything (OWNER-08)");
    g_cookie.clear();
    CHECK(call("POST", "/api/auth/login", "{\"password\":\"eerste-wachtwoord\"}", nullptr) == 503 && err("auth-unavailable"),
          "the old password no longer logs in (nothing cached)");
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
#endif
#if ORCON_OWNER_MODEL && PROVISION_SCENARIO == 1
static std::string jstr(const char* k) { return J[k].is<const char*>() ? J[k].as<const char*>() : ""; }
static std::string body(std::initializer_list<std::pair<const char*, std::string>> kv) {
    JsonDocument d; for (auto& p : kv) d[p.first] = p.second;
    std::string o; serializeJson(d, o); return o;
}
// A new device from first start to installed, then password recovery, a new key and the refusals:
// the wizard over the setup AP only, never from the home network, never with the station up.
static int provisionScenario() {
    printf("Provisioning scenario: a new device (owner model)\n");
    g_apModeFake = true; g_apOnly = true; g_viaAp = true; g_wifiFake = false;
    CHECK(call("GET", "/api/setup/state", "", nullptr) == 200 && J["setup"]["state"] == "new" && J["setup"]["via_ap"] == true &&
          J["auth"]["password_set"] == false, "first start: the page learns it is a new device on the setup AP");
    CHECK(call("GET", "/api/provision/state", "", nullptr) == 200 && J["state"] == "new" && J["password_set"] == false &&
          J["logged_in"] == false && J["key"]["registered"] == false && J["wifi"]["ssid"] == "", "wizard state: nothing set yet, no network name");
    // The home network never reaches the wizard; nor does the AP while the station is up.
    g_viaAp = false;
    CHECK(call("GET", "/api/provision/state", "", nullptr) == 403 && err("setup-ap-only"), "home network: wizard state refused");
    CHECK(call("POST", "/api/provision/password", "{\"password\":\"eerste-wachtwoord\"}", nullptr) == 403 && err("setup-ap-only") && !g_storeSet,
          "home network: no first password");
    g_viaAp = true; g_staUp = true;
    CHECK(call("POST", "/api/provision/password", "{\"password\":\"eerste-wachtwoord\"}", nullptr) == 403 && err("wifi-test-running") && !g_storeSet,
          "station up: no first password (no path to the home network at that moment)");
    g_staUp = false;
    // Step 1: the first password.
    CHECK(call("POST", "/api/provision/password", "{\"password\":\"kort\"}", nullptr) == 400 && err("password-weak") &&
          g_ownState == provision::State::New, "weak password: refused, nothing stored");
    CHECK(call("POST", "/api/provision/password", "{\"password\":\"eerste-wachtwoord\"}", nullptr, "text/plain") == 403 && !g_storeSet,
          "step 1 needs JSON (no cross-site form)");
    CHECK(call("POST", "/api/provision/password", "{\"password\":\"eerste-wachtwoord\"}", nullptr, "application/json", "http://203.0.113.9") == 403 &&
          !g_storeSet, "step 1 refuses a foreign origin");
    g_markFails = true;
    CHECK(call("POST", "/api/provision/password", "{\"password\":\"eerste-wachtwoord\"}", nullptr) == 503 && err("state-write-failed") &&
          !g_storeSet && g_ownState == provision::State::New, "step 1: state not recorded (or a reset pending): no password stored");
    g_markFails = false;
    CHECK(call("POST", "/api/provision/password", "{\"password\":\"eerste-wachtwoord\"}", nullptr) == 200 && !g_cookie.empty() &&
          g_storeSet && g_ownState == provision::State::Setup, "step 1: password set, state Setup, logged in");
    const std::string tok = jstr("token");
    CHECK(call("POST", "/api/provision/password", "{\"password\":\"tweede-wachtwoord\"}", tok.c_str()) == 403 && err("password-already-set"),
          "never a second first password, not even logged in");
    g_cookie.clear();
    CHECK(call("POST", "/api/provision/password", "{\"password\":\"tweede-wachtwoord\"}", nullptr) == 403 && err("password-already-set"),
          "another phone on the AP cannot take over after step 1");
    // An interrupted wizard: without the session nothing else works; the login brings it back.
    CHECK(call("POST", "/api/provision/wifi", "{\"ssid\":\"Thuis\",\"password\":\"wifi-wachtwoord\"}", nullptr) == 401 && err("login-required") &&
          g_wtStarts == 0, "no session: no WiFi test");
    CHECK(call("POST", "/api/provision/key", "{\"passphrase\":\"mijn sleutelzin 1\"}", nullptr) == 401 && err("login-required"),
          "no session: no key");
    CHECK(call("POST", "/api/auth/login", "{\"password\":\"eerste-wachtwoord\"}", nullptr) == 200, "interrupted wizard: log in again with the step-1 password");
    const std::string t2 = jstr("token");
    // Step 2: WiFi, tested before it is stored; the AP stays up.
    CHECK(call("POST", "/api/provision/wifi", "{\"ssid\":\"\",\"password\":\"wifi-wachtwoord\"}", t2.c_str()) == 400 && err("bad-ssid"), "empty SSID refused");
    CHECK(call("POST", "/api/provision/wifi", "{\"ssid\":\"Thuis\",\"password\":\"kort\"}", t2.c_str()) == 400 && err("bad-wifi-password"),
          "WPA2 password shorter than 8 refused");
    CHECK(call("POST", "/api/provision/wifi", "{\"ssid\":\"Thuis\",\"password\":\"wifi-wachtwoord\"}", "wrong") == 401 && g_wtStarts == 0,
          "WiFi test needs the CSRF token");
    CHECK(call("POST", "/api/provision/wifi", body({{"ssid", "Verborgen netwerk"}, {"password", "wifi-wachtwoord"}}).c_str(), t2.c_str()) == 202 &&
          g_wtSsid == "Verborgen netwerk" && g_wtStarts == 1, "WiFi test started (a hidden network is joined by name)");
    CHECK(call("POST", "/api/provision/wifi", "{\"ssid\":\"Thuis\",\"password\":\"\"}", t2.c_str()) == 409 && err("wifi-test-running"), "one test at a time");
    g_wtState = "wrong-password";
    CHECK(call("GET", "/api/provision/state", "", nullptr) == 200 && J["wifi"]["test"] == "wrong-password" && J["wifi"]["ssid"] == "Thuis",
          "the result is readable; the stored name only when logged in");
    CHECK(call("POST", "/api/provision/wifi", "{\"ssid\":\"Open Cafe\",\"password\":\"\"}", t2.c_str()) == 202 && g_wtStarts == 2,
          "retry after a wrong password; an open network has no password");
    g_wtState = "connected";
    // Step 3 (profile) over the AP while installing: the read session is not refused for being on the AP.
    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:000101\"}", t2.c_str()) != 403, "installing, AP only: the profile step works over the AP");
    call("POST", "/api/setup/read/cancel", "{\"sid\":1}", t2.c_str());
    // Step 4: the owner key.
    CHECK(call("POST", "/api/provision/key", "{\"passphrase\":\"te kort\"}", t2.c_str()) == 400 && err("passphrase-weak"), "short passphrase refused");
    CHECK(call("POST", "/api/provision/key", "{\"passphrase\":\"eerste-wachtwoord\"}", t2.c_str()) == 400 && err("passphrase-is-password"),
          "the passphrase must differ from the admin password");
    g_cryptoOk = false;
    CHECK(call("POST", "/api/provision/key", "{\"passphrase\":\"mijn sleutelzin 1\"}", t2.c_str()) == 503 && err("crypto-unavailable"),
          "a failed crypto self-test closes the key routes");
    g_cryptoOk = true;
    CHECK(call("GET", "/api/provision/key/file", "", nullptr) == 403 && err("no-key-pending"), "no key yet: no file");
    CHECK(call("POST", "/api/provision/key", "{\"passphrase\":\"mijn sleutelzin 1\"}", t2.c_str()) == 200 && jstr("fingerprint").size() == 14,
          "key created; only its fingerprint in the reply");
    CHECK(call("GET", "/api/provision/key/file", "", nullptr) == 200 && jstr("filename") == "orcon-recovery-key.pem", "the file is offered for download");
    const std::string staleFile = jstr("pem");
    CHECK(staleFile.find("-----BEGIN ENCRYPTED PRIVATE KEY-----") != std::string::npos, "an encrypted PKCS#8 PEM");
    CHECK(call("POST", "/api/provision/key", "{\"passphrase\":\"mijn sleutelzin 2\"}", t2.c_str()) == 200, "a second key replaces the pending one");
    call("GET", "/api/provision/key/file", "", nullptr);
    const std::string file = jstr("pem");
    CHECK(call("POST", "/api/provision/key/verify", body({{"pem", file}, {"passphrase", "mijn sleutelzin 1"}}).c_str(), t2.c_str()) == 400 &&
          err("bad-passphrase") && g_ownState == provision::State::Setup, "wrong passphrase: not registered");
    CHECK(call("POST", "/api/provision/key/verify", body({{"pem", "geen sleutel"}, {"passphrase", "mijn sleutelzin 2"}}).c_str(), t2.c_str()) == 400 &&
          err("bad-file"), "not a key file: not registered");
    CHECK(call("POST", "/api/provision/key/verify", body({{"pem", staleFile}, {"passphrase", "mijn sleutelzin 1"}}).c_str(), t2.c_str()) == 409 &&
          err("key-mismatch") && !g_ownPubSet, "the file of an earlier attempt: not registered");
    const int restarts = g_restarts;
    CHECK(call("POST", "/api/provision/key/verify", body({{"pem", file}, {"passphrase", "mijn sleutelzin 2"}}).c_str(), t2.c_str()) == 200 &&
          J["installed"] == true && g_ownPubSet && g_ownState == provision::State::Done && g_restarts == restarts + 1,
          "the saved file opens: key registered, installation done, restart onto the home network");
    // Installed: the wizard is closed, the release key grants nothing, nothing returns the device to "new".
    g_apOnly = false;
    CHECK(call("POST", "/api/provision/password", "{\"password\":\"overname-poging\"}", nullptr) == 403 && err("not-installing"), "installed: no first password");
    CHECK(call("POST", "/api/provision/key", "{\"passphrase\":\"mijn sleutelzin 3\"}", t2.c_str()) == 403 && err("physical-mode-required"),
          "installed: a new key only in the PRG recovery mode");
    CHECK(call("POST", "/api/provision/recover", body({{"pem", file}, {"passphrase", "mijn sleutelzin 2"}, {"password", "nieuw-wachtwoord"}}).c_str(),
               nullptr) == 403 && err("physical-mode-required"), "installed: recovery only in the PRG recovery mode");
    CHECK(call("POST", "/api/auth/nonce", "{}", nullptr, "application/json", nullptr) == 403 && err("owner-key-required"),
          "the release key's password recovery is closed once an owner key exists");
#if ORCON_WEB_CONTROL
    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:000101\"}", t2.c_str()) == 403, "installed: RF control over the AP is refused again");
#endif
    // Forgotten password: PRG menu -> recovery mode -> the key file over the setup AP.
    g_physical = true; g_cookie.clear();
    for (int i = 0; i < 4; ++i)
        CHECK(call("POST", "/api/provision/recover", body({{"pem", file}, {"passphrase", "verkeerde zin!"}, {"password", "nieuw-wachtwoord"}}).c_str(),
                   nullptr) == 401 && err("bad-passphrase"), "recovery with a wrong passphrase refused (counts as a failed login)");
    CHECK(call("POST", "/api/provision/recover", body({{"pem", staleFile}, {"passphrase", "mijn sleutelzin 1"}, {"password", "nieuw-wachtwoord"}}).c_str(),
               nullptr) == 401 && err("key-mismatch") && J["retry_s"] == 30, "another (valid) key: refused, and the fifth failure locks");
    CHECK(call("POST", "/api/provision/recover", body({{"pem", file}, {"passphrase", "mijn sleutelzin 2"}, {"password", "nieuw-wachtwoord"}}).c_str(),
               nullptr) == 429 && err("login-locked"), "locked: even the right key waits");
    g_ms += 31000; loopTick();
    CHECK(call("POST", "/api/provision/recover", body({{"pem", file}, {"passphrase", "mijn sleutelzin 2"}, {"password", "nieuw-wachtwoord"}}).c_str(),
               nullptr) == 200 && !g_cookie.empty(), "the right key file: new password, logged in");
    g_cookie.clear();
    CHECK(call("POST", "/api/auth/login", "{\"password\":\"eerste-wachtwoord\"}", nullptr) == 401, "the old password no longer works");
    CHECK(call("POST", "/api/auth/login", "{\"password\":\"nieuw-wachtwoord\"}", nullptr) == 200, "the new one does");
    const std::string t4 = jstr("token");
    // A new key in the recovery mode (logged in) makes the old file worthless.
    CHECK(call("POST", "/api/provision/key", "{\"passphrase\":\"nieuwe sleutelzin\"}", t4.c_str()) == 200, "recovery mode: a new key");
    call("GET", "/api/provision/key/file", "", nullptr);
    const std::string file2 = jstr("pem");
    CHECK(call("POST", "/api/provision/key/verify", body({{"pem", file2}, {"passphrase", "nieuwe sleutelzin"}}).c_str(), t4.c_str()) == 200 &&
          J["installed"] == false && g_restarts == restarts + 1, "new key registered; no restart outside the installation");
    g_cookie.clear();
    CHECK(call("POST", "/api/provision/recover", body({{"pem", file}, {"passphrase", "mijn sleutelzin 2"}, {"password", "nog-een-wachtwoord"}}).c_str(),
               nullptr) == 401 && err("key-mismatch"), "the replaced key file recovers nothing");
    // The recovery mode ends: recovery is closed again.
    g_physical = false;
    CHECK(call("POST", "/api/provision/recover", body({{"pem", file2}, {"passphrase", "nieuwe sleutelzin"}, {"password", "nog-een-wachtwoord"}}).c_str(),
               nullptr) == 403 && err("physical-mode-required"), "after the recovery mode: closed");
    // Captive portal: a phone's connectivity check on the AP goes to the wizard.
    srv.req.clear(); srv.req["Accept-Encoding"] = "gzip"; srv.host = "captive.apple.com"; srv.sent.clear(); srv.routes["GET /"]();
    CHECK(srv.code == 302 && srv.sent["Location"] == "http://192.0.2.5/", "captive portal: another name on the AP is redirected to the wizard");
    // No HTTP route can reset the device or return it to "new".
    bool resetRoute = false;
    for (auto& r : srv.routes) resetRoute |= r.first.find("factory") != std::string::npos || r.first.find("provision/reset") != std::string::npos;
    CHECK(!resetRoute && g_ownState == provision::State::Done, "no factory-reset route; the state never went back to new");
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
#endif

int main() {
    printf("Setup web API tests (RX_ONLY=%d)\n", RX_ONLY);
    orcon::begin(37, 404, 29, 202);
    profiles::store().begin(profileMemory);
    setupweb::restoreProfileSelection();
    setupweb::begin(srv);
#if ORCON_OWNER_MODEL && PROVISION_SCENARIO == 1
    return provisionScenario();
#elif ORCON_OWNER_MODEL && PROVISION_SCENARIO == 2
    return unreadableScenario();
#elif ORCON_OWNER_MODEL && PROVISION_SCENARIO == 3
    return ambiguousScenario();
#endif

    // ---- page ------------------------------------------------------------------------------
    srv.req.clear();
    CHECK(call("GET", "/", "", nullptr, nullptr, nullptr) == 406, "page without gzip support: 406, never raw bytes");
    { srv.req.clear(); srv.req["Accept-Encoding"] = "gzip, deflate"; srv.body = ""; srv.routes["GET /"]();
      CHECK(srv.code == 200 && srv.sent["Content-Encoding"] == "gzip" && srv.out.size() == SETUP_PAGE_GZ_LEN,
            "page served from flash as gzip with Content-Encoding");
      CHECK(srv.type == "text/html; charset=utf-8" && srv.sent.count("Content-Security-Policy") && srv.sent["X-Frame-Options"] == "DENY",
            "page has charset, CSP and frame protection"); }

#if ORCON_WEB_AUTH
    // ---- web login: no default password, a signed recovery code, sessions + CSRF, throttling --------
    CHECK(call("GET", "/api/setup/state", "", nullptr) == 200 && J["auth"]["logged_in"] == false && J["auth"]["password_set"] == false &&
          !J["token"].is<const char*>() && J["profiles"].isNull() && J["unit"].isNull() && J["caps"].isNull(),
          "no session: the state only says how to log in (no token, profiles, unit or caps)");
    for (const char* path : {"/api/profiles", "/api/setup/pair/state", "/api/setup/read/state"})
        CHECK(call("GET", path, "", nullptr) == 401 && err("login-required"), "no session: session data refused");
    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:000101\"}") == 401 && err("login-required"),
          "no session: no mutation, not even with the old per-boot token");
#if ORCON_TX_POC
    CHECK(call("GET", "/api/remote/state", "", nullptr) == 401 && err("login-required"), "no session: control state refused");
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"2\"}") == 401 && txpoc::s_rf.sent[0] == 0, "no session: no RF command");
#else
    for (const char* path : {"/api/setup/restart", "/api/setup/wifi/forget"})
        CHECK(call("POST", path) == 401 && !g_restarts && !g_forgets, "no session: no restart and no WiFi change");
#endif
    CHECK(call("GET", "/api/setup/console", "", nullptr) == 200, "diagnostics stay readable without a login (masked IDs)");
    CHECK(call("POST", "/api/auth/login", "{\"password\":\"geheim-wachtwoord\"}", nullptr) == 409 && err("password-not-set"),
          "no password stored: nothing to log in with (there is no default)");
    CHECK(call("POST", "/api/auth/setup", "{\"code\":\"AAAA-AAAA\",\"password\":\"geheim-wachtwoord\"}", nullptr) == 409 &&
          err("code-required") && !g_storeSet, "no code issued: no password can be set");
    // CLI: a one-time nonce, then a request signed with the OTA key buys a 10-minute code.
    auto nonceNow = []() { call("POST", "/api/auth/nonce", "{}", nullptr, "application/json", nullptr); return std::string(J["nonce"] | ""); };
    auto resetBody = [](const std::string& n, const char* sig, const char* mac) {
        JsonDocument b;
        b["manifest"] = "{\"v\":1,\"kind\":\"orcon-auth-reset\",\"mac\":\"" + std::string(mac) + "\",\"nonce\":\"" + n + "\"}";
        b["sig"] = sig; std::string out; serializeJson(b, out); return out;
    };
    const char* MAC = "02:00:00:00:00:0a";
    std::string nonce = nonceNow();
    CHECK(nonce.size() == 32 && J["mac"] == MAC, "CLI: a 128-bit nonce and the board to sign for");
    CHECK(call("POST", "/api/auth/reset", resetBody(nonce, "dead", MAC).c_str(), nullptr, "application/json", nullptr) == 403 &&
          err("bad-signature"), "request without a valid OTA-key signature: no code");
    CHECK(call("POST", "/api/auth/reset", resetBody(nonce, "a1b2", MAC).c_str(), nullptr, "application/json", nullptr) == 403 &&
          err("nonce-mismatch"), "the nonce was spent by the failed attempt (no replay)");
    CHECK(call("POST", "/api/auth/reset", resetBody(nonceNow(), "a1b2", "02:00:00:00:00:09").c_str(), nullptr, "application/json", nullptr) == 403 &&
          err("wrong-board"), "a request for another board: no code");
    CHECK(call("POST", "/api/auth/reset", resetBody(nonceNow(), "a1b2", MAC).c_str(), nullptr, "text/plain", nullptr) == 403,
          "the CLI route needs JSON too (no cross-site form posts)");
    CHECK(call("POST", "/api/auth/reset", resetBody(nonceNow(), "a1b2", MAC).c_str(), nullptr, "application/json", nullptr) == 200 &&
          std::string(J["code"] | "").size() == 9 && J["ttl_s"] == 600, "signed request: a one-time code, 10 minutes");
    const std::string code = J["code"].as<const char*>();
    { uint32_t cc = 0; const std::string all = console(&cc);
      CHECK(has(all, "herstelcode uitgegeven") && !has(all, code.c_str()), "console notes the code, never shows it"); }
    CHECK(call("POST", "/api/auth/setup", ("{\"code\":\"" + code + "\",\"password\":\"kort\"}").c_str(), nullptr) == 400 &&
          err("password-weak") && !g_storeSet, "weak password refused before the code is spent");
    CHECK(call("POST", "/api/auth/setup", "{\"code\":\"ZZZZ-ZZZZ\",\"password\":\"geheim-wachtwoord\"}", nullptr) == 401 &&
          err("bad-code") && !g_storeSet, "wrong code refused");
    CHECK(call("POST", "/api/auth/setup", ("{\"code\":\"" + code + "\",\"password\":\"geheim-wachtwoord\"}").c_str(), nullptr,
               "application/json", "http://203.0.113.7") == 403 && !g_storeSet, "setting the password needs our own origin");
    g_kdfOk = false;
    CHECK(call("POST", "/api/auth/setup", ("{\"code\":\"" + code + "\",\"password\":\"geheim-wachtwoord\"}").c_str(), nullptr) == 503 &&
          err("auth-unavailable") && !g_storeSet, "PBKDF2 self-test failed: fail closed, nothing stored");
    g_kdfOk = true;
    CHECK(call("POST", "/api/auth/setup", ("{\"code\":\"" + code + "\",\"password\":\"geheim-wachtwoord\"}").c_str(), nullptr) == 200 &&
          J["ok"] == true && g_storeSet && !g_cookie.empty(), "code + password: stored as a salted hash, logged in");
    g_csrf = J["token"] | ""; TOKEN = g_csrf.c_str();
    CHECK(has(g_setCookie, "HttpOnly") && has(g_setCookie, "SameSite=Strict") && has(g_setCookie, "Path=/") && has(g_setCookie, "Max-Age=2592000"),
          "session cookie: HttpOnly, SameSite=Strict, 30 days");
    CHECK(!has(std::string((const char*)&g_store, sizeof(g_store)), "geheim-wachtwoord"), "the password itself is never stored");
    CHECK(call("POST", "/api/auth/setup", ("{\"code\":\"" + code + "\",\"password\":\"ander-wachtwoord\"}").c_str(), nullptr) == 409 &&
          err("code-required"), "the code works once");
    CHECK(call("GET", "/api/setup/state") == 200 && J["auth"]["logged_in"] == true && J["token"] == g_csrf, "session: full state and its CSRF token");
    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:000303\"}", "a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5") == 403 && err("bad-token"),
          "session cookie alone is not enough for a mutation (CSRF token required)");
    const std::string firstCookie = g_cookie;
    CHECK(call("POST", "/api/auth/logout") == 200 && g_cookie.empty(), "logout clears the cookie");
    g_cookie = firstCookie;
    CHECK(call("GET", "/api/profiles", "", nullptr) == 401, "a logged-out session is gone on the device too");
    g_cookie.clear();
    // Login throttle: four free attempts, then 30 s, doubling; the right password waits too.
    for (int i = 0; i < 4; ++i)
        CHECK(call("POST", "/api/auth/login", "{\"password\":\"fout-wachtwoord\"}", nullptr) == 401 && err("bad-password") && J["retry_s"] == 0,
              "wrong password: 401, no wait yet");
    CHECK(call("POST", "/api/auth/login", "{\"password\":\"fout-wachtwoord\"}", nullptr) == 401 && J["retry_s"] == 30, "fifth wrong password: 30 s lock");
    CHECK(call("POST", "/api/auth/login", "{\"password\":\"geheim-wachtwoord\"}", nullptr) == 429 && err("login-locked") && J["retry_s"] == 30 &&
          g_cookie.empty(), "locked: even the right password must wait (no guessing at full speed)");
    call("GET", "/api/setup/state", "", nullptr);
    CHECK(J["auth"]["login_wait_s"] == 30, "the login page is told how long to wait");
    { uint32_t cc = 0; const std::string all = console(&cc);
      CHECK(has(all, "onjuist wachtwoord (5 keer op rij)") && !has(all, "fout-wachtwoord") && !has(all, "geheim-wachtwoord"),
            "console notes failures, never a password"); }
    g_ms += 30000;
    CHECK(call("POST", "/api/auth/login", "{\"password\":\"geheim-wachtwoord\"}", nullptr) == 200 && !g_cookie.empty(), "after the wait: logged in");
    g_csrf = J["token"] | ""; TOKEN = g_csrf.c_str();
    // Change the password: the current one is required; every other session ends.
    CHECK(call("POST", "/api/auth/password", "{\"current\":\"geheim-wachtwoord\",\"password\":\"kort\"}") == 400 && err("password-weak"),
          "change: weak new password refused");
    CHECK(call("POST", "/api/auth/password", "{\"current\":\"verkeerd-oud\",\"password\":\"nieuw-wachtwoord\"}") == 401 && err("bad-password"),
          "change: wrong current password refused");
    const std::string before = g_cookie;
    CHECK(call("POST", "/api/auth/password", "{\"current\":\"geheim-wachtwoord\",\"password\":\"nieuw-wachtwoord\"}") == 200 &&
          g_cookie != before, "change: new password, a fresh session for this browser");
    g_csrf = J["token"] | ""; TOKEN = g_csrf.c_str();
    { const std::string now = g_cookie; g_cookie = before;
      CHECK(call("GET", "/api/profiles", "", nullptr) == 401, "change: the old session no longer works"); g_cookie = now; }
    CHECK(call("GET", "/api/setup/state") == 200 && J["auth"]["logged_in"] == true, "change: logged in with the new session");
#endif

    // ---- capabilities ------------------------------------------------------------------------
    CHECK(call("GET", "/api/setup/state") == 200, "GET state");
#if ORCON_TX_POC
    CHECK(J["rx_only"] == true && J["caps"]["tx_supported"] == true && J["caps"]["pair_supported"] == bool(!ORCON_TX_RF_ENABLE || ORCON_TX_PAIR) &&
          J["caps"]["control_supported"] == true && J["caps"]["read_supported"] == true &&
          J["caps"]["simulated"] == bool(!ORCON_TX_RF_ENABLE) && J["caps"]["timer_supported"] == false,
          "web-control caps: control through the txpoc layer, simulated (dry-run), five functions");
#else
    CHECK(J["rx_only"] == bool(RX_ONLY) && J["caps"]["tx_supported"] == !RX_ONLY && J["caps"]["pair_supported"] == !RX_ONLY &&
          J["caps"]["control_supported"] == !RX_ONLY && J["caps"]["read_supported"] == true, "capabilities come from the build");
#endif
    CHECK(J["token"] == TOKEN && J["active"] == "none" && J["unit"]["id"] == "29:000202", "token, session and selected unit reported");
    CHECK(J["remotes"].size() == 0, "no remote heard yet");
    srv.req.clear(); srv.host = "attacker.example"; srv.routes["GET /api/setup/state"](); deserializeJson(J, srv.out);
    CHECK(!J["token"].is<const char*>() || J["token"] == "", "DNS rebinding: a foreign Host never receives the token");
    rx(press(0x01));                                          // the user presses a key: discovered
    rx(fr(MsgType::I, UNIT, UNIT, 0x31D9, {0x00, 0x00, 0x01}));
    call("GET", "/api/setup/state");
    CHECK(J["remotes"].size() == 1 && J["remotes"][0]["id"] == "29:000101", "remote discovered from its 22F1, unit not listed as remote");
    CHECK(J["wifi"]["connected"] == true && J["wifi"]["ssid"] == "Thuis \"5G\"\x01" && J["caps"]["settings_pages"] == (!ORCON_TX_POC || ORCON_TX_CONTROL),
          "state: WiFi network (JSON-escaped) and whether the classic settings pages exist in this build");
    g_apModeFake = true; call("GET", "/api/setup/state");
    CHECK(J["wifi"]["connected"] == false && J["wifi"]["ssid"] == "", "setup AP: reported as not connected, no network name");
    CHECK(J["wifi"]["ap"] == true && J["wifi"]["ap_name"] == WIFI_AP_NAME && J["wifi"]["ip"] == "192.0.2.5", "setup AP: AP flag, AP name and address reported");
    g_apModeFake = false;
#if ORCON_TX_POC && !ORCON_TX_CONTROL
    CHECK(!srv.routes.count("POST /api/setup/restart") && !srv.routes.count("POST /api/setup/wifi/forget"),
          "TX-PoC builds have no settings pages: no restart / forget-WiFi route");
#else
    for (const char* path : {"/api/setup/restart", "/api/setup/wifi/forget"}) {
        CHECK(call("POST", path, "{}", "wrong") == 403 && err("bad-token") && !g_restarts && !g_forgets, "system action without token refused");
        CHECK(call("POST", path, "{}", TOKEN, "text/plain") == 403 && !g_restarts && !g_forgets, "system action needs JSON (CSRF guard)");
        CHECK(call("POST", path, "{\"now\":1}") == 400 && err("unknown-field") && !g_restarts && !g_forgets, "system action takes no fields");
    }
    CHECK(call("POST", "/api/setup/restart") == 200 && J["restarting"] == true && g_restarts == 1 && g_forgets == 0, "restart scheduled after the reply");
    CHECK(call("POST", "/api/setup/wifi/forget") == 200 && g_forgets == 1 && g_restarts == 1, "forget WiFi: only the WiFi credentials, then the setup AP");
#endif

    // ---- POST guard and validation ---------------------------------------------------------------
    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:000101\"}", nullptr) == 403 && err("bad-token"), "missing token: 403");
    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:000101\"}", "a5a5") == 403 && err("bad-token"), "wrong token: 403");
    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:000101\"}", TOKEN, "text/plain") == 403 && err("forbidden-request"),
          "simple content type (cross-site form): 403");
    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:000101\"}", TOKEN, "application/json", "http://203.0.113.7") == 403,
          "foreign Origin: 403");
    { srv.req.clear(); srv.host = "attacker.example"; srv.body = "{\"remote\":\"29:000101\"}";
      srv.req["X-Orcon-Token"] = TOKEN; srv.req["Content-Type"] = "application/json"; srv.req["Origin"] = "http://attacker.example";
      srv.routes["POST /api/setup/read/start"](); deserializeJson(J, srv.out);
      CHECK(srv.code == 403 && J["error"] == "forbidden-request", "DNS rebinding: matching Origin/Host of a foreign name refused"); }
    std::string big = "{\"remote\":\"" + std::string(200, 'x') + "\"}";
    CHECK(call("POST", "/api/setup/read/start", big.c_str()) == 413, "oversized body: 413");
    CHECK(call("POST", "/api/setup/read/start", "{remote:") == 400 && err("bad-json"), "malformed JSON: 400");
    CHECK(call("POST", "/api/setup/read/start", "[1]") == 400 && err("bad-json"), "non-object JSON: 400");
    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:000101\",\"x\":1}") == 400 && err("unknown-field"), "unknown field: 400");
    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":29000101}") == 400 && err("bad-remote"), "remote must be a string");
    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:0001\"}") == 400 && err("bad-remote"), "malformed remote id");
    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:000303\"}") == 404 && err("remote-not-heard"), "unheard remote refused");
    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:000202\"}") == 404, "a unit is not a remote");
    CHECK(srv.routes.count("GET /api/setup/read/start") == 0 && srv.routes.count("POST /api/setup/read/state") == 0,
          "state changes only via POST, state reads only via GET");
    g_rxState = 2;
    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:000101\"}") == 503 && err("rf-unavailable"), "RF down: refused");
    g_rxState = 0;

    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:000101\",\"remote\":\"29:000303\"}") == 400 && err("bad-json"), "duplicate JSON keys rejected before interpretation");
    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:000101\"}{}") == 400 && err("bad-json"), "trailing JSON object rejected");
#if ORCON_TX_POC
    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:000101\"}", TOKEN, "application/json", nullptr) == 403,
          "web-control mutations require an Origin");
#endif

#if ORCON_TX_POC
    g_apModeFake = true;
    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:000101\"}") == 403, "setup AP refuses web-control mutations");
    g_apModeFake = false;
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"2\",\"request\":0}") == 400 && err("bad-request"), "zero command sequence refused");
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"2\",\"request\":1.5}") == 400 && err("bad-request"), "noninteger command sequence refused");
#endif

    // ---- read session through the RX pipeline -------------------------------------------------
    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:000101\"}") == 201 && J["status"] == "active" && J["step"] == 0,
          "start: 201 active, step 0");
    const uint32_t sid = J["sid"];
    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:000101\"}") == 409 && err("session-busy"), "second session refused");
    CHECK(call("POST", "/api/setup/pair/start") == (RX_ONLY && (!ORCON_TX_POC || (ORCON_TX_RF_ENABLE && !ORCON_TX_PAIR)) ? 501 : 409), "pairing during reading refused");
    rx(fr(MsgType::I, REM2, UNIT, 0x22F1, {0x00, 0x00, 0x04}));         // neighbour presses the right key
    rx(press(0x03));                                                    // ours, wrong key
    call("GET", "/api/setup/read/state");
    CHECK(J["step"] == 0 && J["others"] == 1 && J["why"] == "wrong-button" && J["why_mode"] == 3, "other device and wrong key do not count");
    const uint8_t modes[] = { 0x00, 0x04, 0xFF, 0x01, 0x02, 0x03 };
    for (int k = 0; k < 6; k++) {
        rx(modes[k] == 0xFF ? fr(MsgType::I, REM, UNIT, 0x22F3, {0x00, 0x12, 0x1E, 0x03, 0x04, 0x04, 0x04}) : press(modes[k]));
        rx(modes[k] == 0xFF ? fr(MsgType::I, REM, UNIT, 0x22F3, {0x00, 0x12, 0x1E, 0x03, 0x04, 0x04, 0x04}) : press(modes[k]));   // repeat
        loopTick();
    }
    call("GET", "/api/setup/read/state");
    CHECK(J["status"] == "done" && J["step"] == 6 && J["steps"][2]["cmd"] == "22F3" && J["steps"][5]["pl"] == "000304" &&
          J["steps"][0]["rssi"] == -61, "six presses -> done with commands, payloads, RSSI");
    CHECK(J["steps"][0]["ev"].as<uint32_t>() < J["steps"][5]["ev"].as<uint32_t>(), "event ids increase");
    CHECK(call("POST", "/api/setup/read/cancel", "{\"sid\":1}") == 409, "cancel of a finished session refused");
    { const auto own=orcon::ids().us;const auto sent=g_sent;
      char body[160];snprintf(body,sizeof(body),"{\"sid\":%u,\"name\":\"Ventilatie woonkamer\"}",unsigned(sid));
      profileMemory.failWrite=true;
      CHECK(call("POST","/api/profiles/save",body)==503 && err("profile-write-failed") && profiles::store().data().count==0 &&
            profiles::store().data().active==0,"NVS failure on save: nothing stored, nothing activated, no false success");
      profileMemory.failWrite=false;
      CHECK(call("POST","/api/profiles/save",body)==201 && J["items"].size()==1 && J["active"]==J["items"][0]["id"] &&
            J["items"][0]["remote"]=="29:000101","finished read saved and activated in one step");
      const uint32_t id=J["items"][0]["id"];
      CHECK(orcon::ids().us==own && orcon::ids().fan==UNIT && g_sent==sent,"activation on save routes to the profile unit, no clone, no RF");
      CHECK(profiles::store().begin(profileMemory) && profiles::store().active() && profiles::store().active()->id==id,
            "active selection restored after a simulated restart");
      CHECK(J["items"][0]["paired"]==false && J["items"][0]["control_mapping"]==true,"profile has five validated modes, not a pairing credential");
      CHECK(call("POST","/api/profiles/save",body)==201 && J["items"].size()==1,"duplicate profile save does not create duplicates");
      CHECK(profiles::store().begin(profileMemory) && profiles::store().find(id),"profile reload survives a simulated reboot");
      snprintf(body,sizeof(body),"{\"id\":%u}",unsigned(id));
      CHECK(call("POST","/api/profiles/select",body)==200 && J["active"]==id,"profile selected and selection persisted");
      CHECK(orcon::ids().us==own && orcon::ids().fan==UNIT && g_sent==sent,"selection never clones remote identity or sends RF");
      orcon::setIds(own.cls,own.serial,UNIT32.cls,UNIT32.serial);
      { char saved[160];snprintf(saved,sizeof(saved),"{\"sid\":%u,\"name\":\"Ventilatie woonkamer\"}",unsigned(sid));
        CHECK(call("POST","/api/profiles/save",saved)==201 && orcon::ids().fan==(RX_ONLY?UNIT:UNIT32) && g_sent==sent,
              "saving an active profile keeps routing aligned without RF"); }
      orcon::setIds(own.cls,own.serial,UNIT.cls,UNIT.serial);
      profileMemory.failWrite=true;
      CHECK(call("POST","/api/profiles/rename","{\"id\":1,\"name\":\"Keuken\"}")==503 && err("profile-write-failed"),"NVS failure is reported without false success");
      profileMemory.failWrite=false;
      CHECK(call("POST","/api/profiles/rename","{\"id\":1,\"name\":\"Keuken\\u0000extra\"}")==400 && err("bad-profile-name"),"embedded NUL in profile name rejected");
      CHECK(call("POST","/api/profiles/rename","{\"id\":1,\"name\":\"Keuken\"}")==200 && J["items"][0]["name"]=="Keuken","profile renamed");
      CHECK(call("POST","/api/profiles/delete",body)==200 && J["items"].size()==0 && J["active"]==0,"active profile deleted safely");
      CHECK(call("POST","/api/profiles/select","{\"id\":999}")==404 && err("profile-not-found"),"missing profile refused");
      CHECK(call("POST","/api/profiles/save","{\"sid\":999,\"name\":\"Naam\"}")==409,"profile save requires the completed session");
    }


    // ---- GET has no side effects; timeouts only from the loop ----------------------------------
    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:000101\"}") == 201, "restart");
    const uint32_t sid2 = J["sid"];
    CHECK(sid2 == sid + 1, "session ids increase");
    g_ms += 120000;
    call("GET", "/api/setup/read/state"); call("GET", "/api/setup/read/state");
    CHECK(J["status"] == "active" && J["left_s"] == 0, "GET does not expire the session itself");
    loopTick();
    call("GET", "/api/setup/read/state");
    CHECK(J["status"] == "timeout", "loop tick expires it");
    g_ms += 1800000;                                                    // > 30 min since the last press
    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:000101\"}") == 404, "remote unheard for too long: start refused");
    rx(press(0x02));
    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:000101\"}") == 201, "after a new press: start");
    const uint32_t sid3 = J["sid"];
    char b[48];
    snprintf(b, sizeof(b), "{\"sid\":%u}", unsigned(sid3 + 7));
    CHECK(call("POST", "/api/setup/read/cancel", b) == 409 && err("no-such-session"), "cancel with a stale sid refused");
    CHECK(call("POST", "/api/setup/read/cancel", "{\"sid\":\"x\"}") == 409, "cancel with a non-numeric sid refused");
    snprintf(b, sizeof(b), "{\"sid\":%u}", unsigned(sid3));
    CHECK(call("POST", "/api/setup/read/cancel", b) == 200 && J["status"] == "cancelled", "cancel");
    CHECK(!orcon::idsDirty(), "reading never marks identities for NVS");

#if ORCON_TX_POC
#if ORCON_TX_RF_ENABLE && ORCON_TX_PAIR
    // ---- RF test image: offer (captured structure) -> unit's W 1FC9 -> one closing ack -> learned unit
    orcon::setIds(37, 404, 32, 0);                                     // unit not yet known
    txpoc::s_rf.onair = true;
    CHECK(call("POST", "/api/setup/pair/start") == 201 && J["status"] == "ready" && J["simulated"] == false && J["supported"] == true,
          "RF pairing: session ready on the capped RF test image");
    { const uint32_t ps = J["sid"]; char x[96];
      snprintf(x, sizeof(x), "{\"sid\":%u,\"keys\":[\"auto\"]}", unsigned(ps));
      CHECK(call("POST", "/api/setup/pair/press", x) == 422 && txpoc::s_rf.sent[1] == 0, "only the Auto+1 combination sends");
      snprintf(x, sizeof(x), "{\"sid\":%u,\"keys\":[\"auto\",\"1\"]}", unsigned(ps));
      CHECK(call("POST", "/api/setup/pair/press", x) == 200 && txpoc::s_rf.sent[1] == 0, "offer accepted, owner not yet run");
      loopTick(); call("GET", "/api/setup/pair/state");
      CHECK(J["status"] == "bind_sent" && J["bind_sent"] == true && txpoc::s_rf.sent[1] == 1 && txpoc::s_rf.sent[2] == 0,
            "one offer on air, no ack yet");
      CHECK(call("POST", "/api/setup/pair/press", x) == 409 && err("already-sent") && txpoc::s_rf.sent[1] == 1, "never a second offer per session");
      // Not a confirmation: another device's W, a W with a foreign ID tuple, an RQ, a W to someone else.
      rx(fr(MsgType::W, UNIT32, REM, 0x1FC9, {0x00, 0x31, 0xD9, 0x80, 0, 0xCA}));
      rx(fr(MsgType::W, UNIT32, US, 0x1FC9, {0x00, 0x31, 0xD9, 0x74, 0, 0x65}));
      rx(fr(MsgType::RQ, UNIT32, US, 0x1FC9, {0x00, 0x31, 0xD9, 0x80, 0, 0xCA}));
      loopTick(); call("GET", "/api/setup/pair/state");
      CHECK(J["rf_confirmed"] == false && txpoc::s_rf.sent[2] == 0, "only the unit's exact W 1FC9 00 31D9 <own id> to us confirms");
      rx(fr(MsgType::W, UNIT32, US, 0x1FC9, {0x00, 0x31, 0xD9, 0x80, 0, 0xCA}));
      loopTick(); loopTick(); call("GET", "/api/setup/pair/state");
      CHECK(J["rf_confirmed"] == true && J["ack_sent"] == true && J["paired"] == true && txpoc::s_rf.sent[2] == 1,
            "unit confirmed -> exactly one closing ack -> paired");
      rx(fr(MsgType::W, UNIT32, US, 0x1FC9, {0x00, 0x31, 0xD9, 0x80, 0, 0xCA}));
      loopTick(); loopTick();
      CHECK(txpoc::s_rf.sent[2] == 1 && txpoc::s_rf.sent[1] == 1, "a repeated W never causes a second ack or offer");
      DeviceId learned(0, 0);
      CHECK(setupweb::takeLearnedFan(learned) && learned == UNIT32, "the confirming unit is handed over for NVS");
      uint32_t cc = 0; const std::string all = console(&cc);
      CHECK(has(all, "koppelen: 1FC9-aanbod verzonden (RF), fout none, RX-herstel ok") &&
            has(all, "koppelen: unit bevestigde (W 1FC9 00 31D9 aan ons ID)") && has(all, "koppelen: 1FC9-afsluiting verzonden (RF)") &&
            has(all, ", TX-start t="),
            "console: offer, confirmation and ack each leave a line for remote diagnosis"); }
    // Realistic order: the unit answers within ms, BEFORE the owner has published the offer's
    // result. The W must still count; if the offer then turns out not sent, it must not.
    for (int sentOk = 1; sentOk >= 0; --sentOk) {
        orcon::setIds(37, 404, 32, 0);
        txpoc::s_rf.onair = sentOk == 1; txpoc::s_rf.healthy = true;
        g_ms += 200000;                                                  // past the previous window and rate floor
        CHECK(call("POST", "/api/setup/pair/start") == 201, "RF pairing: second session");
        const uint32_t ps = J["sid"]; char x[96];
        snprintf(x, sizeof(x), "{\"sid\":%u,\"keys\":[\"auto\",\"1\"]}", unsigned(ps));
        const int offers = txpoc::s_rf.sent[1], acks = txpoc::s_rf.sent[2];
        CHECK(call("POST", "/api/setup/pair/press", x) == 200, "offer accepted");
        txpoc::s_owner.tick(g_ms, true); txpoc::s_owner.tick(g_ms, true);   // the owner starts the offer: TX on air
        rxAfter(fr(MsgType::W, UNIT32, US, 0x1FC9, {0x00, 0x31, 0xD9, 0x80, 0, 0xCA}), 20);   // ~20 ms: before the result is out
        loopTick(); loopTick(); call("GET", "/api/setup/pair/state");
        if (sentOk) CHECK(txpoc::s_rf.sent[1] == offers + 1 && J["rf_confirmed"] == true && J["paired"] == true && txpoc::s_rf.sent[2] == acks + 1,
                          "W before the offer result is published: still confirmed, one ack, paired");
        else CHECK(J["status"] == "tx_failed" && J["rf_confirmed"] == false && txpoc::s_rf.sent[2] == acks,
                   "offer not on air: an early W is discarded and no ack is sent");
        call("POST", "/api/setup/pair/cancel", (std::string("{\"sid\":") + std::to_string(ps) + "}").c_str());
    }
    {   // A W received after the press but BEFORE the radio started our offer cannot be its answer.
        orcon::setIds(37, 404, 32, 0);
        txpoc::s_rf.onair = true; g_ms += 200000;
        CHECK(call("POST", "/api/setup/pair/start") == 201, "RF pairing: session for the early-W case");
        const uint32_t ps = J["sid"]; char x[96];
        snprintf(x, sizeof(x), "{\"sid\":%u,\"keys\":[\"auto\",\"1\"]}", unsigned(ps));
        const int acks = txpoc::s_rf.sent[2];
        CHECK(call("POST", "/api/setup/pair/press", x) == 200, "offer accepted, not started yet");
        rx(fr(MsgType::W, UNIT32, US, 0x1FC9, {0x00, 0x31, 0xD9, 0x80, 0, 0xCA}));   // before the owner ran
        loopTick(); loopTick(); call("GET", "/api/setup/pair/state");
        CHECK(J["status"] == "bind_sent" && J["rf_confirmed"] == false && txpoc::s_rf.sent[2] == acks,
              "a W from before our TX start is never the unit's answer: no confirmation, no ack");
        call("POST", "/api/setup/pair/cancel", (std::string("{\"sid\":") + std::to_string(ps) + "}").c_str());
    }
    txpoc::s_rf.onair = false;
    orcon::setIds(37, 404, 32, 202);
#elif ORCON_TX_RF_ENABLE
    CHECK(call("POST", "/api/setup/pair/start") == 501 && err("pairing-unverified"), "RF pairing blocked without complete offer capture");
    CHECK(call("POST", "/api/setup/pair/press", "{\"sid\":1,\"keys\":[\"auto\",\"1\"]}") == 501, "RF bind press also blocked");
    CHECK(txpoc::s_rf.sent[1] == 0 && txpoc::s_rf.sent[2] == 0, "unverified binding never reaches backend");
    rx(fr(MsgType::W, UNIT32, US, 0x1FC9, {0x00, 0x31, 0xD9, 0x80, 0, 0xCA}));   // a unit "confirms" us unasked
    loopTick(); loopTick(); call("GET", "/api/setup/pair/state");
    CHECK(txpoc::s_rf.sent[0] == 0 && txpoc::s_rf.sent[2] == 0 && J["rf_confirmed"] == false && J["paired"] == false,
          "RF build: an incoming W 1FC9 never triggers an automatic ack or pairing");
    orcon::setIds(37,404,32,202);
#else
    // ---- web-control pairing: offer -> W 1FC9 confirm -> closing ack -> learned unit ------------
    orcon::setIds(37, 404, 32, 0);                                     // unit not yet known
    CHECK(call("POST", "/api/setup/pair/press", "{\"sid\":1,\"keys\":[\"auto\",\"1\"]}") == 409, "press without session refused");
    CHECK(call("POST", "/api/setup/pair/start") == 201 && J["status"] == "ready" && J["simulated"] == true,
          "start: ready, dry-run flagged, nothing sent yet");
    const uint32_t ps = J["sid"];
    auto press2 = [&](const char* keys) { char x[96]; snprintf(x, sizeof(x), "{\"sid\":%u,\"keys\":%s}", unsigned(ps), keys); return call("POST", "/api/setup/pair/press", x); };
    CHECK(press2("[\"auto\"]") == 422 && err("unsupported-combination"), "Auto alone: 422");
    CHECK(press2("[\"auto\",\"auto\"]") == 400 && err("bad-keys"), "duplicate key: 400");
    CHECK(press2("[\"1\",\"auto\"]") == 200 && J["status"] == "ready" && txpoc::s_rf.sent[1] == 0,
          "valid press: accepted, transmission still pending (async owner)");
    loopTick();
    call("GET", "/api/setup/pair/state");
    CHECK(J["status"] == "dry_run_complete" && J["bind_sent"] == false && txpoc::s_rf.sent[1] == 1,
          "dry-run offer constructed once, never claimed on air, session released");
    CHECK(J["paired"] == false && J["rf_confirmed"] == false, "dry-run cannot pair");
    rx(fr(MsgType::W, UNIT32, US, 0x1FC9, {0x00, 0x31, 0xD9, 0x80, 0, 0xCA}));
    loopTick(); call("GET", "/api/setup/pair/state");
    CHECK(J["rf_confirmed"] == false && txpoc::s_rf.sent[2] == 0, "RX after dry-run causes no ack or pairing");
    { DeviceId learned(0,0);
      CHECK(!setupweb::takeLearnedFan(learned), "dry-run never offers an identity for persistence"); }
    orcon::setIds(37, 404, 32, 202); // separately configured synthetic unit

#endif
    // ---- web-control TX needs an active, verified profile: nothing is guessed ------------------
    rx(fr(MsgType::I, REM, UNIT32, 0x22F1, {0x00, 0x01, 0x04})); rx(fr(MsgType::I, UNIT32, UNIT32, 0x31D9, {0x00, 0x00, 0x01})); g_ms += 4000;
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"2\"}") == 409 && err("profile-required") && txpoc::s_rf.sent[0] == 0,
          "no active profile: TX refused before admission");
    { setupflow::Read read; read.st = setupflow::St::Done; read.step = setupflow::K_N; read.remote = REM;
      read.targetKnown = true; read.target = UNIT32;
      for (uint8_t k = 0; k < setupflow::K_N; ++k) { read.got[k].cmd = k == setupflow::K_TIMER ? 0x22F3 : 0x22F1; read.got[k].len = k == setupflow::K_TIMER ? 7 : 3;
                                                    read.got[k].pl[1] = setupflow::modeOfKey(k); read.got[k].pl[2] = 4; }
      uint32_t id = 0; CHECK(profiles::store().save(read, "Woonkamer", id) == profiles::Error::None, "verified tail-04 profile stored");
      char body[32]; snprintf(body, sizeof(body), "{\"id\":%u}", unsigned(id));
      CHECK(call("POST", "/api/profiles/select", body) == 200 && orcon::ids().fan == UNIT32 && orcon::ids().us == US, "profile selected, own identity kept");
      orcon::setIds(37, 404, 32, 303);                                // target drifted away from the profile
      CHECK(call("POST", "/api/remote/send", "{\"key\":\"2\"}") == 409 && err("profile-unit-mismatch") && txpoc::s_rf.sent[0] == 0,
            "profile unit differs from the TX target: refused");
      orcon::setIds(37, 404, 32, 202);
    }
    // ---- web-control remote: five functions, dry-run then on-air correlation -------------------
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"timer\"}") == 400 && err("bad-key"), "no timer on this build");
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"timer\",\"minutes\":30}") == 400, "timer with minutes still refused");
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"1\",\"minutes\":15}") == 400 && err("bad-minutes"), "minutes only meaningful for timer");
    CHECK(call("POST", "/api/remote/send", "{\"code\":\"22F1\",\"payload\":\"000307\"}") == 400 && err("unknown-field"),
          "no raw RF payloads accepted");
    rx(fr(MsgType::I, REM, UNIT32, 0x22F1, {0x00, 0x01, 0x04}));                                                   // physical remote: identity + unit state
    rx(fr(MsgType::I, UNIT32, UNIT32, 0x31D9, {0x00, 0x00, 0x01}));
    g_ms += 4000;                                                      // past the remote-activity guard
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"1\"}") == 409 && err("unit-already-in-mode"),
          "unit already in stand 1: nothing sent");
    // dry-run: the whole chain except the air interface; confirmation stays not_available.
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"2\"}") == 202 && J["accepted"] == true &&
          J["sent"] == false && J["simulated"] == bool(!ORCON_TX_RF_ENABLE), "accepted; no send before owner work");
    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:000101\"}") == 409 && err("session-busy"), "read session cannot overlap pending TX");
    loopTick(); call("GET", "/api/remote/state");
    CHECK(J["phase"] == "dry-run-complete" && J["confirm"] == "not_available" && J["rf_sent"] == false,
          "dry-run never claims a send or a confirmation");
    // on-air emulation: send completes -> the unit's own 31D9 in the window confirms.
    txpoc::s_rf.onair = true;
    g_ms += txpoc::RATE_CTL_MS;
    rx(fr(MsgType::I, REM, UNIT32, 0x22F1, {0x00, 0x01, 0x04})); rx(fr(MsgType::I, UNIT32, UNIT32, 0x31D9, {0x00, 0x00, 0x01})); g_ms += 4000;
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"2\"}") == 202 && J["sent"] == false, "queued: sent only after the owner ran");
    loopTick(); call("GET", "/api/remote/state");
    CHECK(J["sent"] == true && J["rf_sent"] == true && J["phase"] == "await-unit-status", "RF send done, awaiting the unit");
    rx(fr(MsgType::I, UNIT32, UNIT32, 0x31D9, {0x00, 0x00, 0x02}));
    loopTick(); call("GET", "/api/remote/state");
    CHECK(J["confirm"] == "pending" && J["phase"] == "await-unit-status", "a match stays provisional inside the window");
    g_ms += txpoc::CONFIRM_MS + txpoc::EXPIRY_GRACE_MS + 1;            // the observation window ends
    loopTick(); call("GET", "/api/remote/state");
    CHECK(J["confirm"] == "confirmed" && J["unit_mode"] == "medium" && J["unit_confirmed"] == true,
          "the unit's prompt report of the requested stand after our RF send is a confirmation");
    { const uint32_t request = J["id"]; const int sent = txpoc::s_rf.sent[0];
      char duplicate[96]; snprintf(duplicate, sizeof(duplicate), "{\"key\":\"2\",\"request\":%u}", unsigned(request));
      CHECK(call("POST", "/api/remote/send", duplicate) == 200 && txpoc::s_rf.sent[0] == sent,
            "duplicate HTTP request returns original result without sending again");
      snprintf(duplicate, sizeof(duplicate), "{\"key\":\"3\",\"request\":%u}", unsigned(request));
      CHECK(call("POST", "/api/remote/send", duplicate) == 409 && err("request-conflict"), "same sequence with different command refused");
    }
    { uint32_t cc = 0; const std::string all = console(&cc);
      CHECK(has(all, "INFO|TX #") && has(all, "stand 2: verzonden (RF), fout none, RX-herstel ok"),
            "console: each finished TX transaction leaves one line (sent, error, RX restore) for remote diagnosis");
      CHECK(has(all, " unit-reported-mode: TX-start t=") && has(all, ", unitmelding t=") && has(all, " ms)"),
            "console: on-air TX also logs its start pass and the unit's report time (correlation evidence)"); }
    {   // A matching report later than PROMPT_MS after our TX start: observed, never confirmed.
        g_ms += txpoc::RATE_CTL_MS;
        CHECK(call("POST", "/api/remote/send", "{\"key\":\"1\"}") == 202, "late-report case: accepted");
        loopTick(); g_ms += txpoc::PROMPT_MS + 100;
        rx(fr(MsgType::I, UNIT32, UNIT32, 0x31D9, {0x00, 0x00, 0x01})); loopTick();
        g_ms += txpoc::CONFIRM_MS + txpoc::EXPIRY_GRACE_MS + 1; loopTick(); call("GET", "/api/remote/state");
        CHECK(J["phase"] == "unit-reported-mode" && J["confirm"] == "observed" && J["unit_confirmed"] == false,
              "a late matching report is observed, not a confirmation");
    }
    // away + auto also work; a remote press during the guard is refused.
    txpoc::s_rf.onair = false;
    g_ms += txpoc::RATE_CTL_MS;
    rx(fr(MsgType::I, REM, UNIT32, 0x22F1, {0x00, 0x00, 0x04}));       // physical remote to our unit
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"3\"}") == 409 && err("remote-active"), "physical remote active: wait");
    g_ms += txpoc::REMOTE_GUARD_MS;
    rx(fr(MsgType::I, UNIT32, UNIT32, 0x31D9, {0x00, 0x00, 0x01}));    // fresh unit state
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"3\"}") == 202, "stand 3 accepted after the guard");
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"auto\"}") == 409 && err("session-busy"),
          "one open transaction at a time");
    loopTick(); g_ms += txpoc::RATE_CTL_MS;
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"auto\"}") == 202 && J["key"] == "auto", "auto accepted");
    loopTick();
#if ORCON_TX_MAX_PER_BOOT
    { const int before = txpoc::s_rf.sent[0]; g_ms += txpoc::RATE_CTL_MS; txpoc::g_rfStarts = ORCON_TX_MAX_PER_BOOT;
      CHECK(call("POST", "/api/remote/send", "{\"key\":\"1\"}") == 409 && err("tx-limit-reached") && txpoc::s_rf.sent[0] == before,
            "one-shot RF test image: after its packet every command is refused before admission, nothing prepared");
      call("GET", "/api/setup/state"); CHECK(J["caps"]["tx_max_per_boot"] == ORCON_TX_MAX_PER_BOOT, "state reports the per-boot cap to the page");
      g_restoreDiag = 0x04000004u; call("GET", "/api/remote/state");
      CHECK(J["restore_diag"] == "04000004" && J["tx_starts"] == ORCON_TX_MAX_PER_BOOT, "remote state: failed-restore reason and packets started this boot");
      g_restoreDiag = 0;
      txpoc::g_rfStarts = 0; }
#endif
    // RX health gates admission.
    g_radioOk = false; g_ms += txpoc::RATE_CTL_MS;
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"away\"}") == 503 && err("rf-unavailable"), "RX unhealthy: refused");
    g_radioOk = true; g_ms += txpoc::RATE_CTL_MS;
    rx(fr(MsgType::I, REM, UNIT32, 0x22F1, {0x00, 0x00, 0x04}));       // tail 04 learned from the remote
    rx(fr(MsgType::I, UNIT32, UNIT32, 0x31D9, {0x00, 0x00, 0x01})); g_ms += 4000;
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"away\"}") == 202, "away accepted");
    loopTick();
    call("GET", "/api/remote/state");
    CHECK(J["key"] == "away", "away tracked as last command");
    // All five user functions traverse admission -> owner -> final API status.
    { const char* key[] = {"away","1","2","3","auto"};
      for (uint8_t m=0; m<5; ++m) {
        g_ms += txpoc::RATE_CTL_MS;
        rx(fr(MsgType::I, UNIT32, UNIT32, 0x31D9, {0x00,0x00,uint8_t((m+1)%5)}));
        char body[48]; snprintf(body,sizeof(body),"{\"key\":\"%s\"}",key[m]);
        CHECK(call("POST","/api/remote/send",body)==202 && txpoc::controller().status().result.request.input.mode==m,
              "five-button API mapping preserves mode byte");
        loopTick(); call("GET","/api/remote/state");
        CHECK(J["key"]==key[m] && J["rf_sent"]==false && J["confirm"]=="not_available",
              "five-button dry-run completes without claiming RF confirmation");
      }
    }
#elif RX_ONLY
    // ---- RX-only: pairing and control refused, nothing reaches the radio -----------------------
    const uint32_t refused = orcon::state().txRefused;
    CHECK(call("POST", "/api/setup/pair/start") == 501 && err("tx-unavailable"), "pair start: 501 tx-unavailable");
    CHECK(call("POST", "/api/setup/pair/press", "{\"sid\":1,\"keys\":[\"auto\",\"1\"]}") == 501, "pair press: 501");
    call("GET", "/api/setup/pair/state");
    CHECK(J["supported"] == false && J["status"] == "idle" && J["bind_sent"] == false, "pair state: unsupported, idle");
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"2\"}") == 501 && err("tx-unavailable"), "remote send: 501");
    CHECK(call("GET", "/api/remote/state") == 501, "remote state: 501");
    CHECK(g_sent.empty() && orcon::state().txCount == 0 && orcon::state().txRefused == refused,
          "no TX attempted at all (not even a refused one)");
#else
    // ---- pairing (TX) ---------------------------------------------------------------------------
    orcon::setIds(37, 404, 32, 0);                                     // unit not yet known (HRC400 class 32)
    CHECK(call("POST", "/api/setup/pair/press", "{\"sid\":1,\"keys\":[\"auto\",\"1\"]}") == 409, "press without session refused");
    CHECK(call("POST", "/api/setup/pair/start") == 201 && J["status"] == "ready" && g_sent.empty(), "start: ready, nothing sent");
    const uint32_t ps = J["sid"];
    auto press2 = [&](const char* keys) { char x[96]; snprintf(x, sizeof(x), "{\"sid\":%u,\"keys\":%s}", unsigned(ps), keys); return call("POST", "/api/setup/pair/press", x); };
    CHECK(press2("[\"auto\"]") == 422 && err("unsupported-combination"), "Auto alone: 422");
    CHECK(press2("[\"auto\",\"1\",\"2\"]") == 422, "extra key: 422");
    CHECK(press2("[\"auto\",\"auto\"]") == 400 && err("bad-keys"), "duplicate key: 400");
    CHECK(press2("[\"auto\",\"x\"]") == 400 && press2("[]") == 400 && press2("\"auto\"") == 400, "bad key lists: 400");
    CHECK(g_sent.empty(), "nothing sent by invalid presses");
    g_radioOk = false;
    CHECK(press2("[\"1\",\"auto\"]") == 502 && err("tx-failed"), "radio failure: 502, not shown as success");
    call("GET", "/api/setup/pair/state");
    CHECK(J["status"] == "tx_failed" && J["bind_sent"] == false, "state says tx_failed");
    CHECK(press2("[\"1\",\"auto\"]") == 409 && err("already-sent"), "no automatic or repeated bind");
    snprintf(b, sizeof(b), "{\"sid\":%u}", unsigned(ps));
    CHECK(call("POST", "/api/setup/pair/cancel", b) == 200 && J["status"] == "cancelled", "cancel");
    g_radioOk = true;
    CHECK(call("POST", "/api/setup/pair/start") == 201, "new pairing session");
    const uint32_t ps2 = J["sid"];
    snprintf(b, sizeof(b), "{\"sid\":%u,\"keys\":[\"auto\",\"1\"]}", unsigned(ps2));
    CHECK(call("POST", "/api/setup/pair/press", b) == 200 && J["status"] == "bind_sent" && g_sent[0x1FC9] == 1, "Auto + 1: one 1FC9 sent");
    CHECK(J["paired"] == false && J["rf_confirmed"] == false, "sent is not confirmed");
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"2\"}") == 409, "no remote commands during pairing");
    rx(fr(MsgType::I, UNIT32, UNIT32, 0x31D9, {0x00, 0x00, 0x01}));    // unit traffic only
    loopTick(); call("GET", "/api/setup/pair/state");
    CHECK(J["rf_confirmed"] == false && J["paired"] == false && J["status"] == "bind_sent",
          "firmware learned a unit from traffic, but without a 1FC9 to us it is not paired");
    rx(fr(MsgType::W, UNIT32, US, 0x1FC9, {0x00, 0x22, 0xF1}));         // the unit's bind confirmation to us
    loopTick(); call("GET", "/api/setup/pair/state");
    CHECK(J["rf_confirmed"] == true && J["paired"] == true && J["status"] == "paired", "1FC9 to us + firmware unit -> paired");

    // ---- virtual remote (TX) ------------------------------------------------------------------
    orcon::setIds(37, 404, 29, 0);
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"2\"}") == 409 && err("unit-not-selected"), "no unit selected: 409");
    orcon::setIds(37, 404, 29, 202);
    g_sent.clear();
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"9\"}") == 400 && err("bad-key"), "unknown key: 400");
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"timer\"}") == 400 && err("bad-minutes"), "timer needs minutes");
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"timer\",\"minutes\":45}") == 400, "timer: only 15/30/60");
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"2\",\"minutes\":15}") == 400, "minutes only for timer");
    CHECK(call("POST", "/api/remote/send", "{\"code\":\"22F1\",\"payload\":\"000307\"}") == 400 && err("unknown-field"),
          "no raw RF payloads accepted");
    CHECK(g_sent.empty(), "nothing sent by invalid commands");
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"2\"}") == 202 && J["accepted"] == true && J["sent"] == true &&
          J["confirm"] == "pending", "accepted + sent; confirmation pending");
    CHECK(g_sent[0x22F1] == 2, "existing setMode(): one command = its two 22F1 copies, no more");
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"3\"}") == 429 && err("too-fast"), "flooding refused");
    rx(fr(MsgType::I, UNIT, UNIT, 0x31D9, {0x00, 0x00, 0x01}));         // unit reports another mode
    call("GET", "/api/remote/state");
    CHECK(J["confirm"] == "pending", "other mode is no confirmation");
    rx(fr(MsgType::I, REM, UNIT, 0x31D9, {0x00, 0x00, 0x02}));          // right mode, wrong sender
    call("GET", "/api/remote/state");
    CHECK(J["confirm"] == "pending", "a report from another device is no confirmation");
    rx(fr(MsgType::I, UNIT, UNIT, 0x31D9, {0x00, 0x00, 0x02}));
    call("GET", "/api/remote/state");
    CHECK(J["confirm"] == "observed" && J["unit_mode"] == "medium" && J["unit_confirmed"]==false, "legacy mode observation is not causal TX confirmation");
    g_ms += 2000;
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"timer\",\"minutes\":30}") == 202 && J["confirm"] == "not_available" &&
          g_sent[0x22F3] == 1, "timer: 22F3 once, no fake confirmation");
    g_ms += 2000;
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"away\"}") == 202, "away sent");
    g_ms += 11000; loopTick(); call("GET", "/api/remote/state");
    CHECK(J["confirm"] == "not_confirmed", "no report within 10 s -> not_confirmed");
    g_radioOk = false; g_ms += 2000;
    CHECK(call("POST", "/api/remote/send", "{\"key\":\"1\"}") == 502 && err("tx-failed"), "radio failure: 502");
    call("GET", "/api/remote/state");
    CHECK(J["accepted"] == true && J["sent"] == false && J["confirm"] == "none", "state: accepted, not sent");
#endif

#if ORCON_TX_POC
    { setupflow::Read read;read.st=setupflow::St::Done;read.step=setupflow::K_N;read.remote=REM;
      read.targetKnown=true;read.target=UNIT32;
      for(uint8_t k=0;k<setupflow::K_N;++k){read.got[k].cmd=k==setupflow::K_TIMER?0x22F3:0x22F1;read.got[k].len=k==setupflow::K_TIMER?7:3;read.got[k].pl[1]=setupflow::modeOfKey(k);read.got[k].pl[2]=7;}
      uint32_t id=0;CHECK(profiles::store().save(read,"Geteste familie",id)==profiles::Error::None,"captured family mapping stored");
      char body[48];snprintf(body,sizeof(body),"{\"id\":%u}",unsigned(id));
      CHECK(call("POST","/api/profiles/select",body)==200 && orcon::ids().us==US,"profile routes target without cloning remote");
      rx(fr(MsgType::I,REM,UNIT32,0x22F1,{0,1,7}));rx(fr(MsgType::I,UNIT32,UNIT32,0x31D9,{0,0,1}));g_ms+=4000;
      CHECK(call("POST","/api/remote/send","{\"key\":\"3\"}")==202 && txpoc::controller().status().result.request.tail==7,"active profile supplies its validated family byte to TX");
      CHECK(call("POST","/api/profiles/delete",body)==409 && err("session-busy"),"profile cannot change during open TX");
      loopTick();
      CHECK(call("POST","/api/profiles/delete",body)==200 && J["active"]==0,"deleting active profile restores base routing");
    }
#endif

    // Worst-case bounded state: twelve discovered remotes and four escaped names.
    { for(uint32_t n=0;n<profiles::LIMIT;++n) {
        setupflow::Read read;read.st=setupflow::St::Done;read.step=setupflow::K_N;read.remote=DeviceId(29,600+n);
        for(uint8_t k=0;k<setupflow::K_N;++k){read.got[k].cmd=k==setupflow::K_TIMER?0x22F3:0x22F1;read.got[k].len=k==setupflow::K_TIMER?7:3;read.got[k].pl[1]=setupflow::modeOfKey(k);read.got[k].pl[2]=4;}
        char name[profiles::NAME_BYTES+1];memset(name,'"',profiles::NAME_BYTES);name[0]=char('A'+n);name[profiles::NAME_BYTES]=0;uint32_t id=0;
        CHECK(profiles::store().save(read,name,id)==profiles::Error::None,"bounded profile capacity can store escaped names");
      }
      for(uint32_t n=0;n<12;++n)rx(fr(MsgType::I,DeviceId(29,700+n),UNIT,0x22F1,{0,1,4}));
      CHECK(call("GET","/api/setup/state")==200 && J["profiles"]["items"].size()==4 && J["remotes"].size()==12,
            "full discovery and profile response fits bounded JSON buffer");
      profileMemory.blob[0]^=1;CHECK(!profiles::store().begin(profileMemory),"corrupt profile storage fails closed");
#if ORCON_TX_POC
      rx(fr(MsgType::I,UNIT32,UNIT32,0x31D9,{0,0,1}));g_ms+=4000;
      CHECK(call("POST","/api/remote/send","{\"key\":\"2\"}")==503 && err("profile-storage-unavailable"),"corrupt profile storage blocks fallback TX");
#endif
    }

    // ---- pairing capture: complete 1FC9/10E0 frames (synthetic IDs only) -----------------------
    {
        call("GET", "/api/setup/capture"); const unsigned before = J["total"].as<unsigned>();
        Frame offer = fr(MsgType::I, REM, REM2, 0x1FC9, {});
        offer.len = 30; for (uint8_t i = 0; i < 30; ++i) offer.payload[i] = uint8_t(0xA0 + i);
        rx(offer); rx(fr(MsgType::W, UNIT, REM, 0x1FC9, {0x00, 0x31, 0xD9, 0x01, 0x02, 0x03}));
        rx(fr(MsgType::I, REM, UNIT, 0x1FC9, {0x00})); rx(press(0x04));
        Frame big = fr(MsgType::I, REM, REM2, 0x10E0, {}); big.len = 64; big.has_addr[2] = true; big.addr[2] = REM;
        for (uint8_t i = 0; i < 64; ++i) big.payload[i] = i; rx(big); loopTick();
        CHECK(call("GET", "/api/setup/capture") == 200 && J["total"].as<unsigned>() == before + 4, "capture: only 1FC9/10E0 frames counted");
        JsonArray fs = J["frames"].as<JsonArray>(); const size_t n = fs.size();
        CHECK(n >= 4 && fs[n - 4]["type"] == "I" && fs[n - 4]["cmd"] == "1FC9" && fs[n - 4]["len"] == 30 &&
              fs[n - 4]["pl"] == "A0A1A2A3A4A5A6A7A8A9AAABACADAEAFB0B1B2B3B4B5B6B7B8B9BABBBCBD" &&
              fs[n - 4]["addr"][0] == "29:000101" && fs[n - 4]["addr"][1] == "29:000303" && fs[n - 4]["addr"][2].isNull(),
              "capture: bind offer with every payload byte and full addresses (console stays masked)");
        CHECK(fs[n - 3]["type"] == "W" && fs[n - 3]["pl"] == "0031D9010203" && fs[n - 2]["pl"] == "00", "capture: unit reply and closing ack kept in order");
        CHECK(fs[n - 1]["len"] == 64 && std::string(fs[n - 1]["pl"].as<const char*>()).size() == 128, "capture: 64-byte payload complete");
        for (int i = 0; i < 9; ++i) rx(fr(MsgType::I, REM, UNIT, 0x1FC9, {uint8_t(i)}));
        loopTick(); call("GET", "/api/setup/capture");
        CHECK(J["frames"].size() == 8 && J["frames"][7]["pl"] == "08" && J["frames"][0]["pl"] == "01" && J["total"].as<unsigned>() == before + 13,
              "capture: bounded ring of 8, oldest dropped, total still counted");
        srv.req.clear(); srv.host = "attacker.example"; srv.routes["GET /api/setup/capture"]();
        CHECK(srv.code == 403, "capture: foreign Host refused (DNS rebinding)");
    }

    // ---- live RF console ------------------------------------------------------------------------
    {
        uint32_t cur = 0; console(&cur);                                  // skip everything so far
        std::string all;
        garbled(-83); loopTick(); std::string c = console(&cur); all += c;
        CHECK(has(c, "RAW|sync #") && has(c, "-83 dBm pre=FF") && !has(c, "geen geldig frame"), "console: raw sync shown at once");
        g_ms += 1600; loopTick(); c = console(&cur); all += c;
        CHECK(has(c, "RAW|sync #") && has(c, "-83 dBm: geen geldig frame"), "console: sync without a clean frame marked undecoded");
        rx(press(0x04)); g_ms += 1600; loopTick(); c = console(&cur); all += c;
        CHECK(has(c, "RX|#") && has(c, " I 22F1 29:****01>29:****02 pl=000404"), "console: decoded remote frame, masked IDs");
        CHECK(has(c, "INFO|29:****01 knop Auto: in de lijst gehoorde afstandsbedieningen") && !has(c, "geen geldig frame"),
              "console: registered in the heard list; decoded sync not marked undecoded");
        CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:000101\"}") == 201, "console: read session started");
        loopTick(); g_ms += 400;
        rx(fr(MsgType::I, REM2, UNIT, 0x22F1, {0x00, 0x00, 0x04}));
        rx(press(0x03));
        rx(press(0x00)); rx(press(0x00));
        rx(fr(MsgType::I, UNIT, UNIT, 0x31D9, {0x00, 0x00, 0x01}));
        c = console(&cur); all += c;
        CHECK(has(c, "INFO|uitlezen gestart (sessie") && has(c, "druk Afwezig"), "console: session start");
        CHECK(has(c, "FILTER|29:****03 telt niet: niet de gekozen afstandsbediening"), "console: other remote filtered with reason");
        CHECK(has(c, "FILTER|29:****01 telt niet: knop 3, verwacht Afwezig"), "console: wrong key filtered with reason");
        CHECK(has(c, "OK|29:****01 knop Afwezig geaccepteerd: stap 1/6"), "console: accepted press");
        CHECK(has(c, "FILTER|29:****01 telt niet: herhaling van de vorige druk"), "console: repeat filtered");
        CHECK(has(c, "RX|#") && has(c, " 31D9 29:****02>"), "console: unit status shown as decoded frame only");
        call("GET", "/api/setup/read/state"); char b[48]; snprintf(b, sizeof(b), "{\"sid\":%u}", J["sid"].as<unsigned>());
        call("POST", "/api/setup/read/cancel", b); loopTick(); all += console(&cur);
        CHECK(has(all, "INFO|uitlezen geannuleerd"), "console: cancel");
        g_ovf += 2; g_noteDrops += 3; g_rxState = 1; g_ms += 1100; loopTick(); c = console(&cur); all += c;
        CHECK(has(c, "WARN|UART-overflow +2") && has(c, "WARN|console miste 3 syncs") && has(c, "ERR|radio herstelt"), "console: health deltas");
        g_rxState = 0; g_ms += 1100; loopTick(); c = console(&cur); all += c;
        CHECK(has(c, "INFO|radio weer ontvangend"), "console: recovery shown");
        g_ms += 31000; loopTick(); c = console(&cur); all += c;
        CHECK(has(c, "INFO|laatste ") && has(c, "zonder geldig frame; ruis -101 dBm"), "console: 30 s summary");
        for (int i = 0; i < 9; i++) garbled(-90);
        loopTick(); c = console(&cur); all += c;
        CHECK(has(c, "niet meer gevolgd (te veel tegelijk)"), "console: bounded open-sync table");
        CHECK(!has(all, "000101") && !has(all, "000202") && !has(all, "000303") && !has(all, TOKEN) && !has(all, "192.0.2"),
              "console: no full device IDs, token or IP");
        CHECK(call("GET", "/api/setup/console?after=abc") == 200 && J["ev"].size() > 0, "console: bad cursor -> from the oldest");
        call("GET", "/api/setup/console?after=999999999");
        CHECK(J["ev"].size() > 0 && J["lost"].as<uint32_t>() > 0, "console: cursor from an earlier boot restarts, reports the gap");
        CHECK(J["ev"].size() <= 16 && srv.out.size() < 3072, "console: bounded reply");
        srv.req.clear(); srv.args.clear(); srv.host = "attacker.example"; srv.routes["GET /api/setup/console"]();
        CHECK(srv.code == 403, "console: DNS rebinding refused");
        CHECK(srv.routes.count("POST /api/setup/console") == 0, "console: read-only route");
#if RX_ONLY
        CHECK(g_sent.empty() && orcon::state().txCount == 0, "console: still no TX in RX-only");
#endif
    }

#if ORCON_OWNER_MODEL
    {   // The ownership routes on an installed (here: migrated) device, from the home network.
        g_viaAp = false;
        CHECK(call("GET", "/api/provision/state", "", nullptr) == 403 && err("setup-ap-only"), "owner model: wizard state not on the home network");
        CHECK(call("POST", "/api/provision/password", "{\"password\":\"overname-poging\"}", nullptr) == 403 && err("setup-ap-only"),
              "owner model: no first password from the home network");
        g_ownState = provision::State::Done;
        CHECK(call("POST", "/api/auth/nonce", "{}", nullptr, "application/json", nullptr) == 403 && err("owner-key-required"),
              "owner model: no release-key recovery once installed with an owner key");
        g_ownState = provision::State::Legacy;
    }
    {   // Codex OWNER-11: NVS lost its owner (Arduino auto-format) while the sentinel says owned -> Locked.
        g_ownState = provision::State::Locked; g_viaAp = true; g_physical = true;
        CHECK(call("GET", "/api/provision/state", "", nullptr) == 200 && J["state"] == "locked", "locked: the state says so (on the AP)");
        CHECK(call("POST", "/api/provision/password", "{\"password\":\"overname-poging\"}", nullptr) == 403 && err("factory-reset-required"),
              "locked: no first password, even on the AP in the recovery mode");
        CHECK(call("POST", "/api/provision/recover", "{\"pem\":\"x\",\"passphrase\":\"y\",\"password\":\"overname-poging\"}", nullptr) == 403 &&
              err("factory-reset-required"), "locked: no recovery");
        CHECK(call("POST", "/api/auth/nonce", "{}", nullptr, "application/json", nullptr) == 403 && err("owner-key-required"),
              "locked: no release-key recovery");
        g_ownState = provision::State::Legacy; g_viaAp = false; g_physical = false;
    }
    {   // Codex OWNER-03: a release-key code issued on a Legacy device dies when the owner registers a key.
        g_viaAp = true; g_physical = true; g_cookie.clear();
        auto issue = []() {
            call("POST", "/api/auth/nonce", "{}", nullptr, "application/json", nullptr);
            const std::string n = J["nonce"] | "";
            JsonDocument d; d["manifest"] = "{\"v\":1,\"kind\":\"orcon-auth-reset\",\"mac\":\"02:00:00:00:00:0a\",\"nonce\":\"" + n + "\"}"; d["sig"] = "a1b2";
            std::string b; serializeJson(d, b);
            call("POST", "/api/auth/reset", b.c_str(), nullptr, "application/json", nullptr);
            return std::string(J["code"] | "");
        };
        const std::string first = issue();
        CHECK(first.size() == 9 && call("POST", "/api/auth/setup", ("{\"code\":\"" + first + "\",\"password\":\"legacy-wachtwoord\"}").c_str(), nullptr) == 200,
              "Legacy: the release-key code still sets a password (until an owner key exists)");
        const std::string tok = J["token"] | "";
        const std::string stale = issue();                               // e.g. a release-key holder on the AP
        CHECK(stale.size() == 9, "Legacy: a second code is issued");
        CHECK(call("POST", "/api/provision/key", "{\"passphrase\":\"eigen sleutelzin\"}", tok.c_str()) == 200, "recovery mode: the owner makes a key");
        call("GET", "/api/provision/key/file", "", nullptr);
        JsonDocument v; v["pem"] = J["pem"].as<const char*>(); v["passphrase"] = "eigen sleutelzin";
        std::string vb; serializeJson(v, vb);
        CHECK(call("POST", "/api/provision/key/verify", vb.c_str(), tok.c_str()) == 200 && g_ownState == provision::State::Done, "owner key registered: Done");
        g_cookie.clear();
        CHECK(call("POST", "/api/auth/setup", ("{\"code\":\"" + stale + "\",\"password\":\"aanvaller-wachtw\"}").c_str(), nullptr) == 403 &&
              err("owner-key-required"), "the stale release-key code no longer sets a password");
        CHECK(call("POST", "/api/auth/login", "{\"password\":\"aanvaller-wachtw\"}", nullptr) == 401, "the attacker's password never took effect");
        g_ownState = provision::State::Legacy; g_physical = false; g_viaAp = false;
    }
#endif
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
