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
#include <map>
#include <string>
using namespace ramses; using orcon::Mode;

unsigned long g_ms = 1000;
unsigned long millis() { return g_ms; }
void delay(unsigned long ms) { g_ms += ms; }
static int g_sends = 0;
static std::map<uint16_t, int> g_cmd;    // sendFrame calls per message code
namespace radio {
bool sendFrame(const Frame& f) { ++g_sends; ++g_cmd[f.cmd]; return !RX_ONLY; }
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
    // ACK window: started by commands only, as before (not by periodic sensor sends)
    g_ms += 60000; uint32_t before = orcon::state().lastCmdMs;
    orcon::sendDemand(40); orcon::sendCo2(700);
    CHECK(orcon::state().lastCmdMs == before, "periodic 31E0/1298 sends do not open the ACK window");
    orcon::requestStatus();
    CHECK(orcon::state().lastCmdMs == g_ms, "a command (RQ 31DA) opens the ACK window");
}

// 25 simulated hours of orcon::tick() at 1 s steps.
static void run25h() { for (uint32_t s = 0; s < 25u * 3600u; s++) { g_ms += 1000; orcon::tick(g_ms); } }
static bool near(int got, int want, int tol) { return got >= want - tol && got <= want + tol; }

static void v3PeriodicTick() {
    printf("\n== V3 / TX build: orcon::tick() periodic sends (25 h simulated) ==\n");
    orcon::sendCo2(700);                      // CO2 value set -> 1298 is repeated
    g_cmd.clear();
    run25h();
    char m[160];
    snprintf(m, sizeof(m), "RQ 31DA every 60 s: %d (~1500), RQ 10D0 every 10 min: %d (~150)", g_cmd[0x31DA], g_cmd[0x10D0]);
    CHECK(near(g_cmd[0x31DA], 1500, 3) && near(g_cmd[0x10D0], 150, 3), m);
    snprintf(m, sizeof(m), "31E0 every 5 min: %d (~300), 1298 every 10 min: %d (~150), 10E0 daily: %d (1)",
             g_cmd[0x31E0], g_cmd[0x1298], g_cmd[0x10E0]);
    CHECK(near(g_cmd[0x31E0], 300, 3) && near(g_cmd[0x1298], 150, 3) && near(g_cmd[0x10E0], 1, 1), m);
    CHECK(g_cmd[0x22F1] == 0 && g_cmd[0x2411] == 0, "no mode/fan-speed frames without a selftest or keeper action");

    // keeper: unit announces 042F (power-cycled) -> saved fan speed re-pushed ~20 s later
    orcon::setAutoRestore(true); orcon::setFanSaved(4, 70); g_cmd.clear();
    orcon::onFrame(fr(MsgType::I, HRC, false, {}, true, HRC, 0x042F, {0x00}));
    for (int s = 0; s < 19; s++) { g_ms += 1000; orcon::tick(g_ms); }
    int early = g_cmd[0x2411];
    for (int s = 0; s < 3; s++) { g_ms += 1000; orcon::tick(g_ms); }
    snprintf(m, sizeof(m), "auto-restore after 042F: %d fan-speed frame(s) at ~20 s, none before", g_cmd[0x2411]);
    CHECK(early == 0 && g_cmd[0x2411] == 1, m);
    orcon::setAutoRestore(false);

    // selftest: 3x mode every 8 s, switches itself off after 2 min
    orcon::setSelftest(true); g_cmd.clear();
    for (int s = 0; s < 140; s++) { g_ms += 1000; orcon::tick(g_ms); }
    snprintf(m, sizeof(m), "selftest sent %d mode frames in 140 s (3 per 8 s) and stopped", g_cmd[0x22F1]);
    CHECK(near(g_cmd[0x22F1], 48, 6) && !orcon::state().selftest, m);
}

static void rxOnlyTickSilent() {
    printf("\n== RX-only: orcon::tick() never transmits (25 h simulated) ==\n");
    orcon::setAutoRestore(true); orcon::setFanSaved(4, 70); orcon::setSelftest(true);
    orcon::onFrame(fr(MsgType::I, UNIT, false, {}, true, UNIT, 0x042F, {0x00}));
    const uint32_t txBefore = orcon::state().txCount;
    g_sends = 0; g_cmd.clear();
    run25h();
    char m[96]; snprintf(m, sizeof(m), "25 h of tick(): %d sendFrame calls, txCount unchanged", g_sends);
    CHECK(g_sends == 0 && orcon::state().txCount == txBefore && !orcon::state().selftest && !orcon::autoRestore(), m);
}

int main() {
    printf("orcon state tests (RX_ONLY=%d)\n", RX_ONLY);
#if RX_ONLY
    unitSelection();              // first: State is static, later groups set the mode
    rxOnlyBlocksEverything();
    apiAllowlist();
    rxOnlyTickSilent();
#else
    v3TxStillWorks();
    v3PeriodicTick();
#endif
    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails ? 1 : 0;
}
