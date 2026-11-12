// net.cpp — WiFi (STA + SoftAP), web UI + setup + RF log, MQTT, OLED.
// WiFi/MQTT settings AND device IDs persist in NVS (Preferences).
#include "net.h"
#include "config.h"
#include "orcon.h"
#include "orcon_log.h"
#include "ramses_radio.h"
#include "rx_only.h"
#include "net_cfg.h"
#include "oled_power.h"
#if defined(USE_CC1101_TX) || defined(RADIO_CC1101_ASYNC)
#include "cc1101_tx.h"
#endif
#include "web_page.h"
#include "setup_web.h"
#include "ota_signed.h"
#include "remote_profiles.h"
#include "house_img.h"

#include <Arduino.h>
#include <WiFi.h>
#if !RX_ONLY
#include <ArduinoOTA.h>
#include <Update.h>
#endif
#include <WebServer.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <Preferences.h>
#if ORCON_TX_POC && !ORCON_WEB_CONTROL
#include <esp_system.h>
#include "tx_poc_runtime.h"
#include "tx_poc_page.h"
#endif

namespace net {

static WebServer    server(80);
static WiFiClient   wifiClient;
static PubSubClient mqtt(wifiClient);
static Preferences  prefs;
#if ORCON_TX_POC && !ORCON_WEB_CONTROL
static txpoc::Security g_pocSecurity;
static char g_pocBootToken[33] = "";
static uint32_t g_pocBootMs = 0;
static bool g_pocIdentityValid = false, g_pocAuthenticated = false;
#endif

#if !defined(NO_OLED)
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C
    oled(U8G2_R0, PIN_OLED_RST, PIN_OLED_SCL, PIN_OLED_SDA);
#endif
#if defined(BOARD_HELTEC_V2)
static oledpower::Power g_oledPower;
#endif

static bool     g_apMode    = false;
static bool     g_dirty     = true;
static uint32_t g_lastPub   = 0;
static uint32_t g_lastMqtt  = 0;
static uint32_t g_rebootAt  = 0;
static char     g_ip[24]    = "0.0.0.0";

static String   g_ssid, g_pass, g_mqttHost;
// MQTT runs only with a broker configured: NVS "mqtt" = "" means off (net_cfg.h).
static bool mqttOn() { return MQTT_ENABLED && g_mqttHost.length() > 0; }
static bool     g_maskIds   = true;   // mask device IDs in the UI (privacy, default on)
static bool     g_showFilter = true;  // show the filter row (some units never report it)

// Room temperatures for the airflow house: one Domoticz IDX per room, filled
// live from the broker's "domoticz/out" feed. Order matches the widget's pins.
static const char* const ROOM_KEYS[7] = {"attic","bed1","bath","bed2","kitchen","living","office"};
static uint32_t g_roomIdx[7]  = {0,0,0,0,0,0,0};   // 0 = unmapped
static float    g_roomTemp[7] = {0,0,0,0,0,0,0};
static uint32_t g_roomTs[7]   = {0,0,0,0,0,0,0};   // 0 = never received
static uint32_t g_domRx = 0;       // diag: number of domoticz/out messages seen
static uint32_t g_domLastIdx = 0;  // diag: last IDX seen on domoticz/out
static uint32_t g_cmdRejected = 0; // RX-only: refused /api/cmd + MQTT requests

// ---------------------------------------------------------------------------
// Persistent config (NVS): WiFi, MQTT host, device IDs
// ---------------------------------------------------------------------------
static void saveIds(uint8_t uc, uint32_t us, uint8_t fc, uint32_t fs);  // fwd

static void loadConfig() {
    prefs.begin("orcon", true);
    g_ssid     = prefs.getString("ssid", WIFI_SSID);
    g_pass     = prefs.getString("pass", WIFI_PASSWORD);
    g_mqttHost = prefs.getString("mqtt", MQTT_HOST);
    uint32_t uc = prefs.getUInt("uc", DEV_US_CLASS);
    uint32_t us = prefs.getUInt("us", 0);        // 0 => never stored -> generate a unique one
    uint32_t fc = prefs.getUInt("fc", DEV_FAN_CLASS);
    uint32_t fs = prefs.getUInt("fs", DEV_FAN_SERIAL);
    int16_t fsv[7]; char key[4];
    for (int i = 0; i < 7; i++) { snprintf(key, sizeof(key), "f%d", i); fsv[i] = (int16_t)prefs.getInt(key, -1); }
    int16_t bpv = (int16_t)prefs.getInt("bp", -1);
    bool     ar = prefs.getBool("ar", false);
    bool     pl = prefs.getBool("pl", false);
    g_maskIds    = prefs.getBool("mask", true);
    g_showFilter = prefs.getBool("fltr", true);
#if defined(BOARD_HELTEC_V2)
    g_oledPower.setMode(oledpower::storedMode(prefs.getString("oled_mode", "auto").c_str()), millis());
#endif
    for (int i = 0; i < 7; i++) { char rk[4]; snprintf(rk, sizeof(rk), "r%d", i); g_roomIdx[i] = prefs.getUInt(rk, 0); }
    prefs.end();
#if ORCON_TX_POC && !ORCON_WEB_CONTROL
    // Only existing, fully valid NVS identities are authorized; never generate/write IDs.
    prefs.begin("orcon", true);
    uint32_t storedUc = prefs.getUInt("uc", 0), storedFc = prefs.getUInt("fc", 0);
    uint32_t storedUs = prefs.getUInt("us", 0), storedFs = prefs.getUInt("fs", 0);
    prefs.end();
    g_pocIdentityValid = storedUc > 0 && storedUc < 63 && storedFc > 0 && storedFc < 63 &&
        storedUs > 0 && storedUs <= 0x3FFFF && storedFs > 0 && storedFs <= 0x3FFFF;
#endif
#if ORCON_WEB_CONTROL
    // Validate before narrowing NVS classes to uint8_t. RF images only use an
    // existing identity; never silently introduce a new transmitter on first boot.
    if (uc == 0 || uc >= 63 || us > 0x3FFFF) { uc = 0; us = 0; }
    if (fc == 0 || fc >= 63 || fs > 0x3FFFF) { fc = 0; fs = 0; }
#endif
    bool freshId = (us == 0 && uc != 0);
#if !ORCON_TX_POC || (ORCON_WEB_CONTROL && !ORCON_TX_RF_ENABLE)
    if (freshId) {                               // first boot: a unique serial from the chip ID
        us = (uint32_t)(ESP.getEfuseMac() & 0x3FFFFUL);   // 18-bit RAMSES serial space
        if (us == 0) us = 1;
        saveIds(uc, us, fc, fs);                 // lock the generated identity into NVS
    }
#endif
    (void)freshId;
    orcon::setIds(uc, us, fc, fs);               // apply stored / generated identities
    for (int i = 0; i < 7; i++) orcon::setFanSaved(i, fsv[i]);
    orcon::setBypassSaved(bpv);
    orcon::setAutoRestore(ar);
    orcon::setPassiveLearn(pl);
}
static void saveKeeper() {
    prefs.begin("orcon", false);
    char key[4];
    for (int i = 0; i < 7; i++) { snprintf(key, sizeof(key), "f%d", i); prefs.putInt(key, orcon::fanSaved(i)); }
    prefs.putInt("bp", orcon::bypassSaved());
    prefs.putBool("ar", orcon::autoRestore());
    prefs.end();
}
static void saveWifi(const char* ssid, const char* pass, const char* mqtt) {
    // Only overwrite a field that was actually provided — otherwise a blank
    // (e.g. saving just the MQTT host) would wipe the stored WiFi credentials.
    prefs.begin("orcon", false);
    if (ssid && strlen(ssid)) prefs.putString("ssid", ssid);
    if (pass && strlen(pass)) prefs.putString("pass", pass);
    if (mqtt && strlen(mqtt)) prefs.putString("mqtt", mqtt);
    prefs.end();
}
static void saveIds(uint8_t uc, uint32_t us, uint8_t fc, uint32_t fs) {
    prefs.begin("orcon", false);
    prefs.putUInt("uc", uc); prefs.putUInt("us", us);
    prefs.putUInt("fc", fc); prefs.putUInt("fs", fs);
    prefs.end();
}

static void idStr(const ramses::DeviceId& d, char* out, size_t cap) {
    snprintf(out, cap, "%02u:%06lu", d.cls, (unsigned long)d.serial);
}

// ---------------------------------------------------------------------------
// State -> JSON
// ---------------------------------------------------------------------------
// Returns the JSON length, or 0 if it does not fit (never emit truncated JSON).
static size_t buildStateJson(char* out, size_t cap) {
    const orcon::State& s = orcon::state();
    JsonDocument d;
    d["mode"]      = orcon::modeName(s.mode);           // reported by the selected unit only
    d["mode_age"]  = s.modeMs ? (int32_t)((millis() - s.modeMs) / 1000) : -1;
    d["supply"]    = s.supplyPct;
    d["exhaust"]   = s.exhaustPct;
    d["t_indoor"]  = s.tempIndoor;
    d["t_outdoor"] = s.tempOutdoor;
    d["t_supply"]  = s.tempSupply;
    d["t_exhaust"] = s.tempExhaust;
    d["humidity"]  = s.humidity;
    d["co2"]       = s.co2;
    d["demand"]    = s.demand;
    d["bypass"]    = s.bypassOpen;
    d["filter"]    = s.filterRemaining;
    d["fault"]     = s.fault;
    d["fan_known"] = s.fanKnown;
    const char* ps = "idle";
    switch (orcon::pairState()) {
        case orcon::PairState::Searching: ps = "searching"; break;
        case orcon::PairState::Paired:    ps = "paired";    break;
        case orcon::PairState::Timeout:   ps = "timeout";   break;
        default: break;
    }
    d["pair"]      = ps;
    d["pair_left"] = (uint32_t)(orcon::pairWindowLeftMs() / 1000);
    d["plearn"]    = orcon::passiveLearn();
    d["mask_ids"]  = g_maskIds;
    d["show_filter"] = g_showFilter;
#if defined(BOARD_HELTEC_V2)
    d["oled_available"] = true;
    d["oled_mode"] = oledpower::modeName(g_oledPower.mode());
    d["oled_on"] = g_oledPower.awake();
#else
    d["oled_available"] = false;  // this setting only controls the Heltec V2 OLED
#endif
    char uid[16], fid[16];
    idStr(orcon::ids().us,  uid, sizeof(uid));
    idStr(orcon::ids().fan, fid, sizeof(fid));
    d["us_id"]     = uid;
    d["fan_id"]    = fid;
    d["rssi"]      = (int)radio::lastRssi();
    d["radio"]     = radio::name();
    d["online"]    = s.lastFanMs != 0 && (millis() - s.lastFanMs) < 600000UL;   // the selected unit, not any traffic
    // Last remote command to the unit: a request, confirmed only by the unit's own 31D9.
    char rid[16]; idStr(s.remoteId, rid, sizeof(rid));
    d["remote_cmd"]       = orcon::modeName(s.remoteMode);
    d["remote_id"]        = s.remoteMs ? rid : "";
    d["remote_age"]       = s.remoteMs ? (int32_t)((millis() - s.remoteMs) / 1000) : -1;
    d["remote_confirmed"] = s.remoteConfirmedMs != 0;
    d["cmd_mode"]  = orcon::modeName(s.cmdMode);
    d["selftest"]  = s.selftest;
    d["fan_age"]   = s.lastFanMs ? (int)((millis() - s.lastFanMs) / 1000) : -1;
    d["tx_ack"]    = s.txAckMs && (millis() - s.txAckMs) < 15000;
    d["autorestore"] = orcon::autoRestore();
    d["rx_n"]      = s.rxCount;
    d["tx_n"]      = s.txCount;
    d["uptime_ms"] = millis();  // device monotonic time for RF-log age display
    d["rx_only"]   = (bool)RX_ONLY;   // receive-only firmware: commands are refused
    d["mqtt_on"]   = mqttOn();
    d["mqtt_connected"] = mqttOn() && mqtt.connected();
    d["fw"]        = FW_VERSION;
    d["tx_refused"] = s.txRefused;
    d["cmd_rejected"] = g_cmdRejected;
    const radio::RxDiagnostics rf = radio::rxDiagnostics();
    d["rf_uart_bytes"] = rf.uartBytes;
    d["rf_sync_hits"] = rf.syncHits;
    d["rf_core_sync_hits"] = rf.coreSyncHits;
    d["rf_frames"] = rf.frames;
    d["rf_uart_overflows"] = rf.uartOverflows;
    d["rf_opmode"] = rf.opMode;
    d["rf_lna"] = rf.lna;
    d["rf_rssi_now"] = rf.rssiNow;
    d["rf_buffer_resets"] = rf.bufferResets;
    d["rf_buffered_bytes"] = rf.bufferedBytes;
    d["rf_pending_bytes"] = rf.pendingBytes;
    d["rf_last_uart_age_ms"] = rf.lastUartMs ? (int32_t)(millis() - rf.lastUartMs) : -1;
    d["rf_last_sync_age_ms"] = rf.lastSyncMs ? (int32_t)(millis() - rf.lastSyncMs) : -1;
#if defined(RADIO_SX1276)
    static const char* const RX_STATE[] = { "receiving", "recovering", "failed" };
    d["rf_state"]         = RX_STATE[rf.rxState < 3 ? rf.rxState : 2];
    d["rf_err"]           = rf.rxErr;
    d["rf_recoveries"]    = rf.rxRecoveries;
    d["rf_lost"]          = rf.rxLost;
    d["rf_queue_depth"]   = rf.queueDepth;
    d["rf_queue_max"]     = rf.queueHighWater;
    d["rf_queue_drops"]   = rf.queueDrops;
    d["rf_uart_pend_max"] = rf.uartPendMax;
    d["rf_task_max_us"]   = rf.taskMaxUs;
    d["rf_stack_free"]    = rf.stackFree;
    d["heap_min"]         = ESP.getMinFreeHeap();
#endif
    d["raw31da"]   = s.raw31DA;
    d["raw31da_age"] = s.raw31DAms ? (int)((millis() - s.raw31DAms) / 1000) : -1;
    d["last_rx"]   = s.lastRx;
    d["last_tx"]   = s.lastTx;
    JsonArray sv = d["saved"].to<JsonArray>();
    for (int i = 0; i < 7; i++) sv.add(orcon::fanSaved(i));
    d["byp_saved"] = orcon::bypassSaved();
    JsonObject rooms = d["rooms"].to<JsonObject>();        // live room temps (Domoticz IDX)
    for (int i = 0; i < 7; i++)
        if (g_roomIdx[i] && g_roomTs[i] && (millis() - g_roomTs[i]) < 3600000UL)
            rooms[ROOM_KEYS[i]] = g_roomTemp[i];
    JsonArray ridx = d["room_idx"].to<JsonArray>();         // configured IDX (for the settings form)
    for (int i = 0; i < 7; i++) ridx.add(g_roomIdx[i]);
    d["dom_rx"]   = g_domRx;                                 // diag: domoticz/out message count
    d["dom_last"] = g_domLastIdx;
#if defined(USE_CC1101_TX) || defined(RADIO_CC1101_ASYNC)
    d["cc1101"]    = cc1101tx::version();
    d["urx"]       = cc1101tx::uartBytes();   // bytes seen on the GDO2 RX line (diag)
#endif
    size_t need = measureJson(d);
    if (need >= cap) { Serial.printf("[net] state JSON %u B > %u B buffer: skipped\n", (unsigned)need, (unsigned)cap); return 0; }
    return serializeJson(d, out, cap);
}

// ---------------------------------------------------------------------------
// Command handling (web POST + MQTT)
// ---------------------------------------------------------------------------
// Returns the refused key (RX-only) or nullptr. A refused request is not applied
// at all: no partial settings, no NVS writes, no TX attempt.
static const char* applyCommand(JsonDocument& d) {
#if RX_ONLY
    for (JsonPair kv : d.as<JsonObject>())
        if (!rxOnlyAllowedCmd(kv.key().c_str())) { g_cmdRejected++; return kv.key().c_str(); }
#endif
    if (!d["mode"].isNull()) {
        orcon::Mode m = orcon::modeFromName(d["mode"] | "");
        if (m != orcon::Mode::Unknown) orcon::setMode(m);
    }
    if (!d["timer"].isNull())  orcon::setTimer((uint16_t)(d["timer"] | 0));
    if (!d["pair"].isNull())   orcon::startPairing();      // open the bind window
    if (!d["plearn"].isNull()) {                            // advanced auto-learn toggle
        bool on = (int)(d["plearn"] | 0) != 0;
        orcon::setPassiveLearn(on);
        prefs.begin("orcon", false); prefs.putBool("pl", on); prefs.end();
    }
    if (!d["maskids"].isNull()) {                           // privacy: mask device IDs in the UI
        g_maskIds = (int)(d["maskids"] | 0) != 0;
        prefs.begin("orcon", false); prefs.putBool("mask", g_maskIds); prefs.end();
    }
    if (!d["showfilter"].isNull()) {                        // show/hide the filter row
        g_showFilter = (int)(d["showfilter"] | 0) != 0;
        prefs.begin("orcon", false); prefs.putBool("fltr", g_showFilter); prefs.end();
    }
    if (!d["roomidx"].isNull()) {                          // Domoticz IDX per airflow room
        JsonArray a = d["roomidx"].as<JsonArray>();
        prefs.begin("orcon", false);
        for (int i = 0; i < 7; i++) {
            if (i < (int)a.size()) g_roomIdx[i] = (uint32_t)(a[i] | 0);
            char rk[4]; snprintf(rk, sizeof(rk), "r%d", i); prefs.putUInt(rk, g_roomIdx[i]);
        }
        prefs.end();
    }
    if (!d["selftest"].isNull()) orcon::setSelftest((int)(d["selftest"] | 0) != 0);
    if (!d["connect"].isNull())  orcon::sendConnect();
    if (!d["status"].isNull())   orcon::requestStatus();
    if (!d["bypass"].isNull()) {
        const char* b = d["bypass"] | "";
        uint8_t v = 0xFF;
        if (!strcmp(b, "open")) v = 0xC8; else if (!strcmp(b, "close")) v = 0x00;
        orcon::setBypass(v);
        orcon::setBypassSaved(v); saveKeeper();          // keep across power loss
    }
    if (!d["ac_mode"].isNull()) {
        const char* a = d["ac_mode"] | "";
        uint8_t w = 0;
        if (!strcmp(a, "exhaust")) w = 1; else if (!strcmp(a, "supply")) w = 2;
        orcon::setAcMode(w); saveKeeper();
    }
    if (!d["ac_sup"].isNull() || !d["ac_exh"].isNull()) {
        uint8_t sup = (uint8_t)(d["ac_sup"] | 50);
        uint8_t exh = (uint8_t)(d["ac_exh"] | 50);
        bool    byp = (int)(d["ac_byp"] | 1) != 0;
        orcon::setAcCustom(sup, exh, byp); saveKeeper();
    }
    if (!d["autorestore"].isNull()) { orcon::setAutoRestore((int)(d["autorestore"] | 0) != 0); saveKeeper(); }
    if (!d["reapply"].isNull())      orcon::reapplySaved();
    if (!d["forget"].isNull())     { for (int i = 0; i < 7; i++) orcon::setFanSaved(i, -1); orcon::setBypassSaved(-1); saveKeeper(); }
    if (!d["raw_code"].isNull()) {
        const char* tn = d["raw_type"] | "I";
        uint8_t ty = 0;
        if (!strcmp(tn, "RQ")) ty = 1; else if (!strcmp(tn, "RP")) ty = 2;
        else if (!strcmp(tn, "W")) ty = 3;
        uint16_t code = (uint16_t)strtol(d["raw_code"] | "0000", nullptr, 16);
        uint8_t pl[64]; uint8_t n = 0;
        const char* h = d["raw_pl"] | "";
        while (h[0] && h[1] && n < 64) {
            char b[3] = { h[0], h[1], 0 };
            pl[n++] = (uint8_t)strtol(b, nullptr, 16);
            h += 2; while (*h == ' ') h++;
        }
        orcon::sendRaw(ty, code, pl, n);
    }
    g_dirty = true;
    return nullptr;
}

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------
static void handleRoot()  { server.send_P(200, "text/html", INDEX_HTML); }
static void handleWifi()  { server.send_P(200, "text/html", SETUP_HTML); }
static void handleLogPg() { server.send_P(200, "text/html", LOG_HTML);  }
static void handleWidget() { server.send_P(200, "text/html", WIDGET_HTML); }
static void handleFlow()   { server.send_P(200, "text/html", FLOW_HTML);   }
static void handleHouseImg() {
    server.sendHeader("Cache-Control", "max-age=86400");
    server.send_P(200, "image/webp", (PGM_P)HOUSE_JPG, HOUSE_JPG_LEN);
}
static void handleDebug()  { server.send_P(200, "text/html", DEBUG_HTML);  }
#if !RX_ONLY
static void handleUpdatePage() { server.send_P(200, "text/html", UPDATE_HTML); }
static void handleUpdateUpload() {
    HTTPUpload& up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
        Update.begin(UPDATE_SIZE_UNKNOWN);
    } else if (up.status == UPLOAD_FILE_WRITE) {
        Update.write(up.buf, up.currentSize);
    } else if (up.status == UPLOAD_FILE_END) {
        Update.end(true);
    }
}
static void handleUpdateDone() {
    bool ok = !Update.hasError();
    server.sendHeader("Connection", "close");
    server.send(200, "application/json", ok ? "{\"ok\":1}" : "{\"ok\":0}");
    if (ok) g_rebootAt = millis() + 1500;   // reboot into the new firmware
}
#endif

