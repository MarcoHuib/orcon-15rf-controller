// orcon.cpp — see orcon.h
#include "orcon.h"
#include "ramses_radio.h"
#include "orcon_log.h"
#include "config.h"
#include <Arduino.h>
#include <string.h>

namespace orcon {

static Ids   g_ids;
static State g_state;
static bool  g_fanSerialFixed = false;

static uint32_t g_lastCo2    = 0;
static uint32_t g_lastFilter = 0;
static uint32_t g_lastDemand = 0;
static uint32_t g_lastInfo   = 0;
static uint32_t g_lastStatus = 0;
static bool     g_bootReq    = false;

static bool     g_selftest = false;
static uint32_t g_stStart  = 0;
static uint32_t g_stLast   = 0;
static Mode     g_stMode   = Mode::Away;

static int16_t  g_fanSaved[7] = {-1,-1,-1,-1,-1,-1,-1};  // keeper: desired %/preset
static int16_t  g_bypSaved    = -1;                       // keeper: bypass C8/00/FF
static bool     g_autoRestore = false;
static uint32_t g_reapplyAt   = 0;
static uint32_t g_last042F    = 0;

static PairState g_pairState    = PairState::Idle;
static uint32_t  g_pairUntil    = 0;
static bool      g_idsDirty     = false;
static bool      g_passiveLearn = false;   // advanced: off by default

// RX-only: refuse before ANY state change (no pairing window, no commanded mode,
// no emulated-sensor values, no TX counters). Counted so the UI can show it.
#define RX_ONLY_REFUSE(ret) do { if (RX_ONLY) { g_state.txRefused++; return ret; } } while (0)

static void applyIds(uint8_t usClass, uint32_t usSerial,
                     uint8_t fanClass, uint32_t fanSerial) {
    g_ids.us = ramses::DeviceId(usClass, usSerial);
    if (fanSerial == 0) {
        g_ids.fan = ramses::DeviceId(DEV_BCAST_CLASS, DEV_BCAST_SERIAL);
        g_fanSerialFixed = false; g_state.fanKnown = false;
    } else {
        g_ids.fan = ramses::DeviceId(fanClass, fanSerial);
        g_fanSerialFixed = true;  g_state.fanKnown = true;
    }
}

void begin(uint8_t uc, uint32_t us, uint8_t fc, uint32_t fs) { applyIds(uc, us, fc, fs); }
void setIds(uint8_t uc, uint32_t us, uint8_t fc, uint32_t fs) { applyIds(uc, us, fc, fs); }

const State& state() { return g_state; }
const Ids&   ids()   { return g_ids; }

static void toHex(const uint8_t* p, uint8_t len, char* out, size_t cap) {
    size_t n = 0;
    for (uint8_t i = 0; i < len && n + 3 < cap; i++) n += snprintf(out + n, cap - n, "%02X", p[i]);
    out[n] = 0;
}
static const char* typeStr(ramses::MsgType t) {
    switch (t) {
        case ramses::MsgType::RQ: return "RQ";
        case ramses::MsgType::RP: return "RP";
        case ramses::MsgType::W:  return "W";
        default:                  return "I";
    }
}
static void fmtFrame(const ramses::Frame& f, char* out, size_t cap) {
    char hex[2 * ramses::MAX_PAYLOAD + 1]; toHex(f.payload, f.len, hex, sizeof(hex));
    snprintf(out, cap, "%s %04X %03u %s", typeStr(f.type), f.cmd, f.len, hex);
}

// Only a frame the radio actually sent is logged/counted. A refused/blocked TX
// leaves no trace but txRefused. Commands (not periodic sends) start the ACK
// window (lastCmdMs), and only once actually sent.
static bool tx(const ramses::Frame& f) {
    if (RX_ONLY || !radio::sendFrame(f)) { g_state.txRefused++; return false; }
    rlog::add('T', f, 0);
    g_state.txCount++;
    fmtFrame(f, g_state.lastTx, sizeof(g_state.lastTx));
    return true;
}
static bool txCmd(const ramses::Frame& f) {
    if (!tx(f)) return false;
    g_state.lastCmdMs = millis();
    return true;
}

static bool sendMode(Mode m) {
    bool ok = txCmd(make22F1(g_ids, m));
    if (ok) g_state.cmdMode = m;
    return ok;
}

bool setMode(Mode m) {
    RX_ONLY_REFUSE(false);
    if (m == Mode::Unknown) return false;
    bool ok = sendMode(m); delay(120); sendMode(m);
    return ok;
}
bool setTimer(uint16_t minutes) { RX_ONLY_REFUSE(false); return tx(make22F3(g_ids, minutes)); }

bool sendBind() {
    RX_ONLY_REFUSE(false);
    bool ok = tx(make1FC9_bind(g_ids));
    delay(60); sendMode(Mode::Auto);
    return ok;
}

// --- pairing ---------------------------------------------------------------
// Mimics a real 15RF's "Auto+1": open a 3-minute window and send the 1FC9 bind
// offer. The fan (put in pairing mode by power-cycling) registers us and then
// emits frames addressed from its own ID — onFrame()/learnFan() captures that
// ID *authoritatively* during the window (no passive sniffing of neighbours).
void startPairing() {
    RX_ONLY_REFUSE();
    g_pairState = PairState::Searching;
    g_pairUntil = millis() + 180000UL;
    tx(make1FC9_bind(g_ids));
}
PairState pairState() {
    if (g_pairState == PairState::Searching && (int32_t)(millis() - g_pairUntil) >= 0)
        g_pairState = PairState::Timeout;
    return g_pairState;
}
uint32_t pairWindowLeftMs() {
    if (g_pairState != PairState::Searching) return 0;
    int32_t left = (int32_t)(g_pairUntil - millis());
    return left > 0 ? (uint32_t)left : 0;
}
bool idsDirty()              { return g_idsDirty; }
void clearIdsDirty()         { g_idsDirty = false; }
void setPassiveLearn(bool on){ g_passiveLearn = on && !RX_ONLY; }
bool passiveLearn()          { return g_passiveLearn; }

void setSelftest(bool on) {
    if (on) RX_ONLY_REFUSE();
    g_selftest = on; g_state.selftest = on;
    if (on) { g_stStart = millis(); g_stLast = 0; g_stMode = Mode::Away; }
}

// RQ 0001 to the unit (the display's power-on RF-check). If the unit answers
// RP 0001, our transmit is confirmed working.
void sendConnect() {
    RX_ONLY_REFUSE();
    txCmd(makeRequest(g_ids, 0x0001));
}

// Arbitrary frame us -> fan (2-address). type: 0=I 1=RQ 2=RP 3=W.
void sendRaw(uint8_t type, uint16_t code, const uint8_t* pl, uint8_t len) {
    RX_ONLY_REFUSE();
    ramses::Frame f;
    f.type = (ramses::MsgType)type;
    f.has_addr[0] = true;  f.addr[0] = g_ids.us;
    f.has_addr[1] = true;  f.addr[1] = g_ids.fan;
    f.has_addr[2] = false;
    f.cmd = code;
    if (len > ramses::MAX_PAYLOAD) len = ramses::MAX_PAYLOAD;
    f.len = len;
    for (uint8_t i = 0; i < len; i++) f.payload[i] = pl[i];
    txCmd(f);
}

bool sendDemand(uint8_t percent) {
    RX_ONLY_REFUSE(false);
    g_state.demand = percent > 100 ? 100 : percent;
    return tx(make31E0(g_ids, percent));
}
bool sendCo2(uint16_t ppm)  { RX_ONLY_REFUSE(false); g_state.co2 = ppm; return tx(make1298(g_ids, ppm)); }
bool sendDeviceInfo()       { RX_ONLY_REFUSE(false); return tx(make10E0(g_ids)); }
bool requestStatus()        { RX_ONLY_REFUSE(false); return txCmd(makeRequest(g_ids, 0x31DA)); }
bool requestFilter()        { RX_ONLY_REFUSE(false); return tx(makeRequest(g_ids, 0x10D0)); }   // ask the fan for filter status
bool setBypass(uint8_t val) { RX_ONLY_REFUSE(false); return tx(make22F7(g_ids, val)); }
bool setFanSpeed(uint8_t p, uint8_t pct) { RX_ONLY_REFUSE(false); return tx(makeFanSpeed(g_ids, p, pct)); }

void setFanSaved(uint8_t p, int16_t pct) { if (p < 7) g_fanSaved[p] = (pct > 100 ? 100 : pct); }
int16_t fanSaved(uint8_t p) { return p < 7 ? g_fanSaved[p] : -1; }
void setBypassSaved(int16_t v) { g_bypSaved = v; }
int16_t bypassSaved() { return g_bypSaved; }
void setAutoRestore(bool on) { g_autoRestore = on && !RX_ONLY; g_state.autoRestore = g_autoRestore; }
bool autoRestore() { return g_autoRestore; }
uint8_t reapplySaved() {
    RX_ONLY_REFUSE(0);
    uint8_t c = 0;
    for (uint8_t p = 0; p < 7; p++)
        if (g_fanSaved[p] >= 0) { setFanSpeed(p, (uint8_t)g_fanSaved[p]); delay(150); c++; }
    if (g_bypSaved >= 0) { setBypass((uint8_t)g_bypSaved); c++; }
    return c;
}

// One-tap AC mode: reconfigures the HIGH preset (params 7/8 = idx 4/5) for
// unbalanced ventilation, opens the bypass, and switches to High.
//  which: 0 = balanced 50/50 (restore), 1 = exhaust-only (suck out),
//         2 = supply-only (blow in). Values are remembered (keeper).
void setAcMode(uint8_t which) {
    RX_ONLY_REFUSE();
    uint8_t hiSup, hiExh, byp;
    if (which == 1)      { hiSup = 0;   hiExh = 100; byp = 0xC8; }   // exhaust, bypass open
    else if (which == 2) { hiSup = 100; hiExh = 0;   byp = 0xC8; }   // supply, bypass open
    else                 { hiSup = 50;  hiExh = 50;  byp = 0xFF; }   // balanced, bypass auto
    setFanSpeed(4, hiSup); setFanSaved(4, hiSup); delay(150);        // High supply  (param 7)
    setFanSpeed(5, hiExh); setFanSaved(5, hiExh); delay(150);        // High exhaust (param 8)
    setBypass(byp);        setBypassSaved((int16_t)byp); delay(150);
    setMode(which == 0 ? Mode::Auto : Mode::High);
}

// Custom unbalanced ventilation: set the High preset to exact supply/exhaust %
// (e.g. a quiet low imbalance for night cooling), optionally open the bypass,
// then switch to High. Remembered across power loss.
void setAcCustom(uint8_t sup, uint8_t exh, bool openByp) {
    RX_ONLY_REFUSE();
    if (sup > 100) sup = 100;
    if (exh > 100) exh = 100;
    setFanSpeed(4, sup); setFanSaved(4, sup); delay(150);   // High supply  (param 7)
    setFanSpeed(5, exh); setFanSaved(5, exh); delay(150);   // High exhaust (param 8)
    if (openByp) { setBypass(0xC8); setBypassSaved(0xC8); delay(150); }
    setMode(Mode::High);
}

void tick(uint32_t now) {
    if (RX_ONLY) return;                     // periodic TX only
    if (g_reapplyAt && now >= g_reapplyAt) { g_reapplyAt = 0; reapplySaved(); }
    if (g_selftest) {
        if (now - g_stLast >= 8000) {
            g_stLast = now;
            g_stMode = (g_stMode == Mode::High) ? Mode::Away : Mode::High;
            for (int i = 0; i < 3; i++) { sendMode(g_stMode); delay(150); }
            if (now - g_stStart > 120000) setSelftest(false);
        }
        return;
    }
    if (!g_bootReq && now > 8000)                      { g_bootReq = true; requestStatus(); requestFilter(); }  // populate after reboot
    if (now - g_lastStatus >= SEND_STATUS_INTERVAL_MS) { g_lastStatus = now; requestStatus(); }  // keep live values fresh
    if (now - g_lastFilter >= 600000UL)                { g_lastFilter = now; requestFilter(); }  // poll 10D0 filter status every 10 min
    if (now - g_lastDemand >= SEND_DEMAND_INTERVAL_MS) { g_lastDemand = now; sendDemand(g_state.demand); }
    if (now - g_lastCo2 >= SEND_CO2_INTERVAL_MS)       { g_lastCo2 = now; if (g_state.co2) sendCo2(g_state.co2); }
    if (now - g_lastInfo >= SEND_DEVINFO_INTERVAL_MS)  { g_lastInfo = now; sendDeviceInfo(); }
}

static float temp16(const uint8_t* p) {
    int16_t v = (int16_t)((p[0] << 8) | p[1]);
    if (v == (int16_t)0x7FFF || v == (int16_t)0x8000) return 0;
    return v / 100.0f;
}
static void learnFan(const ramses::DeviceId& d) {
    if (d.cls != DEV_FAN_CLASS || d.serial == 0) return;
    if (g_pairState == PairState::Searching) {        // authoritative bind during pairing
        g_ids.fan = d; g_state.fanKnown = true; g_fanSerialFixed = true;
        g_pairState = PairState::Paired; g_idsDirty = true;   // net persists ids() to NVS
        return;
    }
    if (g_passiveLearn && !g_fanSerialFixed) {        // advanced fallback only (default off)
        g_ids.fan = d; g_state.fanKnown = true;
    }
}

// The selected unit = the configured fan ID (NVS / pairing). Without one, TX builds
// keep the legacy class-32 match; RX-only shows no unit status until one is chosen.
static bool isUnit(const ramses::DeviceId& d) {
    return g_state.fanKnown ? d == g_ids.fan : (!RX_ONLY && d.cls == DEV_FAN_CLASS);
}
static bool toUnit(const ramses::Frame& f) {
    return (f.has_addr[1] && isUnit(f.addr[1])) ||
           (f.has_addr[2] && !(f.addr[2] == f.addr[0]) && isUnit(f.addr[2]));
}

bool onFrame(const ramses::Frame& f) {
    const uint32_t now = millis();
    rlog::add('R', f, (int)radio::lastRssi());
    g_state.rxCount++;
    fmtFrame(f, g_state.lastRx, sizeof(g_state.lastRx));
    g_state.lastRxMs = now;

    for (int i = 0; i < 3; i++) {                     // pairing (TX builds; RX-only never pairs)
        if (!f.has_addr[i]) continue;
        if (f.addr[i].cls == DEV_FAN_CLASS) learnFan(f.addr[i]);
    }
    if (!f.has_addr[0]) return false;
    const uint8_t* p = f.payload;

    // A remote's mode command to our unit: recorded apart from the unit's own state.
    if (f.cmd == 0x22F1 && f.type != ramses::MsgType::RP && f.len >= 2 && p[1] <= 7 && toUnit(f)) {
        g_state.remoteMode = (Mode)p[1];
        g_state.remoteId   = f.addr[0];
        g_state.remoteMs   = now;
        g_state.remoteConfirmedMs = 0;
        return true;
    }
    // Unit status only from the selected unit itself: neighbours share the channel.
    const bool status = f.type == ramses::MsgType::I || f.type == ramses::MsgType::RP;
    if (!status || !isUnit(f.addr[0])) return false;
    g_state.lastFanMs = now;

    // The unit answered us: an RP / 31D9 / 0001 shortly after one of OUR transmissions.
    if (g_state.lastCmdMs && (now - g_state.lastCmdMs < 6000) &&
        (f.type == ramses::MsgType::RP || f.cmd == 0x31D9 || f.cmd == 0x0001)) {
        g_state.txAckMs = now;
    }

    bool changed = false;
    switch (f.cmd) {
        case 0x042F:
            if (g_autoRestore && (millis() - g_last042F > 60000)) {
                g_last042F = millis();
                g_reapplyAt = millis() + 20000;   // re-push saved settings ~20s after the fan reboots
            }
            changed = true; break;
        case 0x31D9:
            if (f.len >= 3 && p[2] <= 7) {
                g_state.mode = (Mode)p[2]; g_state.modeMs = now; changed = true;
                if (g_state.remoteMs && now - g_state.remoteMs < 10000 && g_state.mode == g_state.remoteMode)
                    g_state.remoteConfirmedMs = now;
            }
            break;
        case 0x31DA:
            // Calibrated against a real Orcon HRC400 frame (ramses_rf layout):
            //  [3-4] CO2 (7FFF=none)  [5] indoor RH%  [6] outdoor RH%
            //  [7-8] exhaust T  [9-10] supply T  [11-12] indoor T  [13-14] outdoor T
            //  [17] bypass pos (00=closed)  [19] exhaust fan  [20] supply fan
            toHex(f.payload, f.len, g_state.raw31DA, sizeof(g_state.raw31DA));
            g_state.raw31DAms = millis();
            if (f.len >= 5)  { uint16_t c = (uint16_t)((p[3] << 8) | p[4]); if (c != 0x7FFF) g_state.co2 = c; }
            if (f.len >= 6)  { if (p[5] <= 100) g_state.humidity = p[5]; }
            if (f.len >= 9)  g_state.tempExhaust = temp16(&p[7]);
            if (f.len >= 11) g_state.tempSupply  = temp16(&p[9]);
            if (f.len >= 13) g_state.tempIndoor  = temp16(&p[11]);
            if (f.len >= 15) g_state.tempOutdoor = temp16(&p[13]);
            if (f.len >= 18) g_state.bypassOpen  = (p[17] != 0x00 && p[17] != 0xEF && p[17] != 0xFF);
            if (f.len >= 20) g_state.exhaustPct  = (p[19] <= 200) ? p[19] / 2 : g_state.exhaustPct;
            if (f.len >= 21) g_state.supplyPct   = (p[20] <= 200) ? p[20] / 2 : g_state.supplyPct;
            changed = true; break;
        case 0x31E0: if (f.len >= 7) { g_state.demand = p[6] > 100 ? 100 : p[6]; changed = true; } break;
        case 0x12A0:
            if (f.len >= 2) g_state.humidity   = p[1];
            if (f.len >= 4) g_state.tempIndoor = temp16(&p[2]);
            changed = true; break;
        case 0x1298: if (f.len >= 3) { g_state.co2 = (uint16_t)((p[1] << 8) | p[2]); changed = true; } break;
        case 0x10D0: if (f.len >= 2) { g_state.filterRemaining = p[1]; changed = true; } break;
        default: break;
    }
    return changed;
}

} // namespace orcon
