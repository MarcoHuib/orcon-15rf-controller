// setup_web.cpp — HTTP glue for the setup wizard and the virtual remote. Session logic
// lives in setup_flow.{h,cpp} (host-tested); contracts in docs/SETUP_WIZARD.md.
// Frames come from the existing RX drain in main.cpp; nothing here touches the radio
// except the TX builds' existing orcon::setMode/setTimer/startPairing.
#include "setup_web.h"
#include "setup_flow.h"
#include "config.h"
#include "net.h"
#include "orcon.h"
#include "orcon_log.h"
#include "ramses_radio.h"
#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <stdarg.h>
#include <stdio.h>
#include "setup_page.h"            // after Arduino.h: PROGMEM comes from the core

#if !ORCON_TX_POC   // the dry-run PoC keeps its own isolated route table
namespace setupweb {
using namespace setupflow;

// Capabilities from the build, never from the page. The SX1262 path cannot decode RAMSES.
#if defined(RADIO_SX1262)
static const bool CAN_RX = false;
#else
static const bool CAN_RX = true;
#endif
static const bool CAN_TX = !RX_ONLY;

static WebServer* g_srv = nullptr;
static char     g_token[33] = "";
static uint32_t g_sid = 0, g_ev = 0;
static Read     g_read;
static Pair     g_pair;
static Cmd      g_cmd;
static char     g_out[1536];                  // one response at a time (loop task only)

struct Out {
    size_t n = 0; bool ok = true;
    void add(const char* fmt, ...) {
        if (!ok) return;
        va_list a; va_start(a, fmt);
        int k = vsnprintf(g_out + n, sizeof(g_out) - n, fmt, a);
        va_end(a);
        if (k < 0 || size_t(k) >= sizeof(g_out) - n) ok = false; else n += size_t(k);
    }
};
static void reply(int code, const Out& o) {
    g_srv->sendHeader("Cache-Control", "no-store");
    if (!o.ok) { g_srv->send(500, "application/json", "{\"error\":\"response-too-large\"}"); return; }
    g_srv->send(code, "application/json", g_out);
}
static void fail(int code, const char* err) { Out o; o.add("{\"error\":\"%s\"}", err); reply(code, o); }

static void idStr(const ramses::DeviceId& d, char (&out)[16]) { snprintf(out, sizeof(out), "%02u:%06lu", d.cls, (unsigned long)d.serial); }
static uint32_t seqNow() { return radio::rxDiagnostics().coreSyncHits; }   // 0 on radios without sync numbers
static const char* rfState() {
    if (!CAN_RX) return "unsupported";
    static const char* const S[] = { "receiving", "recovering", "failed" };
    uint8_t s = radio::rxDiagnostics().rxState;
    return S[s < 3 ? s : 2];
}
static bool pairActive() { return g_pair.st == St::Active; }
static bool fwPaired() { return orcon::pairState() == orcon::PairState::Paired; }

// ---- POST guard: JSON, same origin, per-boot token, bounded body, known keys only --------
static bool hostOk() { return hostAllowed(g_srv->hostHeader().c_str(), net::ipAddr()); }
static bool guard(JsonDocument& d, const char* const* keys, size_t nkeys) {
    if (!hostOk() || !requestAllowed(g_srv->header("Content-Type").c_str(), g_srv->header("Origin").c_str(),
                        g_srv->hostHeader().c_str())) { fail(403, "forbidden-request"); return false; }
    if (!tokenEqual(g_srv->header("X-Orcon-Token").c_str(), g_token)) { fail(403, "bad-token"); return false; }
    if (g_srv->arg("plain").length() > 160) { fail(413, "body-too-large"); return false; }
    if (deserializeJson(d, g_srv->arg("plain")) || !d.is<JsonObject>()) { fail(400, "bad-json"); return false; }   // same instantiation as net.cpp
    for (JsonPair kv : d.as<JsonObject>()) {
        bool ok = false;
        for (size_t i = 0; i < nkeys; i++) if (!strcmp(kv.key().c_str(), keys[i])) ok = true;
        if (!ok) { fail(400, "unknown-field"); return false; }
    }
    return true;
}
static bool sidOk(JsonDocument& d, uint32_t sid) { return d["sid"].is<uint32_t>() && d["sid"].as<uint32_t>() == sid; }

// ---- page -----------------------------------------------------------------------
static void handlePage() {
    if (g_srv->header("Accept-Encoding").indexOf("gzip") < 0) {   // stored gzip-only in flash
        g_srv->send(406, "text/plain", "gzip required"); return;
    }
    g_srv->sendHeader("Content-Encoding", "gzip");
    g_srv->sendHeader("Cache-Control", "no-cache");
    g_srv->sendHeader("X-Content-Type-Options", "nosniff");
    g_srv->sendHeader("X-Frame-Options", "DENY");
    g_srv->sendHeader("Content-Security-Policy", "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
    g_srv->send_P(200, "text/html; charset=utf-8", (PGM_P)SETUP_PAGE_GZ, SETUP_PAGE_GZ_LEN);
}

// ---- /api/setup/state -------------------------------------------------------------
static void handleState() {
    const orcon::State& s = orcon::state();
    const uint32_t now = millis();
    Out o;
    o.add("{\"fw\":\"%s\",\"rx_only\":%s,\"caps\":{\"rx_supported\":%s,\"tx_supported\":%s,"
          "\"read_supported\":%s,\"pair_supported\":%s,\"control_supported\":%s},"
          "\"rf\":\"%s\",\"token\":\"%s\",\"mask_ids\":%s,\"active\":\"%s\"",
          FW_VERSION, RX_ONLY ? "true" : "false", CAN_RX ? "true" : "false", CAN_TX ? "true" : "false",
          CAN_RX ? "true" : "false", (CAN_TX && CAN_RX) ? "true" : "false", CAN_TX ? "true" : "false",
          rfState(), hostOk() ? g_token : "", net::maskIds() ? "true" : "false",
          g_read.st == St::Active ? "read" : pairActive() ? "pair" : "none");
    char id[16] = "";
    if (s.fanKnown) idStr(orcon::ids().fan, id);
    o.add(",\"unit\":{\"known\":%s,\"id\":\"%s\",\"mode\":\"%s\",\"age_s\":%ld}",
          s.fanKnown ? "true" : "false", id, orcon::modeName(s.mode),
          s.modeMs ? long((now - s.modeMs) / 1000) : -1L);
    o.add(",\"remotes\":[");
    const rlog::Device* dev = nullptr;
    const size_t n = rlog::devices(&dev);
    bool first = true;
    for (size_t i = 0; i < n; i++) {               // discovery only: the user picks one explicitly
        if (!(dev[i].roles & rlog::ROLE_REMOTE) || now - dev[i].lastMs > 1800000UL) continue;
        idStr(dev[i].id, id);
        o.add("%s{\"id\":\"%s\",\"age_s\":%lu,\"rssi\":%d,\"cmd\":\"%04X\",\"n\":%lu}", first ? "" : ",", id,
              (unsigned long)((now - dev[i].lastMs) / 1000), dev[i].rssi, dev[i].lastCmd, (unsigned long)dev[i].count);
        first = false;
    }
    o.add("]}");
    reply(200, o);
}

// ---- read session -------------------------------------------------------------------
static void readBody(int code) {
    const Read& r = g_read;
    char id[16] = "";
    if (r.sid) idStr(r.remote, id);
    Out o;
    o.add("{\"sid\":%lu,\"status\":\"%s\",\"step\":%u,\"remote\":\"%s\",\"left_s\":%lu,\"ev\":%lu,"
          "\"rf\":\"%s\",\"others\":%lu,\"why\":\"%s\",\"why_ev\":%lu,\"why_cmd\":\"%04X\",\"why_mode\":%d,\"steps\":[",
          (unsigned long)r.sid, stName(r.st), r.step, id, (unsigned long)(r.leftMs(millis()) / 1000),
          (unsigned long)g_ev, rfState(), (unsigned long)r.others, whyName(r.why), (unsigned long)r.whyEv,
          r.whyCmd, r.whyMode == 0xFF ? -1 : r.whyMode);
    for (uint8_t k = 0; k < K_N; k++) {
        const Step& s = r.got[k];
        if (k >= r.step) { o.add("%s{\"key\":\"%s\",\"ok\":false}", k ? "," : "", KEY_NAME[k]); continue; }
        char pl[17] = "";
        for (uint8_t i = 0; i < s.len && i < 8; i++) snprintf(pl + 2 * i, 3, "%02X", s.pl[i]);
        o.add("%s{\"key\":\"%s\",\"ok\":true,\"cmd\":\"%04X\",\"pl\":\"%s\",\"rssi\":%d,\"ev\":%lu}",
              k ? "," : "", KEY_NAME[k], s.cmd, pl, s.rssi, (unsigned long)s.ev);
    }
    o.add("]}");
    reply(code, o);
}
static void handleReadState() { readBody(200); }

static void handleReadStart() {
    static const char* const K[] = { "remote" };
    JsonDocument d;
    if (!guard(d, K, 1)) return;
    if (!CAN_RX) { fail(501, "read-unsupported"); return; }
    if (g_read.st == St::Active || pairActive()) { fail(409, "session-busy"); return; }
    ramses::DeviceId id;
    if (!d["remote"].is<const char*>() || !parseId(d["remote"].as<const char*>(), id)) { fail(400, "bad-remote"); return; }
    const rlog::Device* dev = nullptr;
    const size_t n = rlog::devices(&dev);
    bool heard = false;
    for (size_t i = 0; i < n; i++)
        if (dev[i].id == id && (dev[i].roles & rlog::ROLE_REMOTE) && millis() - dev[i].lastMs <= 1800000UL) heard = true;
    if (!heard) { fail(404, "remote-not-heard"); return; }
    if (radio::rxDiagnostics().rxState != 0) { fail(503, "rf-unavailable"); return; }
    g_read.start(++g_sid, id, millis(), seqNow());
    readBody(201);
}

static void handleReadCancel() {
    static const char* const K[] = { "sid" };
    JsonDocument d;
    if (!guard(d, K, 1)) return;
    if (g_read.st != St::Active || !sidOk(d, g_read.sid)) { fail(409, "no-such-session"); return; }
    g_read.cancel();
    readBody(200);
}

// ---- pairing (TX builds) --------------------------------------------------------------
static void pairBody(int code) {
    const bool fp = fwPaired();
    const orcon::Ids& ids = orcon::ids();
    Out o;
    o.add("{\"supported\":%s,\"sid\":%lu,\"status\":\"%s\",\"bind_sent\":%s,\"rf_confirmed\":%s,\"paired\":%s,\"left_s\":%lu}",
          (CAN_TX && CAN_RX) ? "true" : "false", (unsigned long)g_pair.sid, g_pair.status(fp, ids.fan),
          g_pair.sent ? "true" : "false", g_pair.confirmed ? "true" : "false",
          g_pair.paired(fp, ids.fan) ? "true" : "false", (unsigned long)(g_pair.leftMs(millis()) / 1000));
    reply(code, o);
}
static void handlePairState() { pairBody(200); }

static void handlePairStart() {
    JsonDocument d;
    if (!guard(d, nullptr, 0)) return;
    if (!CAN_TX || !CAN_RX) { fail(501, "tx-unavailable"); return; }
    if (g_read.st == St::Active || pairActive()) { fail(409, "session-busy"); return; }
    g_pair.start(++g_sid, millis());
    pairBody(201);
}

static void handlePairPress() {
    static const char* const K[] = { "sid", "keys" };
    JsonDocument d;
    if (!guard(d, K, 2)) return;
    if (!CAN_TX || !CAN_RX) { fail(501, "tx-unavailable"); return; }
    if (!pairActive() || !sidOk(d, g_pair.sid)) { fail(409, "no-such-session"); return; }
    JsonArray a = d["keys"].as<JsonArray>();
    if (a.isNull() || a.size() == 0 || a.size() > K_N) { fail(400, "bad-keys"); return; }
    uint8_t mask = 0;
    for (JsonVariant v : a) {
        uint8_t k = v.is<const char*>() ? keyFromName(v.as<const char*>()) : uint8_t(K_NONE);
        if (k == K_NONE || (mask & (1u << k))) { fail(400, "bad-keys"); return; }
        mask |= uint8_t(1u << k);
    }
    // One request carries the whole simultaneous combination; separate presses never add up.
    if (!Pair::isBindCombo(mask)) { fail(422, "unsupported-combination"); return; }
    if (g_pair.sent || g_pair.txFailed) { fail(409, "already-sent"); return; }   // no retry
#if !RX_ONLY
    const uint32_t before = orcon::state().txCount;
    orcon::startPairing();                          // existing 1FC9 bind offer + 3-min window
    g_pair.bindResult(orcon::state().txCount > before, millis());
#endif
    if (!g_pair.sent) { fail(502, "tx-failed"); return; }
    pairBody(200);
}

static void handlePairCancel() {
    static const char* const K[] = { "sid" };
    JsonDocument d;
    if (!guard(d, K, 1)) return;
    if (!pairActive() || !sidOk(d, g_pair.sid)) { fail(409, "no-such-session"); return; }
    g_pair.cancel();                                // a sent offer cannot be recalled; the window just runs out
    pairBody(200);
}

// ---- virtual remote (TX builds only) ------------------------------------------------------
#if RX_ONLY
static void handleRemoteOff() { fail(501, "tx-unavailable"); }
#else
static void remoteBody(int code) {
    const orcon::State& s = orcon::state();
    const uint32_t now = millis();
    Out o;
    o.add("{\"supported\":true,\"id\":%lu,\"key\":\"%s\",\"minutes\":%u,\"accepted\":%s,\"sent\":%s,"
          "\"confirm\":\"%s\",\"age_s\":%lu,\"unit_mode\":\"%s\",\"unit_age_s\":%ld}",
          (unsigned long)g_cmd.id, g_cmd.key < K_N ? KEY_NAME[g_cmd.key] : "", g_cmd.minutes,
          g_cmd.id ? "true" : "false", g_cmd.sent ? "true" : "false", confirmName(g_cmd.confirm),
          (unsigned long)(g_cmd.id ? g_cmd.ageMs(now) / 1000 : 0), orcon::modeName(s.mode),
          s.modeMs ? long((now - s.modeMs) / 1000) : -1L);
    reply(code, o);
}
static void handleRemoteState() { remoteBody(200); }

static void handleRemoteSend() {
    static const char* const K[] = { "key", "minutes" };
    JsonDocument d;
    if (!guard(d, K, 2)) return;
    const uint8_t k = d["key"].is<const char*>() ? keyFromName(d["key"].as<const char*>()) : uint8_t(K_NONE);
    if (k == K_NONE) { fail(400, "bad-key"); return; }
    uint16_t minutes = 0;
    if (k == K_TIMER) {                             // the 15RF timer: high speed for 15, 30 or 60 min
        minutes = d["minutes"].is<uint16_t>() ? d["minutes"].as<uint16_t>() : 0;
        if (minutes != 15 && minutes != 30 && minutes != 60) { fail(400, "bad-minutes"); return; }
    } else if (!d["minutes"].isNull()) { fail(400, "bad-minutes"); return; }
    if (!orcon::state().fanKnown) { fail(409, "unit-not-selected"); return; }
    if (g_read.st == St::Active || pairActive()) { fail(409, "session-busy"); return; }
    const uint32_t now = millis();
    if (g_cmd.tooSoon(now)) { fail(429, "too-fast"); return; }
    const uint32_t before = orcon::state().txCount, seq = seqNow();
    const bool ok = k == K_TIMER ? orcon::setTimer(minutes) : orcon::setMode(orcon::Mode(modeOfKey(k)));
    g_cmd.record(g_cmd.id + 1, k, minutes, ok && orcon::state().txCount > before, millis(), seq);
    if (!g_cmd.sent) { fail(502, "tx-failed"); return; }
    remoteBody(202);
}
#endif

// ---- wiring ------------------------------------------------------------------------
void begin(WebServer& s) {
    g_srv = &s;
    uint8_t b[16]; esp_fill_random(b, sizeof(b));
    static const char HEX_[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { g_token[2 * i] = HEX_[b[i] >> 4]; g_token[2 * i + 1] = HEX_[b[i] & 15]; }
    g_token[32] = 0;
    static const char* headers[] = { "Origin", "Content-Type", "X-Orcon-Token", "Accept-Encoding" };
    s.collectHeaders(headers, sizeof(headers) / sizeof(headers[0]));
    s.on("/", HTTP_GET, handlePage);
    s.on("/api/setup/state",       HTTP_GET,  handleState);
    s.on("/api/setup/read/start",  HTTP_POST, handleReadStart);
    s.on("/api/setup/read/state",  HTTP_GET,  handleReadState);
    s.on("/api/setup/read/cancel", HTTP_POST, handleReadCancel);
    s.on("/api/setup/pair/start",  HTTP_POST, handlePairStart);
    s.on("/api/setup/pair/press",  HTTP_POST, handlePairPress);
    s.on("/api/setup/pair/state",  HTTP_GET,  handlePairState);
    s.on("/api/setup/pair/cancel", HTTP_POST, handlePairCancel);
#if RX_ONLY
    s.on("/api/remote/send",  HTTP_POST, handleRemoteOff);   // explicit refusal, no TX code linked
    s.on("/api/remote/state", HTTP_GET,  handleRemoteOff);
#else
    s.on("/api/remote/send",  HTTP_POST, handleRemoteSend);
    s.on("/api/remote/state", HTTP_GET,  handleRemoteState);
#endif
}

void onFrame(const ramses::Frame& f, uint32_t seq, int16_t rssi) {
    ++g_ev;                                         // monotonic per boot: orders events for the page
    g_read.onFrame(f, g_ev, seq, millis(), rssi);
    g_pair.onFrame(f, orcon::ids().us);
    if (f.cmd == 0x31D9 && f.len >= 3 && f.has_addr[0] && orcon::state().fanKnown && f.addr[0] == orcon::ids().fan &&
        (f.type == ramses::MsgType::I || f.type == ramses::MsgType::RP))
        g_cmd.onUnitMode(f.payload[2], seq);
}

void tick(uint32_t now) {
    g_read.tick(now);
    g_pair.tick(now, fwPaired(), orcon::ids().fan);
    g_cmd.tick(now);
}

} // namespace setupweb
#endif // !ORCON_TX_POC