static char g_stateBuf[3072];                  // loop task only (HTTP + MQTT publish)
static void handleState() {
    if (!buildStateJson(g_stateBuf, sizeof(g_stateBuf))) { server.send(500, "application/json", "{\"err\":\"state too large\"}"); return; }
    server.send(200, "application/json", g_stateBuf);
}
static void handleNetCfg() {   // current WiFi SSID + MQTT host, to prefill the setup page
    JsonDocument d;
    d["ssid"] = g_ssid;
    d["mqtt"] = g_mqttHost;
    d["mqtt_on"] = mqttOn();
    char buf[192]; serializeJson(d, buf, sizeof(buf));
    server.send(200, "application/json", buf);
}
static void handleCmd() {
    JsonDocument d;
    if (deserializeJson(d, server.arg("plain"))) {
        server.send(400, "application/json", "{\"err\":\"bad json\"}"); return;
    }
    if (const char* refused = applyCommand(d)) {
        JsonDocument r; r["err"] = "rx-only"; r["key"] = refused;
        char b[128]; serializeJson(r, b, sizeof(b));
        server.send(403, "application/json", b); return;
    }
    server.send(200, "application/json", "{\"ok\":1}");
}

// GET /api/log -> { log:[...], devices:[...], seen_remote, seen_fan }  (devices = discovery only)
static void handleLog() {
    static char buf[7168];
    size_t n = snprintf(buf, sizeof(buf), "{\"log\":");
    n += rlog::toJson(buf + n, sizeof(buf) - n - 1400);     // leave room for the device list
    n += snprintf(buf + n, sizeof(buf) - n, ",\"devices\":");
    n += rlog::devicesJson(buf + n, sizeof(buf) - n - 160);
    const rlog::Seen& s = rlog::seen();
    char rem[16] = "", fan[16] = "";
    if (s.haveRemote) idStr(s.remote, rem, sizeof(rem));
    if (s.haveFan)    idStr(s.fan,    fan, sizeof(fan));
    snprintf(buf + n, sizeof(buf) - n,
             ",\"seen_remote\":\"%s\",\"seen_fan\":\"%s\",\"uptime_ms\":%lu}",
             rem, fan, (unsigned long)millis());
    server.send(200, "application/json", buf);
}

