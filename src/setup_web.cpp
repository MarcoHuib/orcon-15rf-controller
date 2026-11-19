// setup_web.cpp — HTTP glue for the setup wizard and the virtual remote. Session logic
// lives in setup_flow.{h,cpp} (host-tested); contracts in docs/SETUP_WIZARD.md.
// Frames come from the existing RX drain in main.cpp; nothing here touches the radio
// directly: TX builds use the existing orcon::setMode/setTimer/startPairing, and
// ORCON_WEB_CONTROL builds use the txpoc control layer (one transaction per action).
#include "setup_web.h"
#include "setup_flow.h"
#include "remote_profiles.h"
#include "rf_console.h"
#include "config.h"
#include "net.h"
#include "orcon.h"
#include "orcon_log.h"
#include "ramses_radio.h"
#if ORCON_TX_POC
#include "tx_poc_runtime.h"
#include "tx_budget.h"
#endif
#if ORCON_WEB_AUTH
#include "web_auth.h"
#include "auth_store.h"
#endif
#if ORCON_OWNER_MODEL
#include "owner_key.h"
#include "owner_store.h"
#include "provision.h"
#endif
#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include "setup_page.h"            // after Arduino.h: PROGMEM comes from the core

#if !ORCON_TX_POC || ORCON_WEB_CONTROL
namespace setupweb {
using namespace setupflow;

// Capabilities from the build, never from the page. The SX1262 path cannot decode RAMSES.
#if defined(RADIO_SX1262)
static const bool CAN_RX = false;
#else
static const bool CAN_RX = true;
#endif
#if ORCON_TX_POC
// Web-control build: the txpoc layer serves pair/control. "simulated" tells the page
// the RF stage is a dry run (no bytes on air) — every status stays truthful about it.
static const bool CAN_TX = true;
static const bool SIMULATED = !ORCON_TX_RF_ENABLE;
#else
static const bool CAN_TX = !RX_ONLY;
static const bool SIMULATED = false;
#endif
static const bool CAN_PAIR = CAN_TX && CAN_RX && (!ORCON_TX_POC || SIMULATED || ORCON_TX_PAIR);

#if ORCON_TX_POC
static const uint8_t TXB_PER_MINUTE = txbudget::PER_MINUTE, TXB_PER_HOUR = txbudget::PER_HOUR;
static const size_t MODE_ONAIR = 53, OFFER_ONAIR = 105;   // on-air bytes: a 22F1 command, the 1FC9 offer
#else
static const uint8_t TXB_PER_MINUTE = 0, TXB_PER_HOUR = 0;
#endif
static WebServer* g_srv = nullptr;
static char     g_token[33] = "";
static uint32_t g_sid = 0, g_ev = 0;
static Read     g_read;
static Pair     g_pair;
static Cmd      g_cmd;
#if ORCON_TX_POC
static bool     g_pairDryComplete = false;
static bool     g_pendOffer = false, g_pendAck = false;   // bind sends awaiting their result
static uint32_t g_offerAt = 0;                            // when the offer was admitted
static ramses::DeviceId g_learnedFan;                     // pairing result to persist (net.cpp)
#endif
static char     g_out[3072];                  // one response at a time (loop task only)
static rfcon::Ring g_con;                     // live RF console (loop task only)
static char     g_conOut[3072];

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
[[maybe_unused]] static void failRetry(int code, const char* err, uint32_t waitMs) {   // tells the page when to try again
    Out o; o.add("{\"error\":\"%s\",\"retry_s\":%lu}", err, (unsigned long)((waitMs + 999) / 1000)); reply(code, o);
}

static void idStr(const ramses::DeviceId& d, char (&out)[16]) { snprintf(out, sizeof(out), "%02u:%06lu", d.cls, (unsigned long)d.serial); }
static uint32_t seqNow() { return radio::rxDiagnostics().coreSyncHits; }   // 0 on radios without sync numbers
static const char* rfState() {
    if (!CAN_RX) return "unsupported";
    static const char* const S[] = { "receiving", "recovering", "failed" };
    uint8_t s = radio::rxDiagnostics().rxState;
    return S[s < 3 ? s : 2];
}
static bool pairActive() { return g_pair.st == St::Active; }
#if !ORCON_TX_POC
static bool fwPaired() { return orcon::pairState() == orcon::PairState::Paired; }
#endif

#if ORCON_WEB_AUTH
// ---- web login (web_auth.h): sessions in RAM, the password record in NVS ----------------------
static webauth::Sessions g_sessions;
static webauth::Throttle g_throttle;
static webauth::Recovery g_recovery;
static webauth::Cred     g_cred;
static bool              g_credSet = false;
static bool              g_credUnreadable = false;   // a stored password that cannot be read: never "none" (fail closed)
static webauth::Session* currentSession() {
    char sid[33];
    if (!webauth::cookieSid(g_srv->header("Cookie").c_str(), sid)) return nullptr;
    return g_sessions.find(sid, millis());
}
#endif

// ---- POST guard: JSON, same origin, a logged-in session and its CSRF token (the per-boot token
// on builds without the web login), bounded body, known keys only ----------------------------------
static bool hostOk() { return hostAllowed(g_srv->hostHeader().c_str(), net::ipAddr()); }
static bool sameOrigin(bool allowAp) {
#if ORCON_WEB_CONTROL
    // The setup AP serves only settings and the login; RF control needs the home network. Except
    // during the installation: the device then runs the AP only (no home network at all), and the
    // wizard's profile step (read, pair) has no other way in.
#if ORCON_OWNER_MODEL
    if (provision::installing(ownerstore::state()) && net::apOnly()) allowAp = true;
#endif
    if ((net::apMode() && !allowAp) || g_srv->header("Origin").length() == 0) { fail(403, "forbidden-request"); return false; }
#else
    (void)allowAp;
#endif
    if (!hostOk() || !requestAllowed(g_srv->header("Content-Type").c_str(), g_srv->header("Origin").c_str(),
                        g_srv->hostHeader().c_str())) { fail(403, "forbidden-request"); return false; }
    return true;
}
static bool parseBody(JsonDocument& d, const char* const* keys, size_t nkeys, size_t maxBody) {
    if (g_srv->arg("plain").length() > maxBody) { fail(413, "body-too-large"); return false; }
    if (!uniqueObjectKeys(g_srv->arg("plain").c_str(), keys, nkeys)) { fail(400, "bad-json"); return false; }
    if (deserializeJson(d, g_srv->arg("plain")) || !d.is<JsonObject>()) { fail(400, "bad-json"); return false; }   // same instantiation as net.cpp
    for (JsonPair kv : d.as<JsonObject>()) {
        bool ok = false;
        for (size_t i = 0; i < nkeys; i++) if (!strcmp(kv.key().c_str(), keys[i])) ok = true;
        if (!ok) { fail(400, "unknown-field"); return false; }
    }
    return true;
}
static bool guard(JsonDocument& d, const char* const* keys, size_t nkeys, bool allowAp = false, size_t maxBody = 160) {
    if (!sameOrigin(allowAp)) return false;
#if ORCON_WEB_AUTH
    webauth::Session* s = currentSession();
    if (!s) { fail(401, "login-required"); return false; }
    if (!webauth::equal(g_srv->header("X-Orcon-Token").c_str(), s->csrf)) { fail(403, "bad-token"); return false; }
#else
    if (!tokenEqual(g_srv->header("X-Orcon-Token").c_str(), g_token)) { fail(403, "bad-token"); return false; }
#endif
    return parseBody(d, keys, nkeys, maxBody);
}
bool authorize(bool mutation, bool allowAp) {
#if ORCON_WEB_AUTH
    if (mutation) { if (!sameOrigin(allowAp)) return false; }
    else if (!hostOk()) { fail(403, "forbidden-request"); return false; }
    webauth::Session* s = currentSession();
    if (!s) { fail(401, "login-required"); return false; }
    if (mutation && !webauth::equal(g_srv->header("X-Orcon-Token").c_str(), s->csrf)) { fail(403, "bad-token"); return false; }
    return true;
#else
    (void)mutation; (void)allowAp;
    return true;                                     // legacy builds: unchanged
#endif
}
bool uploadAuthorized() {
#if ORCON_WEB_AUTH
    if (!hostOk()) return false;
    const String origin = g_srv->header("Origin");      // kept alive: c_str() of a temporary would dangle
    if (origin.length() && !requestAllowed("application/json", origin.c_str(), g_srv->hostHeader().c_str())) return false;   // same origin when sent
    webauth::Session* s = currentSession();
    return s && webauth::equal(g_srv->header("X-Orcon-Token").c_str(), s->csrf);
#else
    return true;
#endif
}
static bool sidOk(JsonDocument& d, uint32_t sid) { return d["sid"].is<uint32_t>() && d["sid"].as<uint32_t>() == sid; }

// ---- page -----------------------------------------------------------------------
static void handlePage() {
#if ORCON_OWNER_MODEL
    if (net::apMode() && !hostOk()) {                 // captive portal: a phone's check for another name
        char loc[40]; snprintf(loc, sizeof(loc), "http://%s/", net::ipAddr());
        g_srv->sendHeader("Location", loc); g_srv->sendHeader("Cache-Control", "no-store");
        g_srv->send(302, "text/plain", ""); return;
    }
#endif
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

// Profiles contain observations only. Our transmitter identity is never replaced
// with the physical remote's identity, and selecting a target does not pair it.
static ramses::DeviceId g_configFan(0,0);
static bool g_profileBase=false;
void restoreProfileSelection() {
    if(!g_profileBase){g_configFan=orcon::ids().fan;g_profileBase=true;}
#if !RX_ONLY
    // Legacy TX builds run periodic Orcon traffic. A profile must never retarget
    // that traffic implicitly; profile control is exclusive to the gated V2 path.
    return;
#else
    const profiles::Profile* p=profiles::store().active();
    const ramses::DeviceId target=p?p->unit:g_configFan;
    const auto own=orcon::ids().us;
    const bool valid=target.cls>0&&target.cls<63&&target.serial>0;
    orcon::setIds(own.cls,own.serial,target.cls,valid?target.serial:0);
#endif
}
static void quoted(Out& o,const char* s) {
    o.add("\"");
    for(;*s;++s){const unsigned char c=*s;if(c=='"'||c=='\\')o.add("\\%c",c);else if(c<0x20)o.add("\\u%04x",c);else o.add("%c",c);}
    o.add("\"");
}
static void profileFields(Out& o) {
    const auto& store=profiles::store();const auto& data=store.data();
    o.add("{\"storage_ok\":%s,\"active\":%lu,\"limit\":%u,\"items\":[",store.ready()?"true":"false",(unsigned long)data.active,unsigned(profiles::LIMIT));
    for(uint8_t i=0;i<data.count;++i){const auto& p=data.items[i];char remote[16],unit[16];idStr(p.remote,remote);idStr(p.unit,unit);
        o.add("%s{\"id\":%lu,\"name\":",i?",":"",(unsigned long)p.id);quoted(o,p.name);
        o.add(",\"remote\":\"%s\",\"unit\":\"%s\",\"control_mapping\":%s,\"paired\":false}",remote,unit,p.controlTail()?"true":"false");
    }
    o.add("]}");
}
static void profileBody(int code) {Out o;profileFields(o);reply(code,o);}
static bool profileBusy() {
    if(g_read.st==St::Active||pairActive())return true;
#if !ORCON_TX_POC
    if(g_cmd.confirm==Cmd::Pending)return true;
#endif
#if ORCON_TX_POC
    if(txpoc::controller().status().busy)return true;
#endif
    return false;
}
static bool profileResult(profiles::Error e) {
    if(e==profiles::Error::None)return true;
    const int code=e==profiles::Error::InvalidName?400:e==profiles::Error::Missing?404:
                   e==profiles::Error::Write||e==profiles::Error::Unavailable?503:409;
    fail(code,profiles::errorName(e));return false;
}
static void handleProfiles() {if(!authorize(false,true))return;profileBody(200);}
static void handleProfileSave() {
    static const char* const K[]={"sid","name"};JsonDocument d;
    if(!guard(d,K,2))return;
    if(profileBusy()){fail(409,"session-busy");return;}
    if(!sidOk(d,g_read.sid)||!g_read.sid||g_read.st!=St::Done){fail(409,"profile-incomplete");return;}
    if(!d["name"].is<const char*>()||d["name"].as<JsonString>().size()!=strlen(d["name"].as<const char*>())){fail(400,"bad-profile-name");return;}
    uint32_t id=0;
    // Save and activate atomically: the page only moves on when the reply shows it active.
    if(profileResult(profiles::store().save(g_read,d["name"].as<const char*>(),id,true))) {
        restoreProfileSelection();
        profileBody(201);
    }
}
#if !ORCON_TX_POC || ORCON_TX_CONTROL
// Systeem/WiFi page actions (builds with the settings pages only): the same POST guard
// as every write, an empty body, and the reply goes out before the controller reboots.
static void handleSystem(bool forgetWifi) {
    JsonDocument d;
    if(!guard(d,nullptr,0,true))return;
    if(profileBusy()){fail(409,"session-busy");return;}
    if(forgetWifi)net::forgetWifi();else net::restartSoon();
    g_srv->send(200,"application/json","{\"ok\":true,\"restarting\":true}");
}
#endif
static void handleProfileEdit(const char* action) {
    static const char* const K[]={"id","name"};JsonDocument d;
    if(!guard(d,K,!strcmp(action,"rename")?2:1))return;
    if(profileBusy()){fail(409,"session-busy");return;}
    if(!d["id"].is<uint32_t>()){fail(400,"bad-profile-id");return;}
    const uint32_t id=d["id"].as<uint32_t>();profiles::Error e;
    if(!strcmp(action,"select"))e=profiles::store().select(id);
    else if(!strcmp(action,"delete"))e=profiles::store().remove(id);
    else {
        if(!d["name"].is<const char*>()||d["name"].as<JsonString>().size()!=strlen(d["name"].as<const char*>())){fail(400,"bad-profile-name");return;}
        e=profiles::store().rename(id,d["name"].as<const char*>());
    }
    if(profileResult(e)){restoreProfileSelection();profileBody(200);}
}

// ---- /api/setup/state -------------------------------------------------------------
static void handleState() {
#if ORCON_WEB_AUTH
    // Not logged in: only what the login page needs. No token, profiles, IDs or network details.
    webauth::Session* sess = hostOk() ? currentSession() : nullptr;
    if (!sess) {
        Out o;
        o.add("{\"fw\":\"%s\",\"auth\":{\"enabled\":true,\"logged_in\":false,\"password_set\":%s,\"login_wait_s\":%lu},"
              "\"wifi\":{\"ap\":%s}", FW_VERSION, g_credSet || g_credUnreadable ? "true" : "false",
              (unsigned long)((g_throttle.waitMs(millis()) + 999) / 1000), net::apMode() ? "true" : "false");
#if ORCON_OWNER_MODEL
        // What the page needs to choose between the wizard, the recovery screens and the login.
        o.add(",\"setup\":{\"state\":\"%s\",\"via_ap\":%s,\"physical\":%s}", provision::name(ownerstore::state()),
              net::fromSetupAp() ? "true" : "false", ownerstore::physicalActive() ? "true" : "false");
#endif
        o.add("}");
        reply(200, o);
        return;
    }
    const char* token = sess->csrf;
#else
    const char* token = hostOk() ? g_token : "";
#endif
    const orcon::State& s = orcon::state();
    const uint32_t now = millis();
    Out o;
    o.add("{\"fw\":\"%s\",\"rx_only\":%s,\"caps\":{\"rx_supported\":%s,\"tx_supported\":%s,"
          "\"read_supported\":%s,\"pair_supported\":%s,\"control_supported\":%s,\"simulated\":%s,"
          "\"timer_supported\":%s,\"profile_control_supported\":%s,\"settings_pages\":%s,\"tx_max_per_boot\":%d,"
          "\"tx_per_minute\":%d,\"tx_per_hour\":%d,\"dashboard\":%s,\"mqtt_supported\":%s},"
          "\"rf\":\"%s\",\"token\":\"%s\",\"mask_ids\":%s,\"active\":\"%s\","
          "\"auth\":{\"enabled\":%s,\"logged_in\":true}",
          FW_VERSION, RX_ONLY ? "true" : "false", CAN_RX ? "true" : "false", CAN_TX ? "true" : "false",
          CAN_RX ? "true" : "false", CAN_PAIR ? "true" : "false", CAN_TX ? "true" : "false",
          SIMULATED ? "true" : "false", ORCON_TX_POC ? "false" : "true", ORCON_WEB_CONTROL ? "true" : "false",
          !ORCON_TX_POC || ORCON_TX_CONTROL ? "true" : "false",    // WiFi/Systeem/OLED routes exist (net.cpp)
          ORCON_TX_MAX_PER_BOOT,
          ORCON_TX_RF_ENABLE ? int(TXB_PER_MINUTE) : 0, ORCON_TX_RF_ENABLE ? int(TXB_PER_HOUR) : 0,
          ORCON_TX_POC ? "false" : "true", MQTT_ENABLED ? "true" : "false",   // /dashboard; MQTT settings
          rfState(), token, net::maskIds() ? "true" : "false",
          g_read.st == St::Active ? "read" : pairActive() ? "pair" : "none", ORCON_WEB_AUTH ? "true" : "false");
    char id[16] = "";
    if (s.fanKnown) idStr(orcon::ids().fan, id);
    o.add(",\"unit\":{\"known\":%s,\"id\":\"%s\",\"mode\":\"%s\",\"age_s\":%ld}",
          s.fanKnown ? "true" : "false", id, orcon::modeName(s.mode),
          s.modeMs ? long((now - s.modeMs) / 1000) : -1L);
    o.add(",\"remote_request\":{\"mode\":\"%s\",\"age_s\":%ld}", orcon::modeName(s.remoteMode),
          s.remoteMs ? long((now - s.remoteMs) / 1000) : -1L);
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
    o.add("],\"wifi\":{\"connected\":%s,\"ap\":%s,\"ap_name\":\"%s\",\"ip\":",net::wifiConnected()?"true":"false",net::apMode()?"true":"false",WIFI_AP_NAME);
    quoted(o,net::ipAddr());o.add(",\"ssid\":");quoted(o,net::wifiConnected()?net::ssidName():"");
    o.add("},\"profiles\":");profileFields(o);
#if ORCON_OWNER_MODEL
    { uint8_t q[ownerkey::PUB_LEN];
      o.add(",\"setup\":{\"state\":\"%s\",\"via_ap\":%s,\"physical\":%s,\"key\":%s}", provision::name(ownerstore::state()),
            net::fromSetupAp() ? "true" : "false", ownerstore::physicalActive() ? "true" : "false", ownerstore::ownerKey(q) ? "true" : "false"); }
#endif
    o.add("}");
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
static void handleReadState() { if (!authorize(false, true)) return; readBody(200); }

static void handleReadStart() {
    static const char* const K[] = { "remote" };
    JsonDocument d;
    if (!guard(d, K, 1)) return;
    if (!CAN_RX) { fail(501, "read-unsupported"); return; }
    if (g_read.st == St::Active || pairActive()) { fail(409, "session-busy"); return; }
#if ORCON_TX_POC
    if (txpoc::controller().status().busy) { fail(409, "session-busy"); return; }
#endif
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
// Web-control path: "paired" = the unit's W 1FC9 confirmation AND our closing ack sent.
static bool pairDone() {
#if ORCON_TX_POC
    return g_pair.webctlPaired();
#else
    return fwPaired();
#endif
}
static const ramses::DeviceId& pairFan() {
#if ORCON_TX_POC
    return g_pair.from;
#else
    return orcon::ids().fan;
#endif
}
static void pairBody(int code) {
    Out o;
    o.add("{\"supported\":%s,\"sid\":%lu,\"status\":\"%s\",\"bind_sent\":%s,\"rf_confirmed\":%s,"
          "\"ack_sent\":%s,\"paired\":%s,\"left_s\":%lu,\"simulated\":%s}",
          CAN_PAIR ? "true" : "false", (unsigned long)g_pair.sid,
#if ORCON_TX_POC
          g_pairDryComplete ? "dry_run_complete" :
#endif
          g_pair.status(pairDone(), pairFan()),
          g_pair.sent ? "true" : "false", g_pair.confirmed ? "true" : "false",
          g_pair.ackSent ? "true" : "false",
          g_pair.paired(pairDone(), pairFan()) ? "true" : "false",
          (unsigned long)(g_pair.leftMs(millis()) / 1000), SIMULATED ? "true" : "false");
    reply(code, o);
}
static void handlePairState() { if (!authorize(false, true)) return; pairBody(200); }

static void handlePairStart() {
    JsonDocument d;
    if (!guard(d, nullptr, 0)) return;
    if (!CAN_PAIR) { fail(501, ORCON_TX_POC ? "pairing-unverified" : "tx-unavailable"); return; }
    if (g_read.st == St::Active || pairActive()) { fail(409, "session-busy"); return; }
#if ORCON_TX_POC
    if (txpoc::controller().status().busy) { fail(409, "session-busy"); return; }  // open RF transaction
#endif
    #if ORCON_TX_POC
    g_pairDryComplete = false;
    g_pendOffer = g_pendAck = false;
#endif
    g_pair.start(++g_sid, millis());
    pairBody(201);
}

static void handlePairPress() {
    static const char* const K[] = { "sid", "keys" };
    JsonDocument d;
    if (!guard(d, K, 2)) return;
    if (!CAN_PAIR) { fail(501, ORCON_TX_POC ? "pairing-unverified" : "tx-unavailable"); return; }
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
#if ORCON_TX_POC
    // Asynchronous: the owner transmits the offer; sent/tx_failed arrive with the
    // transaction result (setupweb::tick). Never a retry: one press, one offer.
    if (g_pair.sent || g_pair.txFailed || g_pendOffer) { fail(409, "already-sent"); return; }
    if (const uint32_t w = txpoc::budgetWaitMs(2, OFFER_ONAIR)) { failRetry(429, "tx-budget", w); return; }
#if ORCON_OWNER_MODEL
    if (!net::ensureOwnIdentity()) { fail(503, "identity-unavailable"); return; }   // random, at the first pairing
#endif
    const rlog::Device* heard = nullptr; const size_t heardN = rlog::devices(&heard);
    const bool identityClear = !txpoc::identityConflict(orcon::ids(), rlog::seen(), heard, heardN);
    const txpoc::Admission adm = txpoc::controller().submitBind((uint8_t)txpoc::FrameKind::BindOffer,
                                     orcon::ids(), txpoc::rxReady(), identityClear, millis());
    if (adm == txpoc::Admission::Busy || adm == txpoc::Admission::Limited) { fail(409, "session-busy"); return; }
    if (adm != txpoc::Admission::Accepted) { fail(502, "tx-failed"); return; }  // refused before radio work
    g_pendOffer = true;
    g_offerAt = millis();
    pairBody(200);
#else
    if (g_pair.sent || g_pair.txFailed) { fail(409, "already-sent"); return; }   // no retry
    const uint32_t before = orcon::state().txCount;
    orcon::startPairing();                          // existing 1FC9 bind offer + 3-min window
    g_pair.bindResult(orcon::state().txCount > before, millis());
    if (!g_pair.sent) { fail(502, "tx-failed"); return; }
    pairBody(200);
#endif
}

static void handlePairCancel() {
    static const char* const K[] = { "sid" };
    JsonDocument d;
    if (!guard(d, K, 1)) return;
    if (!pairActive() || !sidOk(d, g_pair.sid)) { fail(409, "no-such-session"); return; }
    g_pair.cancel();                                // a sent offer cannot be recalled; the window just runs out
#if ORCON_TX_POC
    g_pendOffer = g_pendAck = false;
#endif
    pairBody(200);
}

// ---- virtual remote (TX builds only) ------------------------------------------------------
// CAN_TX == false exactly on receive-only, non-PoC builds (see the constant above).
#if (!ORCON_TX_POC && RX_ONLY)
static void handleRemoteOff() { fail(501, "tx-unavailable"); }
#else
#if ORCON_TX_POC
// Web-control path: one txpoc transaction per button press; the unit's own 31D9 within
// the window is the only confirmation. In dry-run builds nothing reaches the air and
// the "confirm" chip says so (not_available), never a simulated confirmation.
static const char* keyOfMode(uint8_t m) {
    switch (m) { case 0: return "away"; case 1: return "1"; case 2: return "2"; case 3: return "3"; case 4: return "auto"; }
    return "";
}
static const char* ctlConfirm(const txpoc::Status& s) {
    switch (s.phase) {
        case txpoc::Phase::Queued:
        case txpoc::Phase::AwaitUnit:      return "pending";
        // confirmed: on air, the requested stand, reported within PROMPT_MS of our TX start
        // (the unit's reaction to our frame). A later matching report is only observed.
        case txpoc::Phase::UnitReported:   return s.result.rfSent && s.prompt ? "confirmed" : "observed";
        case txpoc::Phase::Unchanged:      return "unchanged";
        case txpoc::Phase::Ambiguous:      return "ambiguous";
        case txpoc::Phase::NotConfirmed:   return "not_confirmed";
        case txpoc::Phase::DryRunComplete: return "not_available";  // nothing on air: no causal proof
        default:                           return "none";
    }
}
static void remoteBody(int code) {
    const txpoc::Status& s = txpoc::controller().status();
    const txpoc::Result& r = s.result;
    const orcon::State& os = orcon::state();
    const uint32_t now = millis();
    const bool isCmd = r.request.kind == (uint8_t)txpoc::FrameKind::Mode && r.request.input.request && r.request.ids.fan==orcon::ids().fan;
#if ORCON_TX_RF_ENABLE
    const uint32_t restoreDiag = radio::sx1276::restoreDiag();   // facts for diagnosing a failed RX restore
#else
    const uint32_t restoreDiag = 0;
#endif
    Out o;
    o.add("{\"supported\":true,\"id\":%lu,\"key\":\"%s\",\"minutes\":0,"
          "\"accepted\":%s,\"sent\":%s,\"confirm\":\"%s\",\"age_s\":%lu,"
          "\"unit_mode\":\"%s\",\"unit_age_s\":%ld,"
          "\"phase\":\"%s\",\"error\":\"%s\",\"rf_sent\":%s,\"simulated\":%s,\"unit_confirmed\":%s,\"next_request\":%lu,"
          "\"restore_diag\":\"%08lX\",\"tx_starts\":%lu,",
          (unsigned long)(isCmd ? r.request.input.request : 0),
          isCmd ? keyOfMode(r.request.input.mode) : "",
          isCmd ? "true" : "false", isCmd && r.rfSent ? "true" : "false",
          isCmd ? ctlConfirm(s) : "none",
          (unsigned long)(isCmd && s.acceptedMs ? (now - s.acceptedMs) / 1000 : 0),
          orcon::modeName(os.mode),
          os.modeMs ? long((now - os.modeMs) / 1000) : -1L,
          isCmd ? txpoc::phaseName(s.phase) : "idle", isCmd ? txpoc::errorName(r.error) : "none",
          isCmd && r.rfSent ? "true" : "false", SIMULATED ? "true" : "false",
          isCmd && !strcmp(ctlConfirm(s), "confirmed") ? "true" : "false", (unsigned long)s.nextRequest,
          (unsigned long)restoreDiag, (unsigned long)txpoc::rfTxStarts());
    const txpoc::BudgetView bv = txpoc::budget();
    o.add("\"budget\":{\"minute\":%u,\"minute_max\":%u,\"hour\":%u,\"hour_max\":%u,\"air_ms_hour\":%lu,\"retry_s\":%lu}}",
          bv.minute, unsigned(ORCON_TX_RF_ENABLE ? TXB_PER_MINUTE : 0), bv.hour, unsigned(ORCON_TX_RF_ENABLE ? TXB_PER_HOUR : 0),
          (unsigned long)(bv.airUsHour / 1000), (unsigned long)((txpoc::budgetWaitMs(1, MODE_ONAIR) + 999) / 1000));
    reply(code, o);
}
static void handleRemoteState() { if (!authorize(false, true)) return; remoteBody(200); }
static void ctlRefuse(txpoc::Admission a) {
    const char* e = "invalid"; int code = 400;
    switch (a) {
        case txpoc::Admission::Replay:           e = "stale-request";         code = 409; break;
        case txpoc::Admission::Conflict:         e = "request-conflict";      code = 409; break;
        case txpoc::Admission::Invalid:          e = "bad-key";               code = 400; break;
        case txpoc::Admission::UnknownUnit:      e = "unit-not-selected";     code = 409; break;
        case txpoc::Admission::Busy:             e = "session-busy";          code = 409; break;
        case txpoc::Admission::Unavailable:      e = "rf-unavailable";        code = 503; break;
        case txpoc::Admission::Limited:          e = "too-fast";              code = 429; break;
        case txpoc::Admission::IdentityUnverified: e = "identity-unverified"; code = 403; break;
        case txpoc::Admission::IdentityConflict: e = "identity-conflict";     code = 403; break;
        case txpoc::Admission::UnitStateUnknown: e = "unit-state-unknown";    code = 409; break;
        case txpoc::Admission::Unchanged:        e = "unit-already-in-mode";  code = 409; break;
        case txpoc::Admission::RemoteActive:     e = "remote-active";         code = 409; break;
        default:                                                              break;
    }
    fail(code, e);
}
static void handleRemoteSend() {
    static const char* const K[] = { "key", "minutes", "request" };
    JsonDocument d;
    if (!guard(d, K, 3)) return;
    if (!d["request"].is<uint32_t>() || !d["request"].as<uint32_t>()) { fail(400, "bad-request"); return; }
    const uint8_t k = d["key"].is<const char*>() ? keyFromName(d["key"].as<const char*>()) : uint8_t(K_NONE);
    if (k == K_NONE || k == K_TIMER) { fail(400, "bad-key"); return; }   // five functions; no timer here
    if (!d["minutes"].isNull()) { fail(400, "bad-minutes"); return; }
    if (!orcon::state().fanKnown) { fail(409, "unit-not-selected"); return; }
    if (g_read.st == St::Active || pairActive()) { fail(409, "session-busy"); return; }
    const rlog::Device* heard = nullptr; const size_t heardN = rlog::devices(&heard);
    const bool identityClear = !txpoc::identityConflict(orcon::ids(), rlog::seen(), heard, heardN);
    if(!profiles::store().ready()){fail(503,"profile-storage-unavailable");return;}
    // TX only from an active profile: its six captured frames are the evidence for the
    // five mode bytes and the unit-family tail. Without one nothing is guessed.
    const profiles::Profile* profile=profiles::store().active();
    if(!profile){fail(409,"profile-required");return;}
    const uint8_t tail=profile->controlTail();
    if(!tail){fail(409,"profile-protocol-unsupported");return;}
    if(!(profile->unit==orcon::ids().fan)){fail(409,"profile-unit-mismatch");return;}
    if(profile->remote==orcon::ids().us){fail(403,"identity-conflict");return;}
#if ORCON_TX_MAX_PER_BOOT
    // RF test image: refuse cleanly once the RF backend has used its per-boot packets.
    if(txpoc::rfTxStarts()>=ORCON_TX_MAX_PER_BOOT){fail(409,"tx-limit-reached");return;}
#endif
    if(const uint32_t w=txpoc::budgetWaitMs(1,MODE_ONAIR)){failRetry(429,"tx-budget",w);return;}
    txpoc::Admission a = txpoc::controller().submitCtl(modeOfKey(k), tail, orcon::ids(),
                             true, txpoc::rxReady(), identityClear, millis(), d["request"].as<uint32_t>());
    if (a == txpoc::Admission::Duplicate) { remoteBody(200); return; }
    if (a != txpoc::Admission::Accepted) { ctlRefuse(a); return; }
    remoteBody(202);
}
#else
static void remoteBody(int code) {
    const orcon::State& s = orcon::state();
    const uint32_t now = millis();
    Out o;
    o.add("{\"supported\":true,\"id\":%lu,\"key\":\"%s\",\"minutes\":%u,\"accepted\":%s,\"sent\":%s,"
          "\"confirm\":\"%s\",\"age_s\":%lu,\"unit_mode\":\"%s\",\"unit_age_s\":%ld,\"unit_confirmed\":false}",
          (unsigned long)g_cmd.id, g_cmd.key < K_N ? KEY_NAME[g_cmd.key] : "", g_cmd.minutes,
          g_cmd.id ? "true" : "false", g_cmd.sent ? "true" : "false", g_cmd.confirm==Cmd::Confirmed ? "observed" : confirmName(g_cmd.confirm),
          (unsigned long)(g_cmd.id ? g_cmd.ageMs(now) / 1000 : 0), orcon::modeName(s.mode),
          s.modeMs ? long((now - s.modeMs) / 1000) : -1L);
    reply(code, o);
}
static void handleRemoteState() { if (!authorize(false, true)) return; remoteBody(200); }

static void handleRemoteSend() {
    static const char* const K[] = { "key", "minutes" };
    JsonDocument d;
    if (!guard(d, K, 2)) return;
    const uint8_t k = d["key"].is<const char*>() ? keyFromName(d["key"].as<const char*>()) : uint8_t(K_NONE);
    if (k == K_NONE) { fail(400, "bad-key"); return; }
    if(!profiles::store().ready()){fail(503,"profile-storage-unavailable");return;}
    if(profiles::store().active()){fail(501,"profile-backend-unavailable");return;}
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
#endif // ORCON_TX_POC
#endif // CAN_TX

// ---- live RF console ------------------------------------------------------------------
// Facts only: syncs from the RX task's note queue, decoded frames from the existing drain,
// the wizard's own verdict, session changes and RX health deltas. Masked IDs only.
static const char* const KEY_NL[K_N] = { "Afwezig", "Auto", "Timer", "1", "2", "3" };
static const uint32_t GARBLED_MS = 1500;      // a sync without a clean frame by then: undecoded
struct Open { uint32_t seq = 0, ms = 0; int16_t rssi = 0; };
static Open     g_open[8];                    // syncs still waiting for their frame
static uint32_t g_lastHealthMs = 0, g_lastSumMs = 0, g_sumSyncs = 0, g_sumFrames = 0, g_sumBad = 0;
static radio::RxDiagnostics g_h;               // last health snapshot
static uint32_t g_hSyncDrops = 0;
static bool     g_hInit = false;
static St       g_cSt = St::Idle;
static uint32_t g_cSid = 0;

static void drainSyncs() {
    radio::SyncInfo si;
    while (radio::pollSync(si)) {
        g_con.add(rfcon::RAW, si.ms, "sync #%lu %d dBm pre=%02X", (unsigned long)si.seq, si.rssi, si.pre);
        Open* slot = &g_open[0];
        for (Open& o : g_open) { if (!o.seq) { slot = &o; break; } if (o.ms < slot->ms) slot = &o; }
        if (slot->seq) {                       // 8 open at once: report the oldest as unresolved
            g_con.add(rfcon::RAW, si.ms, "sync #%lu: niet meer gevolgd (te veel tegelijk)", (unsigned long)slot->seq);
        }
        slot->seq = si.seq; slot->ms = si.ms; slot->rssi = si.rssi;
    }
}
static void resolveSync(uint32_t seq) { for (Open& o : g_open) if (seq && o.seq == seq) o.seq = 0; }
static void expireSyncs(uint32_t now) {
    for (Open& o : g_open) if (o.seq && now - o.ms >= GARBLED_MS) {
        g_con.add(rfcon::RAW, now, "sync #%lu %d dBm: geen geldig frame (verminkt of onvolledig)", (unsigned long)o.seq, o.rssi);
        g_sumBad++; o.seq = 0;
    }
}
static bool listedRemote(const ramses::DeviceId& id) {
    const rlog::Device* dev = nullptr;
    const size_t n = rlog::devices(&dev);
    for (size_t i = 0; i < n; i++) if (dev[i].id == id && (dev[i].roles & rlog::ROLE_REMOTE)) return true;
    return false;
}
static void consoleFrame(const ramses::Frame& f, uint32_t seq, int16_t rssi, Why w, uint32_t othersBefore) {
    char t[96];
    rfcon::frameText(f, seq, rssi, t, sizeof(t));
    const uint32_t now = millis();
    g_con.add(rfcon::RX, now, "%s", t);
    if ((f.cmd != 0x22F1 && f.cmd != 0x22F3) || !f.has_addr[0]) return;   // only remote commands matter here
    char id[12]; rfcon::maskId(f.addr[0], id);
    const uint8_t k = keyOfFrame(f);
    const char* key = k < K_N ? KEY_NL[k] : "geen knop";
    const uint8_t want = g_read.step < K_N ? g_read.step : 0;
    switch (w) {
    case Why::Accepted:  g_con.add(rfcon::OK, now, "%s knop %s geaccepteerd: stap %u/6", id, key, g_read.step); return;
    case Why::Duplicate: g_con.add(rfcon::FILTER, now, "%s telt niet: herhaling van de vorige druk", id); return;
    case Why::WrongKey:  g_con.add(rfcon::FILTER, now, "%s telt niet: knop %s, verwacht %s", id, key, KEY_NL[want]); return;
    case Why::Unknown:   g_con.add(rfcon::FILTER, now, "%s telt niet: %04X is geen knop van deze wizard", id, f.cmd); return;
    case Why::Stale:     g_con.add(rfcon::FILTER, now, "%s telt niet: van vóór de huidige stap", id); return;
    default: break;
    }
    if (g_read.others > othersBefore) {
        g_con.add(rfcon::FILTER, now, "%s telt niet: niet de gekozen afstandsbediening", id); return;
    }
    if (g_read.st == St::Active && f.type != ramses::MsgType::I) {
        g_con.add(rfcon::FILTER, now, "%s telt niet: geen knopdruk (type %s)", id, f.type == ramses::MsgType::RP ? "RP" : "RQ/W"); return;
    }
    const bool listed = listedRemote(f.addr[0]);
    g_con.add(listed ? rfcon::INFO : rfcon::FILTER, now, "%s knop %s: %s", id, key,
              listed ? "in de lijst gehoorde afstandsbedieningen (geen uitleessessie actief)"
                     : "niet in de lijst (een antwoord RP telt niet als afstandsbediening)");
}
static void consoleTick(uint32_t now) {
    drainSyncs();
    expireSyncs(now);
    if (g_read.sid != g_cSid || g_read.st != g_cSt) {          // session changes
        char id[12]; rfcon::maskId(g_read.remote, id);
        if (g_read.st == St::Active) g_con.add(rfcon::INFO, now, "uitlezen gestart (sessie %lu, %s): druk %s", (unsigned long)g_read.sid, id, KEY_NL[g_read.step < K_N ? g_read.step : 0]);
        else if (g_read.st == St::Done) g_con.add(rfcon::INFO, now, "uitlezen voltooid: alle 6 knoppen van %s", id);
        else if (g_read.st == St::Timeout) g_con.add(rfcon::WARN, now, "time-out: geen geldige knop binnen 60 s (stap %u)", g_read.step + 1);
        else if (g_read.st == St::Cancelled) g_con.add(rfcon::INFO, now, "uitlezen geannuleerd");
        g_cSid = g_read.sid; g_cSt = g_read.st;
    }
    if (g_hInit && now - g_lastHealthMs < 1000) return;
    g_lastHealthMs = now;
    const radio::RxDiagnostics d = radio::rxDiagnostics();
    const uint32_t sd = radio::syncInfoDrops();
    if (!g_hInit) {
        g_h = d; g_hSyncDrops = sd; g_hInit = true; g_lastSumMs = now;
        g_sumSyncs = d.coreSyncHits; g_sumFrames = d.frames; g_sumBad = 0;
        static const char* const S[] = { "ontvangend", "herstellend", "uitgevallen" };
        g_con.add(d.rxState ? rfcon::ERR : rfcon::INFO, now, "console gestart: radio %s, %lu syncs, %lu frames sinds boot",
                  S[d.rxState < 3 ? d.rxState : 2], (unsigned long)d.coreSyncHits, (unsigned long)d.frames);
        return;
    }
    if (d.uartOverflows != g_h.uartOverflows) g_con.add(rfcon::WARN, now, "UART-overflow +%lu: ontvangen bytes verloren", (unsigned long)(d.uartOverflows - g_h.uartOverflows));
    if (d.queueDrops != g_h.queueDrops) g_con.add(rfcon::WARN, now, "frame-queue vol: +%lu frames gedropt", (unsigned long)(d.queueDrops - g_h.queueDrops));
    if (d.rxLost != g_h.rxLost) g_con.add(rfcon::WARN, now, "radio buiten RX aangetroffen (+%lu)", (unsigned long)(d.rxLost - g_h.rxLost));
    if (d.rxRecoveries != g_h.rxRecoveries) g_con.add(rfcon::WARN, now, "radio-herstart, poging %u", d.rxRecoveries);
    if (sd != g_hSyncDrops) g_con.add(rfcon::WARN, now, "console miste %lu syncs (lus te traag)", (unsigned long)(sd - g_hSyncDrops));
    if (d.rxState != g_h.rxState) {
        if (d.rxState == 0) g_con.add(rfcon::INFO, now, "radio weer ontvangend");
        else g_con.add(rfcon::ERR, now, "%s", d.rxState == 1 ? "radio herstelt: er wordt nu niets ontvangen" : "radio uitgevallen (herstel opgebruikt)");
    }
    g_h = d; g_hSyncDrops = sd;
    if (now - g_lastSumMs >= 30000) {
        g_con.add(rfcon::INFO, now, "laatste %lu s: %lu syncs, %lu frames, %lu zonder geldig frame; ruis %d dBm",
                  (unsigned long)((now - g_lastSumMs) / 1000), (unsigned long)(d.coreSyncHits - g_sumSyncs),
                  (unsigned long)(d.frames - g_sumFrames), (unsigned long)g_sumBad, d.rssiNow);
        g_lastSumMs = now; g_sumSyncs = d.coreSyncHits; g_sumFrames = d.frames; g_sumBad = 0;
    }
}
// ---- Pairing capture: complete 1FC9/10E0 frames ------------------------------------
// The console shows six payload bytes; protocol work on the bind offer needs every byte.
// Filled from onFrame (loop task, after the RX queue), so RX timing is never touched.
// RAM only: nothing is written to flash; the IDs are unmasked like /api/log.
static const uint8_t CAP_N = 8;
struct Captured { uint32_t ms = 0, seq = 0; int16_t rssi = 0; ramses::Frame f; };
static Captured g_cap[CAP_N];
static uint8_t  g_capHead = 0, g_capCount = 0;
static uint32_t g_capTotal = 0;
static void captureFrame(const ramses::Frame& f, uint32_t seq, int16_t rssi) {
    if (f.cmd != 0x1FC9 && f.cmd != 0x10E0) return;
    Captured& c = g_cap[g_capHead];
    c.ms = millis(); c.seq = seq; c.rssi = rssi; c.f = f;
    g_capHead = uint8_t((g_capHead + 1) % CAP_N);
    if (g_capCount < CAP_N) ++g_capCount;
    ++g_capTotal;
}
static void handleCapture() {
    if (!hostOk()) { fail(403, "forbidden-request"); return; }
    static const char* const T[] = { "I", "RQ", "RP", "W" };
    Out o;
    o.add("{\"now\":%lu,\"total\":%lu,\"frames\":[", (unsigned long)millis(), (unsigned long)g_capTotal);
    for (uint8_t k = 0; k < g_capCount; ++k) {
        const Captured& c = g_cap[(g_capHead + CAP_N - g_capCount + k) % CAP_N];
        const ramses::Frame& f = c.f;
        o.add("%s{\"seq\":%lu,\"ms\":%lu,\"rssi\":%d,\"type\":\"%s\",\"addr\":[", k ? "," : "",
              (unsigned long)c.seq, (unsigned long)c.ms, c.rssi, unsigned(f.type) < 4 ? T[unsigned(f.type)] : "?");
        for (int i = 0; i < 3; ++i) {
            if (f.has_addr[i]) o.add("%s\"%02u:%06lu\"", i ? "," : "", f.addr[i].cls, (unsigned long)f.addr[i].serial);
            else               o.add("%snull", i ? "," : "");
        }
        o.add("],\"cmd\":\"%04X\",\"len\":%u,\"pl\":\"", f.cmd, f.len);
        for (uint8_t i = 0; i < f.len && i < ramses::MAX_PAYLOAD; ++i) o.add("%02X", f.payload[i]);
        o.add("\"}");
    }
    o.add("]}");
    reply(200, o);
}

static void handleConsole() {
    if (!hostOk()) { fail(403, "forbidden-request"); return; }
    const String a = g_srv->arg("after");
    char* end = nullptr;
    unsigned long after = strtoul(a.c_str(), &end, 10);
    if (!a.length() || (end && *end)) after = 0;                 // absent or not a number: from the start
    consoleTick(millis());                                       // fresh syncs/health in this reply
    for (uint32_t max = 16; max; max /= 2)
        if (g_con.json(uint32_t(after), max, millis(), g_conOut, sizeof(g_conOut))) {
            g_srv->sendHeader("Cache-Control", "no-store");
            g_srv->send(200, "application/json", g_conOut);
            return;
        }
    fail(500, "response-too-large");
}

// ---- wiring ------------------------------------------------------------------------
#if ORCON_WEB_AUTH
// ---- web login routes --------------------------------------------------------------------------
// Passwords and codes are never logged; console lines only say what happened.
static void setCookie(const char* sid, uint32_t maxAgeS) {
    char c[128];
    snprintf(c, sizeof(c), "orcon_sid=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%lu", sid, (unsigned long)maxAgeS);
    g_srv->sendHeader("Set-Cookie", c);
}
static void sessionReply(const webauth::Session& s) {
    setCookie(s.id, webauth::LIFETIME_MS / 1000);
    Out o; o.add("{\"ok\":true,\"token\":\"%s\"}", s.csrf); reply(200, o);
}
static bool kdfBusy() {   // PBKDF2 holds the loop for a few hundred ms: never during an RF transaction
#if ORCON_TX_POC
    if (txpoc::controller().status().busy) { fail(409, "session-busy"); return true; }
#endif
    return false;
}
static bool setPassword(const char* pw) {
    webauth::Cred c{};
    if (!webauth::makeCred(pw, authstore::kdf, authstore::random, c)) { fail(503, "auth-unavailable"); return false; }
    if (!authstore::save(c)) {
        // The write may have persisted although its read-back failed (Codex OWNER-06/08). A replacement may
        // be durable, so every session ends; RAM then mirrors storage and nothing cached outlives that:
        // unreadable = no login until it reads again; only a confirmed absence means "no password".
        g_sessions.clear();
        webauth::Cred back{};
        const authstore::Status st = authstore::status(back);
        g_credSet = st == authstore::Status::Ok;
        g_credUnreadable = st == authstore::Status::Unreadable;
        g_cred = g_credSet ? back : webauth::Cred{};
        fail(503, "auth-write-failed");
        return false;
    }
    g_cred = c; g_credSet = true; g_credUnreadable = false;
    return true;
}
static const char* str(JsonDocument& d, const char* k) { return d[k].is<const char*>() ? d[k].as<const char*>() : ""; }
static void badPassword() {
    g_throttle.failure(millis());
    g_con.add(rfcon::WARN, millis(), "aanmelden: onjuist wachtwoord (%u keer op rij)", g_throttle.failures());
    failRetry(401, "bad-password", g_throttle.waitMs(millis()));
}
static void handleLogin() {
    static const char* const K[] = { "password" };
    JsonDocument d;
    if (!sameOrigin(true) || !parseBody(d, K, 1, 400)) return;
    if (!g_credSet) { fail(g_credUnreadable ? 503 : 409, g_credUnreadable ? "auth-unavailable" : "password-not-set"); return; }
    if (const uint32_t w = g_throttle.waitMs(millis())) { failRetry(429, "login-locked", w); return; }
    if (!authstore::kdfReady()) { fail(503, "auth-unavailable"); return; }
    if (kdfBusy()) return;
    const uint32_t t0 = millis();
    if (!webauth::verify(g_cred, str(d, "password"), authstore::kdf)) { badPassword(); return; }
    g_throttle.success();
    const webauth::Session& s = g_sessions.create(millis(), authstore::random);
    g_con.add(rfcon::INFO, millis(), "aangemeld (wachtwoordcontrole %lu ms)", (unsigned long)(millis() - t0));
    sessionReply(s);
}
static void handleLogout() {
    JsonDocument d;
    if (!guard(d, nullptr, 0, true)) return;
    char sid[33];
    if (webauth::cookieSid(g_srv->header("Cookie").c_str(), sid)) g_sessions.remove(sid);
    setCookie("", 0);
    Out o; o.add("{\"ok\":true}"); reply(200, o);
}
static void handlePassword() {   // change, while logged in: the current password is required
    static const char* const K[] = { "current", "password" };
    JsonDocument d;
    if (!guard(d, K, 2, true, 400)) return;
    if (!webauth::passwordOk(str(d, "password"))) { fail(400, "password-weak"); return; }
    if (const uint32_t w = g_throttle.waitMs(millis())) { failRetry(429, "login-locked", w); return; }
    if (kdfBusy()) return;
    if (!webauth::verify(g_cred, str(d, "current"), authstore::kdf)) { badPassword(); return; }
    g_throttle.success();
    if (!setPassword(str(d, "password"))) return;
    g_sessions.clear();                                // every other device must log in again
    g_con.add(rfcon::INFO, millis(), "wachtwoord gewijzigd; andere sessies afgemeld");
    sessionReply(g_sessions.create(millis(), authstore::random));
}
// The release key may reset a password only on a migrated device that has no owner key yet.
#if ORCON_OWNER_MODEL
static bool vendorRecoveryOk() {
    if (ownerstore::state() == provision::State::Legacy) return true;
    fail(403, "owner-key-required");                 // per-device owner keys: the release key grants no access
    return false;
}
#else
static bool vendorRecoveryOk() { return true; }
#endif
static void handleSetup() {      // first password, or a forgotten one: needs the one-time code
    static const char* const K[] = { "code", "password" };
    JsonDocument d;
    if (!sameOrigin(true) || !vendorRecoveryOk() || !parseBody(d, K, 2, 400)) return;   // Legacy only (Codex OWNER-03)
    if (!webauth::passwordOk(str(d, "password"))) { fail(400, "password-weak"); return; }   // before a try is spent
    if (!authstore::kdfReady()) { fail(503, "auth-unavailable"); return; }
    if (kdfBusy()) return;
    const webauth::Recovery::Check c = g_recovery.use(str(d, "code"), millis());
    if (c != webauth::Recovery::Check::Ok) {
        g_con.add(rfcon::WARN, millis(), "wachtwoord instellen: herstelcode geweigerd (%s)", webauth::checkName(c));
        fail(c == webauth::Recovery::Check::Wrong ? 401 : 409, webauth::checkName(c));
        return;
    }
    if (!setPassword(str(d, "password"))) return;
    g_sessions.clear();
    g_throttle.success();
    g_con.add(rfcon::WARN, millis(), "wachtwoord ingesteld met herstelcode; alle sessies afgemeld");
    sessionReply(g_sessions.create(millis(), authstore::random));
}
// CLI routes (tools/orcon.py auth-code): no browser, no Origin; JSON on our own host only.
static bool cliOk() {
    if (!hostOk() || !requestAllowed(g_srv->header("Content-Type").c_str(), g_srv->header("Origin").c_str(),
                        g_srv->hostHeader().c_str())) { fail(403, "forbidden-request"); return false; }
    return true;
}
static void handleAuthNonce() {
    if (!cliOk() || !vendorRecoveryOk()) return;
    const char* n = g_recovery.issueNonce(millis(), authstore::random);
    char mac[18]; authstore::ownMac(mac);           // the CLI signs for exactly this board
    Out o; o.add("{\"nonce\":\"%s\",\"mac\":\"%s\",\"ttl_s\":%lu}", n, mac, (unsigned long)(webauth::NONCE_TTL_MS / 1000)); reply(200, o);
}
static void handleAuthReset() {  // a request signed with the OTA key buys one 10-minute code
    static const char* const K[] = { "manifest", "sig" };
    JsonDocument d;
    if (!cliOk() || !vendorRecoveryOk() || !parseBody(d, K, 2, 600)) return;
    char mac[18]; authstore::ownMac(mac);
    const char* text = str(d, "manifest");
    if (const char* e = webauth::checkResetRequest(text, mac, g_recovery, millis())) { fail(403, e); return; }
    if (!authstore::verifySignature(text, strlen(text), str(d, "sig"))) {
        g_con.add(rfcon::WARN, millis(), "herstelverzoek geweigerd: ongeldige handtekening");
        fail(403, "bad-signature");
        return;
    }
    const char* code = g_recovery.issueCode(millis(), authstore::random);
    g_con.add(rfcon::WARN, millis(), "herstelcode uitgegeven (eenmalig, 10 minuten geldig)");
    Out o; o.add("{\"code\":\"%s\",\"ttl_s\":%lu}", code, (unsigned long)(webauth::CODE_TTL_MS / 1000)); reply(200, o);
}
#endif

#if ORCON_OWNER_MODEL
// ---- installation wizard and recovery (/api/provision/*). Rules: provision.h; key file: owner_key.h.
// The generated key waits in RAM (public key + its encrypted file) until the owner proves the saved
// file opens with the passphrase; only then is the public key registered. The private key itself is
// wiped right after encryption and never stored.
static uint8_t g_pendQ[ownerkey::PUB_LEN];
static char    g_pendPem[ownerkey::PEM_MAX];
static char    g_pendFp[15] = "";
static bool    g_pend = false;
static void dropPending() { ownerkey::wipe(g_pendPem, sizeof(g_pendPem)); ownerkey::wipe(g_pendQ, sizeof(g_pendQ)); g_pendFp[0] = 0; g_pend = false; }
static bool provGate(provision::Route r, bool mutation) {
    if (mutation ? !sameOrigin(true) : !hostOk()) { if (!mutation) fail(403, "forbidden-request"); return false; }
    provision::Ctx c;
    c.state = ownerstore::state(); c.viaAp = net::fromSetupAp(); c.staUp = net::staUp(); c.physical = ownerstore::physicalActive();
    webauth::Session* sess = currentSession();
    c.loggedIn = sess && (!mutation || webauth::equal(g_srv->header("X-Orcon-Token").c_str(), sess->csrf));
    c.passwordSet = g_credSet || g_credUnreadable; c.keyPending = g_pend;   // unreadable never reopens step 1
    if (const char* e = provision::allow(r, c)) { fail(!strcmp(e, "login-required") ? 401 : 403, e); return false; }
    return true;
}
static bool plainString(JsonDocument& d, const char* k) {   // a JSON string without embedded NUL
    return d[k].is<const char*>() && d[k].as<JsonString>().size() == strlen(d[k].as<const char*>());
}
static void handleProvState() {
    if (!provGate(provision::Route::State, false)) return;
    webauth::Session* sess = currentSession();
    uint8_t q[ownerkey::PUB_LEN];
    char fp[15] = "";
    const bool reg = ownerstore::ownerKey(q);
    if (reg) ownerkey::fingerprint(q, ownerstore::crypto(), fp);
    Out o;
    o.add("{\"state\":\"%s\",\"physical\":%s,\"physical_left_s\":%lu,\"password_set\":%s,\"logged_in\":%s,\"crypto_ok\":%s,"
          "\"sta_up\":%s,\"ap_name\":\"%s\",\"key\":{\"registered\":%s,\"fingerprint\":\"%s\",\"pending\":%s,\"pending_fingerprint\":\"%s\"},"
          "\"wifi\":{\"test\":\"%s\",\"ip\":\"%s\",\"ssid\":",
          provision::name(ownerstore::state()), ownerstore::physicalActive() ? "true" : "false",
          (unsigned long)(ownerstore::physicalLeftMs() / 1000), g_credSet ? "true" : "false", sess ? "true" : "false",
          ownerstore::cryptoReady() ? "true" : "false", net::staUp() ? "true" : "false", WIFI_AP_NAME, reg ? "true" : "false", fp,
          g_pend ? "true" : "false", g_pendFp, net::wifiTestState(), net::wifiTestIp());
    quoted(o, sess ? net::storedSsid() : "");          // the stored network name for the logged-in owner only
    o.add("}}");
    reply(200, o);
}
static void handleProvPassword() {                     // wizard step 1: the first password, over the setup AP
    static const char* const K[] = { "password" };
    JsonDocument d;
    if (!provGate(provision::Route::Password, true) || !parseBody(d, K, 1, 400)) return;
    if (!plainString(d, "password") || !webauth::passwordOk(str(d, "password"))) { fail(400, "password-weak"); return; }
    if (!authstore::kdfReady()) { fail(503, "auth-unavailable"); return; }
    if (kdfBusy()) return;
    { webauth::Cred probe{};                          // positively confirmed absent, right now; else mirror storage
      const authstore::Status st = authstore::status(probe);
      if (st != authstore::Status::Absent) {
          if (st == authstore::Status::Ok) { g_cred = probe; g_credSet = true; g_credUnreadable = false; } else g_credUnreadable = true;
          fail(403, "password-already-set"); return;
      } }
    if (!ownerstore::markSetup()) { fail(503, "state-write-failed"); return; }   // the state BEFORE the password
    if (!setPassword(str(d, "password"))) return;
    g_sessions.clear();
    g_con.add(rfcon::INFO, millis(), "installatie: beheerderswachtwoord ingesteld");
    sessionReply(g_sessions.create(millis(), authstore::random));
}
static void handleProvWifi() {                         // step 2 (and network recovery): test, then store
    static const char* const K[] = { "ssid", "password" };
    JsonDocument d;
    if (!provGate(provision::Route::WifiTest, true) || !parseBody(d, K, 2, 240)) return;
    if (!plainString(d, "ssid") || !plainString(d, "password")) { fail(400, "bad-json"); return; }
    const char* ssid = str(d, "ssid"); const char* pass = str(d, "password");
    const size_t sn = strlen(ssid), pn = strlen(pass);
    if (!sn || sn > 32) { fail(400, "bad-ssid"); return; }
    if (pn && (pn < 8 || pn > 63)) { fail(400, "bad-wifi-password"); return; }   // WPA2: 8..63; empty = an open network
    if (!net::wifiTestStart(ssid, pass)) { fail(409, "wifi-test-running"); return; }
    g_con.add(rfcon::INFO, millis(), "WiFi-test gestart (accesspoint blijft aan)");
    Out o; o.add("{\"ok\":true,\"test\":\"testing\"}"); reply(202, o);
}
static void handleProvKey() {                          // step 4: a new owner key, encrypted with the passphrase
    static const char* const K[] = { "passphrase" };
    JsonDocument d;
    if (!provGate(provision::Route::Key, true) || !parseBody(d, K, 1, 240)) return;
    const char* pass = str(d, "passphrase");
    if (!plainString(d, "passphrase") || !ownerkey::passphraseOk(pass)) { fail(400, "passphrase-weak"); return; }
    if (!ownerstore::cryptoReady()) { fail(503, "crypto-unavailable"); return; }
    if (kdfBusy()) return;
    if (g_credSet && webauth::verify(g_cred, pass, authstore::kdf)) { fail(400, "passphrase-is-password"); return; }
    uint8_t dk[ownerkey::PRIV_LEN], q[ownerkey::PUB_LEN];
    dropPending();
    const uint32_t t0 = millis();                     // logged: the hardware validation measures this (PBKDF2 50k)
    const bool ok = ownerstore::generate(dk, q) && ownerkey::encryptPem(dk, q, pass, ownerstore::crypto(), g_pendPem, sizeof(g_pendPem));
    ownerkey::wipe(dk, sizeof(dk));                    // from here the private key exists only inside the encrypted file
    if (!ok) { dropPending(); fail(503, "key-generation-failed"); return; }
    memcpy(g_pendQ, q, sizeof(q));
    ownerkey::fingerprint(q, ownerstore::crypto(), g_pendFp);
    g_pend = true;
    g_con.add(rfcon::INFO, millis(), "herstelsleutel %s aangemaakt in %lu ms; wacht op controle van het opgeslagen bestand", g_pendFp,
              (unsigned long)(millis() - t0));
    Out o; o.add("{\"ok\":true,\"fingerprint\":\"%s\",\"filename\":\"orcon-recovery-key.pem\"}", g_pendFp); reply(200, o);
}
static void handleProvKeyFile() {
    if (!provGate(provision::Route::KeyFile, false)) return;
    Out o; o.add("{\"filename\":\"orcon-recovery-key.pem\",\"fingerprint\":\"%s\",\"pem\":", g_pendFp); quoted(o, g_pendPem); o.add("}");
    reply(200, o);
}
static void handleProvKeyVerify() {                    // the owner proves the saved file opens: then it is registered
    static const char* const K[] = { "pem", "passphrase" };
    JsonDocument d;
    if (!provGate(provision::Route::KeyVerify, true) || !parseBody(d, K, 2, 1800)) return;
    if (!plainString(d, "pem") || !plainString(d, "passphrase")) { fail(400, "bad-json"); return; }
    if (!ownerstore::cryptoReady()) { fail(503, "crypto-unavailable"); return; }
    if (kdfBusy()) return;
    uint8_t dk[ownerkey::PRIV_LEN], q[ownerkey::PUB_LEN];
    const uint32_t t0 = millis();
    const char* e = ownerkey::decryptPem(str(d, "pem"), str(d, "passphrase"), ownerstore::crypto(), dk, q);
    ownerkey::wipe(dk, sizeof(dk));
    g_con.add(rfcon::INFO, millis(), "sleutelbestand gecontroleerd in %lu ms: %s", (unsigned long)(millis() - t0), e ? e : "ok");
    if (e) { fail(400, e); return; }
    if (!ownerkey::samePub(q, g_pendQ)) { fail(409, "key-mismatch"); return; }    // a file from an earlier attempt
    const bool installing = ownerstore::state() == provision::State::Setup;
    g_recovery.clear();                               // before any write: no release-key code outlives an owner key,
                                                      // not even one whose registration fails half-way (Codex OWNER-03/07)
    if (!ownerstore::registerKey(q)) { fail(503, "key-write-failed"); return; }
    g_con.add(rfcon::WARN, millis(), "eigenaarssleutel %s geregistreerd%s", g_pendFp, installing ? "; installatie voltooid" : "");
    Out o; o.add("{\"ok\":true,\"fingerprint\":\"%s\",\"installed\":%s}", g_pendFp, installing ? "true" : "false");
    dropPending();
    reply(200, o);
    if (installing) net::restartSoon();               // leave the setup AP: the device joins the home network
}
static void handleProvRecover() {                      // forgotten password: the owner's key file, physical mode only
    static const char* const K[] = { "pem", "passphrase", "password" };
    JsonDocument d;
    if (!provGate(provision::Route::Recover, true) || !parseBody(d, K, 3, 1900)) return;
    if (!plainString(d, "pem") || !plainString(d, "passphrase") || !plainString(d, "password")) { fail(400, "bad-json"); return; }
    if (!webauth::passwordOk(str(d, "password"))) { fail(400, "password-weak"); return; }
    if (const uint32_t w = g_throttle.waitMs(millis())) { failRetry(429, "login-locked", w); return; }
    if (!ownerstore::cryptoReady() || !authstore::kdfReady()) { fail(503, "crypto-unavailable"); return; }
    if (kdfBusy()) return;
    uint8_t owner[ownerkey::PUB_LEN], dk[ownerkey::PRIV_LEN], q[ownerkey::PUB_LEN];
    if (!ownerstore::ownerKey(owner)) { fail(503, "no-owner-key"); return; }
    const uint32_t t0 = millis();
    const char* e = ownerkey::decryptPem(str(d, "pem"), str(d, "passphrase"), ownerstore::crypto(), dk, q);
    ownerkey::wipe(dk, sizeof(dk));
    const unsigned long ms = (unsigned long)(millis() - t0);
    if (!e && !ownerkey::samePub(q, owner)) e = "key-mismatch";                    // a valid key, but not this owner's
    if (e) {
        g_throttle.failure(millis());
        g_con.add(rfcon::WARN, millis(), "wachtwoordherstel geweigerd (%s, %u keer op rij, %lu ms)", e, g_throttle.failures(), ms);
        failRetry(401, e, g_throttle.waitMs(millis()));
        return;
    }
    g_throttle.success();
    if (!setPassword(str(d, "password"))) return;
    g_sessions.clear();
    g_con.add(rfcon::WARN, millis(), "wachtwoord hersteld met de herstelsleutel (%lu ms); alle sessies afgemeld", ms);
    sessionReply(g_sessions.create(millis(), authstore::random));
}
#endif

void begin(WebServer& s) {
    g_srv = &s;
    uint8_t b[16]; esp_fill_random(b, sizeof(b));
    static const char HEX_[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { g_token[2 * i] = HEX_[b[i] >> 4]; g_token[2 * i + 1] = HEX_[b[i] & 15]; }
    g_token[32] = 0;
    static const char* headers[] = { "Origin", "Content-Type", "X-Orcon-Token", "Accept-Encoding", "Cookie" };
    s.collectHeaders(headers, sizeof(headers) / sizeof(headers[0]));
    s.on("/", HTTP_GET, handlePage);
    s.on("/api/setup/state",       HTTP_GET,  handleState);
#if ORCON_WEB_AUTH
    { const authstore::Status st = authstore::status(g_cred);
      g_credSet = st == authstore::Status::Ok; g_credUnreadable = st == authstore::Status::Unreadable; }
    s.on("/api/auth/login",    HTTP_POST, handleLogin);
    s.on("/api/auth/logout",   HTTP_POST, handleLogout);
    s.on("/api/auth/password", HTTP_POST, handlePassword);
    s.on("/api/auth/setup",    HTTP_POST, handleSetup);
    s.on("/api/auth/nonce",    HTTP_POST, handleAuthNonce);
    s.on("/api/auth/reset",    HTTP_POST, handleAuthReset);
#endif
#if ORCON_OWNER_MODEL
    s.on("/api/provision/state",      HTTP_GET,  handleProvState);
    s.on("/api/provision/password",   HTTP_POST, handleProvPassword);
    s.on("/api/provision/wifi",       HTTP_POST, handleProvWifi);
    s.on("/api/provision/key",        HTTP_POST, handleProvKey);
    s.on("/api/provision/key/file",   HTTP_GET,  handleProvKeyFile);
    s.on("/api/provision/key/verify", HTTP_POST, handleProvKeyVerify);
    s.on("/api/provision/recover",    HTTP_POST, handleProvRecover);
#endif
#if !ORCON_TX_POC || ORCON_TX_CONTROL
    s.on("/api/setup/restart",     HTTP_POST, [](){handleSystem(false);});
    s.on("/api/setup/wifi/forget", HTTP_POST, [](){handleSystem(true);});
#endif
    s.on("/api/profiles", HTTP_GET, handleProfiles);
    s.on("/api/profiles/save", HTTP_POST, handleProfileSave);
    s.on("/api/profiles/select", HTTP_POST, [](){handleProfileEdit("select");});
    s.on("/api/profiles/rename", HTTP_POST, [](){handleProfileEdit("rename");});
    s.on("/api/profiles/delete", HTTP_POST, [](){handleProfileEdit("delete");});
    s.on("/api/setup/console",     HTTP_GET,  handleConsole);
    s.on("/api/setup/capture",     HTTP_GET,  handleCapture);
    s.on("/api/setup/read/start",  HTTP_POST, handleReadStart);
    s.on("/api/setup/read/state",  HTTP_GET,  handleReadState);
    s.on("/api/setup/read/cancel", HTTP_POST, handleReadCancel);
    s.on("/api/setup/pair/start",  HTTP_POST, handlePairStart);
    s.on("/api/setup/pair/press",  HTTP_POST, handlePairPress);
    s.on("/api/setup/pair/state",  HTTP_GET,  handlePairState);
    s.on("/api/setup/pair/cancel", HTTP_POST, handlePairCancel);
// CAN_TX == false exactly on receive-only, non-PoC builds.
#if (!ORCON_TX_POC && RX_ONLY)
    s.on("/api/remote/send",  HTTP_POST, handleRemoteOff);   // explicit refusal, no TX code linked
    s.on("/api/remote/state", HTTP_GET,  handleRemoteOff);
#else
    s.on("/api/remote/send",  HTTP_POST, handleRemoteSend);
    s.on("/api/remote/state", HTTP_GET,  handleRemoteState);
#endif
}

void note(uint8_t kind, const char* msg) { g_con.add(kind, millis(), "%s", msg); }

void onFrame(const ramses::Frame& f, uint32_t seq, int16_t rssi) {
    ++g_ev;                                         // monotonic per boot: orders events for the page
    drainSyncs();                                   // this frame's sync note is already queued
    resolveSync(seq);
    const uint32_t othersBefore = g_read.others;
    const Why w = g_read.onFrame(f, g_ev, seq, millis(), rssi);
    consoleFrame(f, seq, rssi, w, othersBefore);
    captureFrame(f, seq, rssi);
#if ORCON_TX_POC
    // While the offer's result is pending, a W 1FC9 counts only if it was received after the
    // radio actually started this offer (the owner pass of start(), not the admission).
    const uint32_t started = txpoc::lastTxStartMs();
    const bool inFlight = g_pendOffer && started && txpoc::due(started, g_offerAt) && txpoc::due(radio::lastReceivedMs(), started + 1);
    if (!SIMULATED) g_pair.onFrame(f, orcon::ids().us, true, inFlight);
#else
    g_pair.onFrame(f, orcon::ids().us);
#endif
    if (f.cmd == 0x31D9 && f.len >= 3 && f.has_addr[0] && orcon::state().fanKnown && f.addr[0] == orcon::ids().fan &&
        (f.type == ramses::MsgType::I || f.type == ramses::MsgType::RP))
        g_cmd.onUnitMode(f.payload[2], seq);
}

#if ORCON_WEB_CONTROL
void consumeTxResults(uint32_t now) {
    const txpoc::Status& ts = txpoc::controller().status();
    // Remote diagnostics: every finished command transaction leaves one console line.
    static uint32_t loggedReq = 0;
    if (!ts.busy && ts.result.request.input.request && ts.result.request.input.request != loggedReq &&
        ts.result.request.kind == (uint8_t)txpoc::FrameKind::Mode) {
        loggedReq = ts.result.request.input.request;
#if ORCON_TX_RF_ENABLE
        const unsigned long diag = (unsigned long)radio::sx1276::restoreDiag();
#else
        const unsigned long diag = 0;
#endif
        g_con.add(ts.result.error == txpoc::Error::None ? rfcon::INFO : rfcon::WARN, now,
                  "TX #%lu stand %s: %s, fout %s, RX-herstel %s%08lX, pakketten deze boot %lu",
                  (unsigned long)loggedReq, keyOfMode(ts.result.request.input.mode),
                  ts.result.rfSent ? "verzonden (RF)" : SIMULATED ? "dry-run, niet verzonden" : "niet verzonden",
                  txpoc::errorName(ts.result.error), diag ? "mislukt 0x" : "ok ", diag, (unsigned long)txpoc::rfTxStarts());
        // Correlation evidence: compare these t values with the RX lines' sync times.
        if (ts.result.rfSent && ts.reported)
            g_con.add(rfcon::INFO, now, "TX #%lu %s: TX-start t=%lu, unitmelding t=%lu (+%lu ms)",
                      (unsigned long)loggedReq, txpoc::phaseName(ts.phase), (unsigned long)ts.result.startedMs,
                      (unsigned long)ts.reportMs, (unsigned long)(ts.reportMs - ts.result.startedMs));
        else if (ts.result.rfSent)
            g_con.add(rfcon::INFO, now, "TX #%lu %s: TX-start t=%lu, geen bruikbare unitmelding",
                      (unsigned long)loggedReq, txpoc::phaseName(ts.phase), (unsigned long)ts.result.startedMs);
#if ORCON_TX_RF_ENABLE
        if (ts.result.touched) {                      // measured airtime and the rolling budget
            const txpoc::BudgetView bv = txpoc::budget();
            const uint32_t air = txpoc::lastAirUs();
            g_con.add(rfcon::INFO, now, "TX #%lu zendtijd %lu.%lu ms; budget %u/%u per minuut, %u/%u per uur",
                      (unsigned long)loggedReq, (unsigned long)(air / 1000), (unsigned long)(air % 1000 / 100),
                      bv.minute, unsigned(TXB_PER_MINUTE), bv.hour, unsigned(TXB_PER_HOUR));
        }
#endif
    }
    const bool sentOk = ts.result.error == txpoc::Error::None && (ts.result.rfSent || SIMULATED);
    if ((g_pendOffer || g_pendAck) && !ts.busy && ts.result.request.kind != (uint8_t)txpoc::FrameKind::Mode) {
#if ORCON_TX_RF_ENABLE
        const unsigned long diag = (unsigned long)radio::sx1276::restoreDiag();
#else
        const unsigned long diag = 0;
#endif
        // TX-start t= correlates with the RX line of the unit's W 1FC9 (its sync time).
        if (diag)
            g_con.add(rfcon::WARN, now, "koppelen: %s %s, fout %s, RX-herstel mislukt 0x%08lX, TX-start t=%lu",
                      ts.result.request.kind == (uint8_t)txpoc::FrameKind::BindOffer ? "1FC9-aanbod" : "1FC9-afsluiting",
                      ts.result.rfSent ? "verzonden (RF)" : SIMULATED ? "dry-run, niet verzonden" : "niet verzonden",
                      txpoc::errorName(ts.result.error), diag, (unsigned long)ts.result.startedMs);
        else
            g_con.add(sentOk ? rfcon::INFO : rfcon::WARN, now, "koppelen: %s %s, fout %s, RX-herstel ok, %u B on-air, TX-start t=%lu",
                      ts.result.request.kind == (uint8_t)txpoc::FrameKind::BindOffer ? "1FC9-aanbod" : "1FC9-afsluiting",
                      ts.result.rfSent ? "verzonden (RF)" : SIMULATED ? "dry-run, niet verzonden" : "niet verzonden",
                      txpoc::errorName(ts.result.error), unsigned(ts.result.bytes), (unsigned long)ts.result.startedMs);
    }
    if (g_pendOffer && !ts.busy && ts.result.request.kind == (uint8_t)txpoc::FrameKind::BindOffer) {
        if (SIMULATED && sentOk) {
            g_pairDryComplete = true;
            g_pair.cancel(); // release the session; no RF send, confirmation or NVS identity
        } else g_pair.bindResult(sentOk, now);
        g_pendOffer = false;
    }
    if (g_pendAck && !ts.busy && ts.result.request.kind == (uint8_t)txpoc::FrameKind::BindAck) {
        if (sentOk && !SIMULATED) g_pair.markAckSent();
        else g_pair.txFailed = true; // one attempt only, including failed acknowledgements
        g_pendAck = false;
    }
}
#endif

void tick(uint32_t now) {
    g_read.tick(now);
    consoleTick(now);
#if ORCON_WEB_AUTH
    g_sessions.sweep(now); g_throttle.sweep(now); g_recovery.sweep(now);   // before a millis() wrap revives them
#endif
#if ORCON_TX_POC
    consumeTxResults(now); // settle completions before expiry or any follow-up
    g_pair.tick(now, pairDone(), pairFan());
    static bool confirmLogged = false;
    if (g_pair.st == St::Active && g_pair.sent && g_pair.confirmed && !confirmLogged)
        g_con.add(rfcon::INFO, now, "koppelen: unit bevestigde (W 1FC9 00 31D9 aan ons ID); afsluiting volgt");
    confirmLogged = g_pair.st == St::Active && g_pair.sent && g_pair.confirmed;
    // The only automatic TX: compiled out wherever pairing is unsupported (RF builds without
    // ORCON_TX_PAIR). One closing ack per session, inside the RF budget (the press reserved it).
    if (CAN_PAIR && g_pair.st == St::Active && g_pair.sent && g_pair.confirmed && !g_pair.ackSent &&
        !g_pair.txFailed && !g_pendAck) {
        orcon::Ids ids = orcon::ids(); ids.fan = g_pair.from;
        const rlog::Device* heard = nullptr; const size_t heardN = rlog::devices(&heard);
        const bool identityClear = !txpoc::identityConflict(ids, rlog::seen(), heard, heardN);
        if (txpoc::controller().submitBind((uint8_t)txpoc::FrameKind::BindAck, ids,
                txpoc::rxReady(), identityClear, now) == txpoc::Admission::Accepted)
            g_pendAck = true;
        // Busy/Unavailable: retried on a later pass while the window lasts.
    }
    // A completed web-control pairing hands the learned unit to net.cpp for NVS.
    if (!SIMULATED && g_pair.st == St::Done && g_pair.webctlPaired() && txpoc::validId(g_pair.from)) g_learnedFan = g_pair.from;
#endif
    g_pair.tick(now, pairDone(), pairFan());
    g_cmd.tick(now);
}

#if ORCON_WEB_CONTROL
bool takeLearnedFan(ramses::DeviceId& out) {
    if (!txpoc::validId(g_learnedFan) || orcon::ids().fan == g_learnedFan) return false;
    out = g_learnedFan;
    return true;
}
#endif

} // namespace setupweb
#endif // !ORCON_TX_POC || ORCON_WEB_CONTROL
