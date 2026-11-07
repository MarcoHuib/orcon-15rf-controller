// test_orcon.cpp — orcon state model: RX-only blocking, unit selection, V3 TX.
// RX-only (Heltec V2): fake radio refuses TX, like the SX1276 backend.
//   g++ -std=c++17 -DRX_ONLY=1 -I src -I test/shim test/test_orcon.cpp src/orcon.cpp \
//       src/orcon_frames.cpp src/orcon_log.cpp src/ramses_codec.cpp -o /tmp/t && /tmp/t
// V3 (TX build): same with -DRX_ONLY=0; the fake radio then accepts TX.
#include "orcon.h"
#include "orcon_log.h"
#include "ramses_radio.h"
#include "rx_only.h"
#include <cstdio>
#include <cstring>
#include <string>
using namespace ramses; using orcon::Mode;

unsigned long g_ms = 1000;
unsigned long millis() { return g_ms; }
void delay(unsigned long ms) { g_ms += ms; }
static int g_sends = 0;
namespace radio {
bool sendFrame(const Frame&) { ++g_sends; return !RX_ONLY; }
float lastRssi() { return -60; }
}

static int fails = 0;
#define CHECK(c,m) do{ if(!(c)){printf("  [FAIL] %s\n",m);fails++;} else printf("  [ ok ] %s\n",m);}while(0)

static const DeviceId UNIT(0x1D, 202), REMOTE(0x1D, 101), REMOTE2(0x1D, 111111),
                      NB_FAN(20, 303), NB_FAN2(37, 404), HRC(0x20, 0x02BCDE);
static Frame fr(MsgType t, DeviceId a0, bool has1, DeviceId a1, bool has2, DeviceId a2,
                uint16_t cmd, std::initializer_list<uint8_t> pl) {
    Frame f; f.type = t;
    f.has_addr[0] = true; f.addr[0] = a0;
    f.has_addr[1] = has1; if (has1) f.addr[1] = a1;
    f.has_addr[2] = has2; if (has2) f.addr[2] = a2;
    f.cmd = cmd; f.len = 0; for (uint8_t b : pl) f.payload[f.len++] = b;
    return f;
}
static Frame remote22F1(DeviceId from, DeviceId to, uint8_t n) { return fr(MsgType::I, from, true, to, false, {}, 0x22F1, {0x00, n, 0x04}); }
static Frame unit31D9(DeviceId from, uint8_t n) { return fr(MsgType::I, from, false, {}, true, from, 0x31D9, {0x00, 0x00, n}); }
static Frame nb31DA(DeviceId from) {   // neighbour HRU status as heard on 2026-11-07 (RH 47%)
    return fr(MsgType::I, from, false, {}, true, from, 0x31DA,
              {0x00,0xEF,0x00,0x7F,0xFF,0x2F,0xEF,0x7F,0xFF,0x7F,0xFF,0x7F,0xFF,0x7F,0xFF,0xF8,0x00,
               0xEF,0x18,0x3D,0x00,0x00,0x00,0xEF,0xEF,0x7F,0xFF,0x7F,0xFF,0x00});
}
static bool logHasTx() { static char j[8192]; rlog::toJson(j, sizeof(j)); return strstr(j, "\"d\":\"T\"") != nullptr; }

static void rxOnlyBlocksEverything() {
    printf("\n== RX-only: every TX/pairing entry point is blocked without side effects ==\n");
    orcon::begin(0x25, 4242, UNIT.cls, UNIT.serial);
    const orcon::Ids before = orcon::ids();
    uint8_t raw[] = {0x00};
    orcon::setMode(Mode::High); orcon::setTimer(30); orcon::sendBind(); orcon::startPairing();
    orcon::sendConnect(); orcon::sendRaw(0, 0x22F1, raw, 1); orcon::sendDemand(80); orcon::sendCo2(900);
    orcon::sendDeviceInfo(); orcon::requestStatus(); orcon::requestFilter(); orcon::setBypass(0xC8);
    orcon::setFanSpeed(4, 70); orcon::setAcMode(1); orcon::setAcCustom(10, 90, true);
    orcon::setFanSaved(0, 40); orcon::reapplySaved(); orcon::setSelftest(true); orcon::tick(20000);
    orcon::setPassiveLearn(true); orcon::setAutoRestore(true);
    const orcon::State& s = orcon::state();
    char m[96]; snprintf(m, sizeof(m), "radio::sendFrame never called (calls=%d)", g_sends);
    CHECK(g_sends == 0, m);
    CHECK(s.txCount == 0 && s.lastTx[0] == 0 && !logHasTx(), "no TX counted, no lastTx, no 'T' log entry");
    CHECK(s.cmdMode == Mode::Unknown && s.lastCmdMs == 0, "no commanded mode recorded");
    CHECK(s.demand == 0 && s.co2 == 0, "emulated sensor values unchanged");
    CHECK(!s.selftest && !orcon::passiveLearn() && !orcon::autoRestore(), "selftest/passive-learn/auto-restore stay off");
    CHECK(orcon::pairState() == orcon::PairState::Idle && !orcon::idsDirty(), "pairing never opens, no NVS-dirty flag");
    CHECK(orcon::ids().fan == before.fan && orcon::ids().us == before.us, "device IDs unchanged");

    // Even within a "pairing window", a fan-class neighbour must not be adopted.
    orcon::startPairing();
    orcon::onFrame(nb31DA(HRC));
    CHECK(orcon::ids().fan == before.fan && !orcon::idsDirty(), "pairing + class-32 neighbour frame: IDs untouched");

    // Remote press + unit reply after a blocked command must not look like an ACK of ours.
    orcon::setMode(Mode::High);
    orcon::onFrame(remote22F1(REMOTE, UNIT, 3)); g_ms += 40; orcon::onFrame(unit31D9(UNIT, 3));
    CHECK(orcon::state().txAckMs == 0, "unit reply to the physical remote is not a TX-ACK");
}