// POST /api/ids {us_class,us_serial,fan_class,fan_serial}
static void handleIds() {
    JsonDocument d;
    if (deserializeJson(d, server.arg("plain"))) {
        server.send(400, "application/json", "{\"err\":\"bad json\"}"); return;
    }
    // Missing fields keep the stored identity (selecting a unit must not reset ours).
    const orcon::Ids& cur = orcon::ids();
    int32_t uc = d["us_class"]   | (int32_t)cur.us.cls;
    int32_t us = d["us_serial"]  | (int32_t)cur.us.serial;
    int32_t fc = d["fan_class"]  | (int32_t)cur.fan.cls;
    int32_t fs = d["fan_serial"] | (int32_t)(orcon::state().fanKnown ? cur.fan.serial : 0);
    if (uc < 0 || uc > 0x3F || fc < 0 || fc > 0x3F || us <= 0 || us > 0x3FFFF || fs < 0 || fs > 0x3FFFF) {
        server.send(400, "application/json", "{\"err\":\"class 0-63, serial 1-262143 (fan 0 = none)\"}"); return;
    }
    saveIds(uc, us, fc, fs);
    orcon::setIds(uc, us, fc, fs);
    g_dirty = true;
    server.send(200, "application/json", "{\"ok\":1}");
}

// GET /api/scan
static void handleScan() {
    int n = WiFi.scanNetworks();
    JsonDocument d;
    JsonArray a = d.to<JsonArray>();
    for (int i = 0; i < n && i < 30; i++) {
        JsonObject o = a.add<JsonObject>();
        o["ssid"] = WiFi.SSID(i);
        o["rssi"] = WiFi.RSSI(i);
        o["lock"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
    String out; serializeJson(d, out);
    server.send(200, "application/json", out);
    WiFi.scanDelete();
}

// POST /api/wifi {ssid,pass,mqtt} -> save + reboot
static void handleWifiSave() {
    JsonDocument d;
    if (deserializeJson(d, server.arg("plain"))) {
        server.send(400, "application/json", "{\"err\":\"bad json\"}"); return;
    }
    String ssid = d["ssid"] | "";
    String pass = d["pass"] | "";
    String mqtt = d["mqtt"] | "";
    mqtt.trim();
    const MqttSave act = mqttSaveAction(!d["mqtt_on"].isNull(), d["mqtt_on"].as<bool>(), mqtt.c_str());
    if (act == MqttSave::Invalid) {
        server.send(400, "application/json", "{\"err\":\"MQTT on needs a broker host or IPv4 (no scheme/port)\"}"); return;
    }
    if (ssid.length() == 0 && pass.length() == 0 && act == MqttSave::Keep) {
        server.send(400, "application/json", "{\"err\":\"nothing to save\"}"); return;
    }
    saveWifi(ssid.c_str(), pass.c_str(), "");     // Wi-Fi only; MQTT below
    if (act != MqttSave::Keep) {
        prefs.begin("orcon", false);
        prefs.putString("mqtt", act == MqttSave::Off ? "" : mqtt.c_str());   // "" = explicitly off
        prefs.end();
    }
    server.send(200, "application/json", "{\"ok\":1}");
    g_rebootAt = millis() + 1200;
}

// ---------------------------------------------------------------------------
// MQTT
// ---------------------------------------------------------------------------
// Cheap IDX scan so the high-volume domoticz/out feed doesn't get a full JSON
// parse per message (that starves the timing-sensitive RAMSES RX).
static uint32_t quickIdx(const byte* p, unsigned int len) {
    for (unsigned int i = 0; i + 4 < len; i++) {
        if (p[i]=='i' && p[i+1]=='d' && p[i+2]=='x') {
            unsigned int j = i + 3;
            while (j < len && (p[j]==' '||p[j]=='"'||p[j]==':')) j++;
            uint32_t v = 0; bool any = false;
            while (j < len && p[j]>='0' && p[j]<='9') { v = v*10 + (p[j]-'0'); j++; any = true; }
            if (any) return v;
        }
    }
    return 0;
}
static void mqttCallback(char* topic, byte* payload, unsigned int len) {
    if (topic && !strcmp(topic, "domoticz/out")) {         // high-volume feed: filter cheaply first
        g_domRx++;
        uint32_t idx = quickIdx(payload, len);
        if (!idx) return;
        g_domLastIdx = idx;
        int slot = -1;
        for (int i = 0; i < 7; i++) if (g_roomIdx[i] == idx) { slot = i; break; }
        if (slot < 0) return;                              // not one of our rooms -> skip the parse
        JsonDocument d;
        if (deserializeJson(d, payload, len)) return;
        // temp is in "Temp" (weather stations) or "svalue1" (plain Temp / Temp+Hum); "svalue" last
        float t;
        if      (!d["Temp"].isNull())    t = d["Temp"].as<float>();
        else if (!d["svalue1"].isNull()) t = (float)atof(d["svalue1"] | "0");
        else                             t = (float)atof(d["svalue"]  | "0");
        g_roomTemp[slot] = t; g_roomTs[slot] = millis();
        return;
    }
    JsonDocument d;                                        // command topic (small, low volume)
    if (deserializeJson(d, payload, len)) return;
    applyCommand(d);
}
#if MQTT_ENABLED
// ---- Home Assistant MQTT auto-discovery (Domoticz reads the same format) ----
static void addDev(JsonDocument& d) {
    JsonObject dev = d["dev"].to<JsonObject>();
    dev["ids"].to<JsonArray>().add(MQTT_NODE_ID);
    dev["name"] = RX_ONLY ? "Orcon RX-only monitor" : "Orcon 15RF";
    dev["mf"]   = "Orcon (DIY)";
    dev["mdl"]  = RX_ONLY ? "Heltec V2 SX1276 RX-only" : "HRC400 15RF";
}
static void pubCfg(const char* comp, const char* oid, JsonDocument& d) {
    char buf[640]; size_t n = serializeJson(d, buf, sizeof(buf));
    char topic[100];
    snprintf(topic, sizeof(topic), "homeassistant/%s/" MQTT_NODE_ID "/%s/config", comp, oid);
    mqtt.publish(topic, (const uint8_t*)buf, n, true);
}
static void pubSensor(const char* oid, const char* name, const char* key,
                      const char* unit, const char* dcla) {
    JsonDocument d;
    d["name"]    = name;
    d["uniq_id"] = String(MQTT_UID_PREFIX) + oid;
    d["stat_t"]  = MQTT_BASE_TOPIC "/state";
    d["val_tpl"] = String("{{ value_json.") + key + " }}";
    if (unit && *unit) d["unit_of_meas"] = unit;
    if (dcla && *dcla) d["dev_cla"]      = dcla;
    addDev(d);
    pubCfg("sensor", oid, d);
}
static void pubBinary(const char* oid, const char* name, const char* tpl, const char* dcla) {
    JsonDocument d;
    d["name"]    = name;
    d["uniq_id"] = String(MQTT_UID_PREFIX) + oid;
    d["stat_t"]  = MQTT_BASE_TOPIC "/state";
    d["val_tpl"] = tpl;
    d["pl_on"]   = "ON";
    d["pl_off"]  = "OFF";
    if (dcla && *dcla) d["dev_cla"] = dcla;
    addDev(d);
    pubCfg("binary_sensor", oid, d);
}
static void pubSelect(const char* oid, const char* name, const char* valTpl,
                      const char* cmdTpl, const char** opts, int nopts) {
    JsonDocument d;
    d["name"]    = name;
    d["uniq_id"] = String(MQTT_UID_PREFIX) + oid;
    d["stat_t"]  = MQTT_BASE_TOPIC "/state";
    d["val_tpl"] = valTpl;
    d["cmd_t"]   = MQTT_BASE_TOPIC "/cmd";
    d["cmd_tpl"] = cmdTpl;
    JsonArray o = d["options"].to<JsonArray>();
    for (int i = 0; i < nopts; i++) o.add(opts[i]);
    addDev(d);
    pubCfg("select", oid, d);
}
static void publishDiscovery() {
    pubSensor("t_indoor",  "Indoor temperature",  "t_indoor",  "\xC2\xB0""C", "temperature");
    pubSensor("t_outdoor", "Outdoor temperature", "t_outdoor", "\xC2\xB0""C", "temperature");
    pubSensor("t_supply",  "Supply temperature",  "t_supply",  "\xC2\xB0""C", "temperature");
    pubSensor("t_exhaust", "Exhaust temperature", "t_exhaust", "\xC2\xB0""C", "temperature");
    pubSensor("humidity",  "Humidity",            "humidity",  "%",   "humidity");
    pubSensor("co2",       "CO2",                 "co2",       "ppm", "carbon_dioxide");
    pubSensor("supply",    "Supply fan",          "supply",    "%",   "");
    pubSensor("exhaust",   "Exhaust fan",         "exhaust",   "%",   "");
    pubSensor("filter",    "Filter remaining",    "filter",    "%",   "");
    pubSensor("rssi",      "RF signal",           "rssi",      "dBm", "signal_strength");
    pubBinary("bypass", "Bypass open", "{{ 'ON' if value_json.bypass else 'OFF' }}", "opening");
    pubBinary("fault",  "Fault",       "{{ 'ON' if value_json.fault  else 'OFF' }}", "problem");
#if !RX_ONLY                       // no command entities on a receive-only board
    static const char* MODES[] = {"away","auto","low","medium","high","boost"};
    pubSelect("mode", "Mode", "{{ value_json.mode }}", "{\"mode\":\"{{ value }}\"}", MODES, 6);
    static const char* BYP[] = {"open","auto","close"};
    pubSelect("bypass_set", "Bypass control",
              "{{ 'open' if value_json.bypass else 'close' }}",
              "{\"bypass\":\"{{ value }}\"}", BYP, 3);
#endif
}
#endif

static void mqttReconnect() {
#if MQTT_ENABLED
    if (!mqttOn() || mqtt.connected() || g_apMode) return;
    // Back off when the broker is unreachable so a blocking connect() doesn't
    // repeatedly stall the loop and starve the radio RX. Grows 5s -> 5min.
    static uint32_t backoff = 5000;
    if (millis() - g_lastMqtt < backoff) return;
    g_lastMqtt = millis();
    String cid = "orcon15rf-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    bool ok = strlen(MQTT_USER)
        ? mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASS)
        : mqtt.connect(cid.c_str());
    if (ok) { if (!RX_ONLY) mqtt.subscribe(MQTT_BASE_TOPIC "/cmd"); mqtt.subscribe("domoticz/out"); publishDiscovery(); g_dirty = true; backoff = 5000; }
    else    { backoff = backoff < 300000 ? backoff * 2 : 300000; }
#endif
}

// ---------------------------------------------------------------------------
// OLED
// ---------------------------------------------------------------------------
static void oledTick() {
#if defined(BOARD_HELTEC_V2)
    if (g_oledPower.sync(oled, millis())) g_dirty = true;
#endif
}

#if defined(NO_OLED)
static void oledInit() {}
static void oledDraw() {}
#else
static void oledInit() {
    pinMode(PIN_VEXT, OUTPUT); digitalWrite(PIN_VEXT, LOW);
    pinMode(PIN_OLED_RST, OUTPUT);
    digitalWrite(PIN_OLED_RST, LOW); delay(20); digitalWrite(PIN_OLED_RST, HIGH);
    oled.begin();
    oled.setFont(u8g2_font_6x12_tf);
#if defined(BOARD_HELTEC_V2)
    pinMode(PIN_PRG_BTN, INPUT_PULLUP);   // read-only: never disturbs the GPIO0 boot/flashing function
    g_oledPower.setMode(g_oledPower.mode(), millis());
    oledTick();
#endif
}
static void oledDraw() {
    oledTick();
#if defined(BOARD_HELTEC_V2)
    if (!g_oledPower.awake()) return;  // no buffer transfers while the OLED is asleep
#endif
    const orcon::State& s = orcon::state();
    char l1[24], l2[24], l3[24], l4[24];
    snprintf(l1, sizeof(l1), "Orcon 15RF  %s", radio::name());
    if (s.selftest)
        snprintf(l2, sizeof(l2), "TST cmd:%s fan:%s",
                 orcon::modeName(s.cmdMode), orcon::modeName(s.mode));
    else
        snprintf(l2, sizeof(l2), "Mode: %s", orcon::modeName(s.mode));
    int fage = s.lastFanMs ? (int)((millis() - s.lastFanMs) / 1000) : -1;
    snprintf(l3, sizeof(l3), "fan %ds  CO2 %u", fage, s.co2);
    snprintf(l4, sizeof(l4), g_apMode ? "AP %s" : "%s", g_ip);
#if ORCON_TX_POC && !ORCON_WEB_CONTROL
    if (!g_pocAuthenticated && g_pocBootToken[0] && uint32_t(millis() - g_pocBootMs) < 60000) {
        // Physical bootstrap only: never put this credential in HTTP, serial or NVS.
        char first[17]; memcpy(first,g_pocBootToken,16); first[16]=0;
        oled.clearBuffer(); oled.drawStr(0,11,"DRY-RUN ONLY / NO RF");
        oled.drawStr(0,26,"Local boot key:"); oled.drawStr(0,41,first);
        oled.drawStr(0,56,g_pocBootToken+16); oled.sendBuffer(); return;
    }
#endif
    oled.clearBuffer();
    oled.drawStr(0, 11, l1); oled.drawStr(0, 26, l2);
    oled.drawStr(0, 41, l3); oled.drawStr(0, 56, l4);
    if (s.fault) oled.drawStr(104, 11, "!");
    oled.sendBuffer();
}
#endif

#if defined(BOARD_HELTEC_V2)
// Physical PRG button (GPIO0): a short press restarts the OLED viewing window
// (wake + fresh 30 s in Auto/Off; no visible change while On). The pin is only
// ever read, with a pull-up — GPIO0 keeps its plain boot-strapping role, so
// reset+hold still enters the ROM flasher exactly as on a stock board.
static void prgPoll(uint32_t now) {
    constexpr uint32_t DEBOUNCE_MS = 30;
    static bool     raw     = true;    // last pin level read
    static bool     stable  = true;    // debounced level; true = released
    static uint32_t changed = 0;       // when the raw level last changed
    const bool level = digitalRead(PIN_PRG_BTN);
    if (level != raw) { raw = level; changed = now; }
    else if (level != stable && uint32_t(now - changed) >= DEBOUNCE_MS) {
        stable = level;
        if (!stable) {                 // debounced press: exactly one wake per press
            g_oledPower.wake(now);
            oledDraw();                // lift power-save and redraw the status now
        }
    }
}

// Local display setting, separate from /api/cmd: the RX-only allowlist and all
// TX/OTA guards stay unchanged. Reject unknown keys before any NVS/state change.
// {"mode":...} persists the policy; {"toggle":1} flips the screen right now
// without touching the stored mode (same wake/sleep actions as the PRG button).
static void handleOledSave() {
    JsonDocument d;
    oledpower::Mode mode = oledpower::Mode::Auto;
    bool toggle = false;
    if (deserializeJson(d, server.arg("plain")) || !d.is<JsonObject>() || d.size() != 1) {
        server.send(400, "application/json", "{\"err\":\"expected mode: on, off or auto\"}"); return;
    }
    if (d["mode"].is<const char*>()) {
        if (!oledpower::parseMode(d["mode"].as<const char*>(), mode)) {
            server.send(400, "application/json", "{\"err\":\"expected mode: on, off or auto\"}"); return;
        }
    } else if (!(d["toggle"].is<int>() && d["toggle"].as<int>() == 1)) {
        server.send(400, "application/json", "{\"err\":\"expected mode: on, off or auto\"}"); return;
    } else {
        toggle = true;
    }
    if (toggle) {
        if (g_oledPower.awake()) g_oledPower.sleep();
        else                     g_oledPower.wake(millis());
    } else {
        if (mode != g_oledPower.mode()) {
            if (!prefs.begin("orcon", false)) {
                server.send(500, "application/json", "{\"err\":\"display setting not saved\"}"); return;
            }
            size_t written = prefs.putString("oled_mode", oledpower::modeName(mode));
            prefs.end();
            if (!written) {
                server.send(500, "application/json", "{\"err\":\"display setting not saved\"}"); return;
            }
        }
        g_oledPower.setMode(mode, millis());
    }
    oledDraw();  // apply power-save immediately; redraw current status on wake
    g_dirty = true;
    server.send(200, "application/json", "{\"ok\":1}");
}
#endif

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
static void wifiConnect() {
    g_apMode = (g_ssid.length() == 0);
    if (!g_apMode) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(g_ssid.c_str(), g_pass.c_str());
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { delay(250); oledDraw(); }
    }
    if (WiFi.status() == WL_CONNECTED) {
        g_apMode = false;
        snprintf(g_ip, sizeof(g_ip), "%s", WiFi.localIP().toString().c_str());
    } else {
        g_apMode = true;
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(WIFI_AP_NAME, WIFI_AP_PASS);
        snprintf(g_ip, sizeof(g_ip), "%s", WiFi.softAPIP().toString().c_str());
    }
}

