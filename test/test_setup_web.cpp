// test_setup_web.cpp — the real setup_web.cpp + orcon.cpp behind a fake WebServer and fake
// radio. Built twice: -DRX_ONLY=1 (Heltec V2) and -DRX_ONLY=0 (TX builds).
// Covers: page/gzip headers, capabilities, POST guard (token, origin, content type, size,
// unknown fields), input validation, session conflicts, GET without side effects, RX
// pipeline integration, pairing statuses, virtual remote statuses and RX-only refusals.
#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "setup_web.h"
#include "setup_page.h"
#include "orcon.h"
#include "orcon_log.h"
#include "ramses_radio.h"
#include "net.h"
#include <map>
#include <string>
#include <vector>
using namespace ramses;

static unsigned long g_ms = 100000;
unsigned long millis() { return g_ms; }
void delay(unsigned long ms) { g_ms += ms; }
static std::map<uint16_t, int> g_sent;       // frames the fake radio transmitted, per code
static bool g_radioOk = true;
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
namespace net { bool maskIds() { return true; } const char* ipAddr() { return "192.0.2.5"; } }

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
// What main.cpp does for every decoded frame: orcon first, then the wizard, same order.
// The RX task queues a sync note, then the decoded frame; main.cpp drains frames in order.
static void rx(const Frame& f) {
    g_ms += 400; ++g_syncs; ++g_frames;
    radio::SyncInfo si; si.seq = g_syncs; si.ms = g_ms; si.rssi = -61; si.pre = 0x55; g_notes.push_back(si);
    orcon::onFrame(f); setupweb::onFrame(f, g_syncs, -61);
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
static void loopTick() { setupweb::tick(g_ms); }
static Frame press(uint8_t mode) { return fr(MsgType::I, REM, UNIT, 0x22F1, {0x00, mode, 0x04}); }

int main() {
    printf("Setup web API tests (RX_ONLY=%d)\n", RX_ONLY);
    orcon::begin(37, 404, 29, 202);
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
    CHECK(J["rx_only"] == bool(RX_ONLY) && J["caps"]["tx_supported"] == !RX_ONLY && J["caps"]["pair_supported"] == !RX_ONLY &&
          J["caps"]["control_supported"] == !RX_ONLY && J["caps"]["read_supported"] == true, "capabilities come from the build");
    CHECK(J["token"] == TOKEN && J["active"] == "none" && J["unit"]["id"] == "29:000202", "token, session and selected unit reported");
    CHECK(J["remotes"].size() == 0, "no remote heard yet");
    srv.req.clear(); srv.host = "attacker.example"; srv.routes["GET /api/setup/state"](); deserializeJson(J, srv.out);
    CHECK(J["token"] == "", "DNS rebinding: a foreign Host never receives the token");
    rx(press(0x01));                                          // the user presses a key: discovered
    rx(fr(MsgType::I, UNIT, UNIT, 0x31D9, {0x00, 0x00, 0x01}));
    call("GET", "/api/setup/state");
    CHECK(J["remotes"].size() == 1 && J["remotes"][0]["id"] == "29:000101", "remote discovered from its 22F1, unit not listed as remote");

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

    // ---- read session through the RX pipeline -------------------------------------------------
    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:000101\"}") == 201 && J["status"] == "active" && J["step"] == 0,
          "start: 201 active, step 0");
    const uint32_t sid = J["sid"];
    CHECK(call("POST", "/api/setup/read/start", "{\"remote\":\"29:000101\"}") == 409 && err("session-busy"), "second session refused");
    CHECK(call("POST", "/api/setup/pair/start") == (RX_ONLY ? 501 : 409), "pairing during reading refused");
    rx(fr(MsgType::I, REM2, UNIT, 0x22F1, {0x00, 0x00, 0x04}));         // neighbour presses the right key
    rx(press(0x03));                                                    // ours, wrong key
    call("GET", "/api/setup/read/state");
    CHECK(J["step"] == 0 && J["others"] == 1 && J["why"] == "wrong-button" && J["why_mode"] == 3, "other device and wrong key do not count");
    const uint8_t modes[] = { 0x00, 0x04, 0xFF, 0x01, 0x02, 0x03 };
    for (int k = 0; k < 6; k++) {
        rx(modes[k] == 0xFF ? fr(MsgType::I, REM, UNIT, 0x22F3, {0x00, 0x12, 0x1E, 0x03, 0x04, 0x04, 0x04}) : press(modes[k]));
        rx(modes[k] == 0xFF ? fr(MsgType::I, REM, UNIT, 0x22F3, {0x00, 0x12, 0x1E, 0x03, 0x04, 0x04, 0x04}) : press(modes[k]));   // repeat
        rx(fr(MsgType::I, UNIT, UNIT, 0x31D9, {0x00, 0x00, 0x01}));     // the unit answers: no evidence
        loopTick();
    }
    call("GET", "/api/setup/read/state");
    CHECK(J["status"] == "done" && J["step"] == 6 && J["steps"][2]["cmd"] == "22F3" && J["steps"][5]["pl"] == "000304" &&
          J["steps"][0]["rssi"] == -61, "six presses -> done with commands, payloads, RSSI");
    CHECK(J["steps"][0]["ev"].as<uint32_t>() < J["steps"][5]["ev"].as<uint32_t>(), "event ids increase");
    CHECK(call("POST", "/api/setup/read/cancel", "{\"sid\":1}") == 409, "cancel of a finished session refused");

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

#if RX_ONLY
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
    CHECK(J["confirm"] == "confirmed" && J["unit_mode"] == "medium", "unit's own 31D9 with the mode -> confirmed");
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