static void apiAllowlist() {
    printf("\n== RX-only API/MQTT allowlist ==\n");
    // Every key applyCommand() handles (net.cpp); CI checks this list against the source.
    static const char* const TX[] = { "mode", "timer", "pair", "plearn", "selftest", "connect", "status",
        "bypass", "ac_mode", "ac_sup", "ac_exh", "ac_byp", "autorestore", "reapply", "forget",
        "raw_code", "raw_type", "raw_pl", "unknown_key" };
    bool allRefused = true;
    for (const char* k : TX) if (rxOnlyAllowedCmd(k)) { printf("    accepted: %s\n", k); allRefused = false; }
    CHECK(allRefused, "every TX / pairing / keeper / raw key is refused");
    CHECK(rxOnlyAllowedCmd("maskids") && rxOnlyAllowedCmd("showfilter") && rxOnlyAllowedCmd("roomidx"),
          "UI-only settings (maskids, showfilter, roomidx) stay allowed");
}

static void unitSelection() {
    printf("\n== selected unit vs neighbours and remotes ==\n");
    orcon::begin(0x25, 4242, UNIT.cls, UNIT.serial);   // explicitly selected: 29:000202
    g_ms += 60000;
    orcon::onFrame(nb31DA(NB_FAN)); orcon::onFrame(nb31DA(NB_FAN2));
    orcon::onFrame(fr(MsgType::I, NB_FAN, false, {}, true, NB_FAN, 0x31D9,
                      {0x00,0x06,0x02,0x00,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x00}));
    const orcon::State& s = orcon::state();
    CHECK(s.humidity == 0 && s.exhaustPct == 0, "neighbour 31DA does not overwrite unit status");
    CHECK(s.mode == Mode::Unknown, "neighbour 31D9 does not set the unit mode");
    CHECK(orcon::ids().fan == UNIT, "neighbour traffic never replaces the selected unit");

    orcon::onFrame(remote22F1(REMOTE2, NB_FAN, 3));                        // other house's remote
    orcon::onFrame(remote22F1(REMOTE, UNIT, 3));                           // our remote, no unit reply
    CHECK(s.mode == Mode::Unknown, "remote command without unit reply is not shown as unit mode");
    CHECK(s.remoteMode == Mode::High && s.remoteId == REMOTE && s.remoteConfirmedMs == 0,
          "remote command recorded separately, unconfirmed");
    g_ms += 40; orcon::onFrame(unit31D9(UNIT, 3));
    CHECK(s.mode == Mode::High, "unit 31D9 sets the mode");
    CHECK(s.remoteConfirmedMs != 0 && s.modeMs == g_ms, "remote command confirmed by the unit's 31D9");
    g_ms += 30000; orcon::onFrame(remote22F1(REMOTE, UNIT, 1)); g_ms += 15000; orcon::onFrame(unit31D9(UNIT, 3));
    CHECK(s.remoteMode == Mode::Low && s.remoteConfirmedMs == 0 && s.mode == Mode::High,
          "unit reporting another mode does not confirm the remote command");
    CHECK(s.lastFanMs != 0, "unit last-heard time follows the selected unit");
}

static void v3TxStillWorks() {
    printf("\n== V3 / TX build: commands and pairing unchanged ==\n");
    orcon::begin(0x25, 4242, 0x20, 0);             // fan unknown: broadcast until paired
    g_sends = 0;
    orcon::setMode(Mode::High);
    CHECK(g_sends == 2 && orcon::state().txCount == 2 && orcon::state().cmdMode == Mode::High,
          "setMode transmits twice and records the commanded mode");
    orcon::onFrame(unit31D9(HRC, 3));
    CHECK(orcon::state().txAckMs != 0, "fan reply after our TX counts as ACK");
    orcon::startPairing();
    CHECK(orcon::pairState() == orcon::PairState::Searching, "pairing window opens");
    orcon::onFrame(nb31DA(HRC));
    CHECK(orcon::pairState() == orcon::PairState::Paired && orcon::ids().fan == HRC && orcon::idsDirty(),
          "pairing learns the HRC400 (class 32) and marks IDs for NVS");
    int n = g_sends; orcon::setBypass(0xC8); uint8_t raw[] = {0x01}; orcon::sendRaw(1, 0x31DA, raw, 1);
    CHECK(g_sends == n + 2, "bypass and raw frames are transmitted");
}

int main() {
    printf("orcon state tests (RX_ONLY=%d)\n", RX_ONLY);
#if RX_ONLY
    unitSelection();              // first: State is static, later groups set the mode
    rxOnlyBlocksEverything();
    apiAllowlist();
#else
    v3TxStillWorks();
#endif
    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails ? 1 : 0;
}