// ---------------------------------------------------------------------------
#if ORCON_TX_POC && !ORCON_WEB_CONTROL
static void pocToken(char out[33]) {
    uint8_t bytes[16]; esp_fill_random(bytes,sizeof(bytes));
    static const char TOKEN_HEX[]="0123456789abcdef";
    for(unsigned i=0;i<16;++i) { out[2*i]=TOKEN_HEX[bytes[i]>>4]; out[2*i+1]=TOKEN_HEX[bytes[i]&15]; }
    out[32]=0;
}
static bool pocReady() {   // RX health only via the owner's atomic heartbeat
    return !ORCON_TX_RF_ENABLE && g_pocIdentityValid && orcon::state().fanKnown && txpoc::validIds(orcon::ids()) && txpoc::rxReady();
}
static bool pocAuthorize(bool mutation) {
    if(uint32_t(millis()-g_pocBootMs)>=600000) {
        server.send(401,"application/json","{\"error\":\"boot-session-expired\"}"); return false;
    }
    String host=server.header("Host"),local=WiFi.localIP().toString();
    String auth=server.header("Authorization"),csrf=server.header("X-CSRF-Token");
    String origin=server.header("Origin"),type=server.header("Content-Type");
    txpoc::Access a=txpoc::accessFrom(!g_apMode && WiFi.getMode()==WIFI_STA && WiFi.status()==WL_CONNECTED,
        host.c_str(),local.c_str(),origin.c_str(),type.c_str());
    const char* token=auth.startsWith("Bearer ")?auth.c_str()+7:"";
    txpoc::AccessResult result=g_pocSecurity.check(token,csrf.c_str(),a,mutation,millis());
    if(result!=txpoc::AccessResult::Allowed) {
        int code=result==txpoc::AccessResult::Limited?429:result==txpoc::AccessResult::Unauthorized?401:403;
        server.send(code,"application/json","{\"error\":\"access-denied\"}"); return false;
    }
    g_pocAuthenticated=true; return true;
}
static void pocStateBody(int code) {
    txpoc::controller().tick(millis());
    const txpoc::Status& s=txpoc::controller().status();
    JsonDocument d;
    d["firmware_mode"]=ORCON_TX_RF_ENABLE?"tx-poc-rf-disabled":"tx-poc-dryrun"; d["fw"]=FW_VERSION;
    d["dry_run"]=!ORCON_TX_RF_ENABLE; d["rf_capable"]=bool(ORCON_TX_RF_ENABLE); d["rf_armed"]=false;
    d["rf_sent"]=false; d["unit_confirmed"]=false;
    d["request"]=s.result.request.input.request; d["next_request"]=s.nextRequest;
    d["mode"]=s.result.request.input.mode; d["busy"]=s.busy; d["locked"]=s.locked;
    d["phase"]=txpoc::phaseName(s.phase); d["error"]=txpoc::errorName(s.result.error);
    d["rx_restored"]=s.result.rxRestored; d["bytes"]=(unsigned)s.result.bytes;
    char source[16],unit[16];
    const orcon::Ids& pinned=s.result.request.input.request?s.result.request.ids:orcon::ids();
    idStr(pinned.us,source,sizeof(source)); idStr(pinned.fan,unit,sizeof(unit));
    d["source_id"]=source; d["unit_id"]=unit;
    d["ready"]=pocReady(); d["csrf"]=g_pocSecurity.csrf();
    if(s.haveUnit) { d["unit_mode"]=s.unitMode; d["unit_age_s"]=(millis()-s.unitMs)/1000; }
    d["remote_seen"]=s.haveRemote;
    d["rx_check"]="dry-run observation; no radio mode switch";
    char body[1024];
    if(measureJson(d)>=sizeof(body)) { server.send(500,"application/json","{\"error\":\"state-too-large\"}"); return; }
    serializeJson(d,body,sizeof(body));
    server.sendHeader("Cache-Control","no-store"); server.sendHeader("X-Content-Type-Options","nosniff");
    server.send(code,"application/json",body);
}
static void handlePocState() { if(pocAuthorize(false)) pocStateBody(200); }
static void handlePocMode() {
    if(!pocAuthorize(true)) return;
#if ORCON_TX_RF_ENABLE
    // The old portal is explicitly dry-run. Never let it drive an RF backend.
    server.send(501,"application/json","{\"error\":\"rf-web-control-required\"}"); return;
#endif
    String body=server.arg("plain"); txpoc::Input input;
    if(!txpoc::parseInput(body.c_str(),body.length(),input)) {
        server.send(400,"application/json","{\"error\":\"expected-mode-1-2-3-and-request\"}"); return;
    }
    txpoc::controller().tick(millis());
    const rlog::Device* heard=nullptr; const size_t heardN=rlog::devices(&heard);
    const bool identityClear=!txpoc::identityConflict(orcon::ids(),rlog::seen(),heard,heardN);
    txpoc::Admission result=txpoc::controller().submit(input,orcon::ids(),
        g_pocIdentityValid && orcon::state().fanKnown,pocReady(),identityClear,millis());
    if(result==txpoc::Admission::Accepted || result==txpoc::Admission::Duplicate) {
        pocStateBody(result==txpoc::Admission::Accepted?202:200); return;
    }
    const char* error="invalid"; int code=400;
    switch(result) {
        case txpoc::Admission::UnknownUnit:error="unknown-or-invalid-identities"; code=403; break;
        case txpoc::Admission::Unavailable:error="radio-unavailable"; code=503; break;
        case txpoc::Admission::Busy:error="transaction-busy"; code=409; break;
        case txpoc::Admission::Limited:error="rate-limited"; code=429; break;
        case txpoc::Admission::Replay:error="request-sequence"; code=409; break;
        case txpoc::Admission::Conflict:error="idempotency-conflict"; code=409; break;
        case txpoc::Admission::IdentityConflict:error="source-id-heard-on-air"; code=403; break;
        case txpoc::Admission::IdentityUnverified:error="press-physical-remote-first"; code=409; break;
        case txpoc::Admission::UnitStateUnknown:error="unit-mode-unknown-or-stale"; code=409; break;
        case txpoc::Admission::Unchanged:error="unit-already-in-mode"; code=409; break;
        case txpoc::Admission::RemoteActive:error="remote-recently-active"; code=409; break;
        default:break;
    }
    char response[96]; snprintf(response,sizeof(response),"{\"error\":\"%s\"}",error);
    server.send(code,"application/json",response);
}
static void handlePocRoot() {
#if ORCON_TX_RF_ENABLE
    server.send(503,"text/plain","RF PoC portal disabled; use the explicit web-control firmware."); return;
#endif
    server.sendHeader("Cache-Control","no-store"); server.sendHeader("X-Frame-Options","DENY");
    server.sendHeader("Content-Security-Policy","default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
    server.send_P(200,"text/html",TX_POC_HTML);
}
#endif // ORCON_TX_POC && !ORCON_WEB_CONTROL
#if !ORCON_TX_POC || ORCON_WEB_CONTROL
class ProfileNvs : public profiles::Storage {
public:
    bool read(uint8_t* out,size_t cap,size_t& len) override {
        Preferences p;if(!p.begin("orconprofiles",false))return false;
        if(!p.isKey("data")){len=0;p.end();return true;}
        len=p.getBytesLength("data");
        const bool ok=len>0&&len<=cap&&p.getBytes("data",out,len)==len;p.end();return ok;
    }
    bool write(const uint8_t* in,size_t len) override {
        Preferences p;if(!p.begin("orconprofiles",false))return false;
        const bool ok=p.putBytes("data",in,len)==len;p.end();return ok;
    }
};
static ProfileNvs profileNvs;
#endif
void begin() {
    loadConfig();          // also applies stored device IDs via orcon::setIds
#if !ORCON_TX_POC || ORCON_WEB_CONTROL
    profiles::store().begin(profileNvs);
    setupweb::restoreProfileSelection();
#endif
    oledInit();
    wifiConnect();

#if !RX_ONLY   // RX-only: no network firmware update (would allow a TX-capable image)
    if (!g_apMode) {                    // OTA only makes sense on the home network
        ArduinoOTA.setHostname("orcon15rf");
        ArduinoOTA.begin();
    }
#endif

#if ORCON_TX_POC && !ORCON_WEB_CONTROL
    char csrf[33]; pocToken(g_pocBootToken); pocToken(csrf);
    if(!g_pocSecurity.begin(g_pocBootToken,csrf,millis())) g_pocBootToken[0]=0;
    memset(csrf,0,sizeof(csrf)); g_pocBootMs=millis();
    const char* headers[]={"Host","Origin","Authorization","X-CSRF-Token","Content-Type"};
    server.collectHeaders(headers,sizeof(headers)/sizeof(headers[0]));
    server.on("/",HTTP_GET,handlePocRoot);
    server.on("/api/tx-poc/state",HTTP_GET,handlePocState);
    server.on("/api/tx-poc/mode",HTTP_POST,handlePocMode);
    server.on("/api/oled",HTTP_POST,[](){ if(pocAuthorize(true)) handleOledSave(); });
    // No legacy command, ID, WiFi/config, log, setup or update routes in the PoC.
    server.onNotFound([](){ server.send(403,"application/json","{\"error\":\"route-disabled-in-poc\"}"); });
#elif ORCON_WEB_CONTROL
    // Wizard control + existing read-only RX monitoring. Legacy mutations stay absent.
    setupweb::begin(server);
    server.on("/log", HTTP_GET, handleLogPg);
    server.on("/api/log", HTTP_GET, handleLog);
    server.on("/api/state", HTTP_GET, handleState);
    server.onNotFound([](){ server.send(403,"application/json","{\"error\":\"route-disabled-in-web-control\"}"); });
#else
    setupweb::begin(server);            // "/" = setup wizard + /api/setup/*, /api/remote/*
    server.on("/dashboard", handleRoot);
    server.on("/wifi",      handleWifi);
    server.on("/log",       handleLogPg);
    server.on("/widget",    handleWidget);
    server.on("/widget.html", handleFlow);
    server.on("/flow",      handleFlow);
    server.on("/house.jpg", handleHouseImg);
    server.on("/debug",     handleDebug);
#if !RX_ONLY
    server.on("/update",    HTTP_GET,  handleUpdatePage);
    server.on("/update",    HTTP_POST, handleUpdateDone, handleUpdateUpload);
#endif
    server.on("/api/state", handleState);
    server.on("/api/netcfg", handleNetCfg);
    server.on("/api/cmd",   HTTP_POST, handleCmd);
    server.on("/api/log",   handleLog);
    server.on("/api/ids",   HTTP_POST, handleIds);
    server.on("/api/scan",  handleScan);
    server.on("/api/wifi",  HTTP_POST, handleWifiSave);
#if defined(BOARD_HELTEC_V2)
    server.on("/api/oled",  HTTP_POST, handleOledSave);
#endif
#endif // ORCON_TX_POC routes
#if ORCON_SIGNED_OTA
    ota::begin(server);                 // signed OTA + /api/diag (every Heltec V2 SX1276 build)
#endif
    server.begin();

#if MQTT_ENABLED
    if (mqttOn()) mqtt.setServer(g_mqttHost.c_str(), MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    mqtt.setBufferSize(3328);  // state JSON (<= 3072) + topic/header; also domoticz/out in
    mqtt.setSocketTimeout(2);     // cap a blocking connect() at 2s (don't starve RX)
#endif
    oledDraw();
}

void notifyStateChanged() { g_dirty = true; }

// ---- accessors for the on-screen System page -------------------------------
const char* ipAddr()       { return g_ip; }
const char* ssidName()     { return g_apMode ? WIFI_AP_NAME : g_ssid.c_str(); }
bool        apMode()       { return g_apMode; }
bool        wifiConnected(){ return !g_apMode && WiFi.status() == WL_CONNECTED; }
bool        maskIds()      { return g_maskIds; }
void        restartSoon()  { g_rebootAt = millis() + 1200; }
// Only SSID + password: profiles, MQTT, OLED and IDs stay. An empty SSID makes wifiConnect() start the setup AP.
void        forgetWifi()   { prefs.begin("orcon", false); prefs.remove("ssid"); prefs.remove("pass"); prefs.end(); restartSoon(); }
const char* mqttHost()     { return g_mqttHost.c_str(); }
bool        mqttConnected() {
#if MQTT_ENABLED
    return mqttOn() && mqtt.connected();
#else
    return false;
#endif
}

void publishState() {
    size_t n = buildStateJson(g_stateBuf, sizeof(g_stateBuf));
#if MQTT_ENABLED
    if (n && mqttOn() && mqtt.connected()) mqtt.publish(MQTT_BASE_TOPIC "/state", g_stateBuf, true);
#endif
    oledDraw();
    g_dirty = false;
    g_lastPub = millis();
}

void loop() {
#if ORCON_TX_POC && !ORCON_WEB_CONTROL
    g_pocSecurity.expire(millis());
#endif
    oledTick();  // runs independently of state publishing and RF activity
#if defined(BOARD_HELTEC_V2)
    prgPoll(millis());   // OLED wake button; a plain GPIO read, no RF/WiFi impact
#endif
    if (g_rebootAt && millis() > g_rebootAt) ESP.restart();
    if (orcon::idsDirty()) {                  // pairing learned a fan -> persist it
        const orcon::Ids& id = orcon::ids();
        saveIds(id.us.cls, id.us.serial, id.fan.cls, id.fan.serial);
        orcon::clearIdsDirty();
        g_dirty = true;
    }
#if ORCON_WEB_CONTROL
    // Web-control pairing completed: store the learned unit once (never rewrite an
    // existing, identical pairing), then let orcon track it like a configured unit.
    ramses::DeviceId learned;
    if (setupweb::takeLearnedFan(learned)) {
        const orcon::Ids& id = orcon::ids();
        saveIds(id.us.cls, id.us.serial, learned.cls, learned.serial);
        orcon::setIds(id.us.cls, id.us.serial, learned.cls, learned.serial);
        g_dirty = true;
    }
#endif
#if !RX_ONLY
    if (!g_apMode) ArduinoOTA.handle();
#endif
    server.handleClient();
#if MQTT_ENABLED
    if (mqttOn()) { mqttReconnect(); mqtt.loop(); }
#endif
    oledTick();
    if (g_dirty || millis() - g_lastPub > 5000) publishState();
}

} // namespace net
