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
#endif
#include <map>
#include <string>
#include <vector>
using namespace ramses;

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
#if ORCON_TX_RF_ENABLE
static uint32_t g_restoreDiag = 0;
namespace radio { namespace sx1276 { uint32_t restoreDiag() { return g_restoreDiag; } } }
#endif
static bool g_wifiFake = true;
static int g_restarts = 0, g_forgets = 0;
namespace net { bool apMode() { return g_apModeFake; } bool maskIds() { return true; } const char* ipAddr() { return "192.0.2.5"; }
                bool wifiConnected() { return g_wifiFake && !g_apModeFake; } const char* ssidName() { return "Thuis \"5G\"\x01"; }
                void restartSoon() { ++g_restarts; } void forgetWifi() { ++g_forgets; } }

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
}
#endif

static int failures = 0;
#define CHECK(c,m) do{ if(!(c)){printf("  [FAIL] %s (line %d)\n",m,__LINE__);failures++;} else printf("  [ ok ] %s\n",m);}while(0)

static WebServer srv;
static JsonDocument J;
static const char* TOKEN = "a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5";
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
    auto it = srv.routes.find(std::string(method) + " " + p);
    if (it == srv.routes.end()) return 404;
    it->second();
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
static void rx(const Frame& f) {
    g_ms += 400; ++g_syncs; ++g_frames;
    radio::SyncInfo si; si.seq = g_syncs; si.ms = g_ms; si.rssi = -61; si.pre = 0x55; g_notes.push_back(si);
    orcon::onFrame(f); setupweb::onFrame(f, g_syncs, -61);
#if ORCON_TX_POC
    txpoc::controller().onFrame(f, g_ms, g_ms, orcon::ids());
#endif
}
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
int main() {
    printf("Setup web API tests (RX_ONLY=%d)\n", RX_ONLY);
    orcon::begin(37, 404, 29, 202);
    profiles::store().begin(profileMemory);
    setupweb::restoreProfileSelection();
    setupweb::begin(srv);

    // ---- page ------------------------------------------------------------------------------
    srv.req.clear();
    CHECK(call("GET", "/", "", nullptr, nullptr, nullptr) == 406, "page without gzip support: 406, never raw bytes");
    { srv.req.clear(); srv.req["Accept-Encoding"] = "gzip, deflate"; srv.body = ""; srv.routes["GET /"]();
      CHECK(srv.code == 200 && srv.sent["Content-Encoding"] == "gzip" && srv.out.size() == SETUP_PAGE_GZ_LEN,
            "page served from flash as gzip with Content-Encoding");
      CHECK(srv.type == "text/html; charset=utf-8" && srv.sent.count("Content-Security-Policy") && srv.sent["X-Frame-Options"] == "DENY",
            "page has charset, CSP and frame protection"); }

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
    CHECK(J["token"] == "", "DNS rebinding: a foreign Host never receives the token");
    rx(press(0x01));                                          // the user presses a key: discovered
    rx(fr(MsgType::I, UNIT, UNIT, 0x31D9, {0x00, 0x00, 0x01}));
    call("GET", "/api/setup/state");
    CHECK(J["remotes"].size() == 1 && J["remotes"][0]["id"] == "29:000101", "remote discovered from its 22F1, unit not listed as remote");
    CHECK(J["wifi"]["connected"] == true && J["wifi"]["ssid"] == "Thuis \"5G\"\x01" && J["caps"]["settings_pages"] == !ORCON_TX_POC,
          "state: WiFi network (JSON-escaped) and whether the classic settings pages exist in this build");
    g_apModeFake = true; call("GET", "/api/setup/state");
    CHECK(J["wifi"]["connected"] == false && J["wifi"]["ssid"] == "", "setup AP: reported as not connected, no network name");
    CHECK(J["wifi"]["ap"] == true && J["wifi"]["ap_name"] == WIFI_AP_NAME && J["wifi"]["ip"] == "192.0.2.5", "setup AP: AP flag, AP name and address reported");
    g_apModeFake = false;
#if ORCON_TX_POC
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
            has(all, "koppelen: unit bevestigde (W 1FC9 00 31D9)") && has(all, "koppelen: 1FC9-afsluiting verzonden (RF)"),
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
        rx(fr(MsgType::W, UNIT32, US, 0x1FC9, {0x00, 0x31, 0xD9, 0x80, 0, 0xCA}));   // queued before the owner runs
        loopTick(); loopTick(); call("GET", "/api/setup/pair/state");
        if (sentOk) CHECK(txpoc::s_rf.sent[1] == offers + 1 && J["rf_confirmed"] == true && J["paired"] == true && txpoc::s_rf.sent[2] == acks + 1,
                          "W before the offer result is published: still confirmed, one ack, paired");
        else CHECK(J["status"] == "tx_failed" && J["rf_confirmed"] == false && txpoc::s_rf.sent[2] == acks,
                   "offer not on air: an early W is discarded and no ack is sent");
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

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
