// test_setup.cpp — setup wizard session logic (setup_flow) with synthetic RF frames.
// IDs are pseudonymised (NN:000NNN). Covers: correct packet, wrong button, other device,
// duplicate, packets from before a step, unknown command, timeout, cancel, RF payload
// variants, pairing confirmation rules, virtual-remote confirmation and request guards.
#include "setup_flow.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>
using namespace ramses; using namespace setupflow;
static int failures = 0;
#define CHECK(c,m) do{ if(!(c)){printf("  [FAIL] %s (line %d)\n",m,__LINE__);failures++;} else printf("  [ ok ] %s\n",m);}while(0)

static const DeviceId REM(29, 101), REM2(29, 303), UNIT(29, 202), US(37, 404);

static Frame fr(MsgType t, DeviceId src, DeviceId dst, uint16_t cmd, std::initializer_list<uint8_t> pl) {
    Frame f; f.type = t; f.addr[0] = src; f.addr[1] = dst; f.has_addr[2] = false; f.cmd = cmd;
    f.len = uint8_t(pl.size()); uint8_t i = 0; for (uint8_t b : pl) f.payload[i++] = b;
    return f;
}
static Frame mode(DeviceId src, uint8_t m, uint8_t tail = 0x04) { return fr(MsgType::I, src, UNIT, 0x22F1, {0x00, m, tail}); }
static Frame timer(DeviceId src) { return fr(MsgType::I, src, UNIT, 0x22F3, {0x00, 0x12, 0x0F, 0x03, 0x04, 0x04, 0x04}); }
// The six buttons in order, as the physical remote would send them.
static Frame press(uint8_t k, DeviceId src = REM) { return k == K_TIMER ? timer(src) : mode(src, modeOfKey(k)); }

int main() {
    printf("Setup wizard logic tests\n");

    // ---- frame -> button -----------------------------------------------------------
    CHECK(keyOfFrame(mode(REM, 0x00)) == K_AWAY && keyOfFrame(mode(REM, 0x04)) == K_AUTO, "22F1 00/04 -> away/auto");
    CHECK(keyOfFrame(mode(REM, 1)) == K_1 && keyOfFrame(mode(REM, 2)) == K_2 && keyOfFrame(mode(REM, 3)) == K_3, "22F1 01/02/03 -> 1/2/3");
    CHECK(keyOfFrame(mode(REM, 2, 0x07)) == K_2, "HRC400 tail 07 and MVS tail 04 both decode (mode byte only)");
    CHECK(keyOfFrame(timer(REM)) == K_TIMER, "22F3 -> timer");
    CHECK(keyOfFrame(fr(MsgType::I, REM, UNIT, 0x22F3, {0x00, 0x12, 0x3C})) == K_TIMER, "22F3 with another payload/length -> timer");
    CHECK(keyOfFrame(mode(REM, 5)) == K_NONE && keyOfFrame(mode(REM, 6)) == K_NONE && keyOfFrame(mode(REM, 7)) == K_NONE,
          "22F1 auto2/boost/disable are no button of ours");
    CHECK(keyOfFrame(fr(MsgType::RQ, REM, UNIT, 0x22F1, {0x00, 0x03, 0x04})) == K_NONE, "RQ 22F1 is not a press");
    CHECK(keyOfFrame(fr(MsgType::I, REM, UNIT, 0x22F1, {0x00})) == K_NONE, "short 22F1 rejected");
    CHECK(keyOfFrame(fr(MsgType::I, UNIT, UNIT, 0x31D9, {0x00, 0x00, 0x03})) == K_NONE, "unit status 31D9 is not a press");
    CHECK(keyFromName("timer") == K_TIMER && keyFromName("3") == K_3 && keyFromName("4") == K_NONE && keyFromName(nullptr) == K_NONE,
          "key names");

    // ---- full session ----------------------------------------------------------------
    {
        Read r; uint32_t t = 1000, seq = 50, ev = 0;
        r.start(7, REM, t, seq);
        CHECK(r.st == St::Active && r.step == 0 && r.sid == 7, "session starts at step 0");
        for (uint8_t k = 0; k < K_N; k++) {
            t += 2000; seq += 3;
            Why w = r.onFrame(press(k), ++ev, seq, t, int16_t(-60 - k));
            CHECK(w == Why::Accepted, KEY_NAME[k]);
            // the remote repeats each press: identical packet right after must not count again
            Why d = r.onFrame(press(k), ++ev, seq + 1, t + 40, -60);
            CHECK(d == Why::Duplicate || r.st == St::Done, "repeat of the same press is a duplicate");
        }
        CHECK(r.st == St::Done && r.step == K_N, "all six -> done");
        CHECK(r.got[K_TIMER].cmd == 0x22F3 && r.got[K_3].pl[1] == 0x03 && r.got[K_2].rssi == -64, "per-step command/payload/rssi kept");
        CHECK(r.got[0].ev < r.got[1].ev && r.got[4].ev < r.got[5].ev, "event ids are monotonic per step");
        CHECK(r.onFrame(press(K_AWAY), ++ev, seq + 10, t + 5000, -60) == Why::None, "no changes after done");
    }

    // ---- wrong button, other devices, unknown, non-I ---------------------------------------
    {
        Read r; r.start(1, REM, 0, 10);
        CHECK(r.onFrame(mode(REM, 3), 1, 11, 1000, -60) == Why::WrongKey && r.step == 0, "wrong button does not advance");
        CHECK(r.whyMode == 3 && r.whyCmd == 0x22F1, "wrong button reports what was received");
        CHECK(r.onFrame(mode(REM2, 0x00), 2, 12, 1100, -55) == Why::None && r.step == 0, "other remote's correct button ignored");
        CHECK(r.onFrame(timer(REM2), 3, 13, 1200, -55) == Why::None && r.others == 2, "other devices are only counted");
        CHECK(r.onFrame(fr(MsgType::I, UNIT, REM, 0x31D9, {0x00, 0x00, 0x00}), 4, 14, 1300, -50) == Why::None && r.others == 2,
              "unit reply is no evidence of a press and not counted");
        CHECK(r.onFrame(mode(REM, 6), 5, 15, 1400, -60) == Why::Unknown && r.step == 0, "unknown command (boost) not used");
        CHECK(r.onFrame(fr(MsgType::I, REM, UNIT, 0x10E0, {0x00}), 6, 16, 1500, -60) == Why::Unknown, "other code from the remote: unknown");
        CHECK(r.onFrame(fr(MsgType::RQ, REM, UNIT, 0x22F1, {0x00, 0x00, 0x04}), 7, 17, 1600, -60) == Why::None && r.step == 0,
              "RQ from the remote ignored");
        CHECK(r.onFrame(mode(REM, 0x00), 8, 18, 1700, -60) == Why::Accepted && r.step == 1, "then the right button advances");
    }

    // ---- packets from before activation (sync numbers and guard) ------------------------------
    {
        Read r; r.start(1, REM, 5000, 100);
        CHECK(r.onFrame(mode(REM, 0x00), 1, 99, 5300, -60) == Why::Stale && r.step == 0, "sync before start -> stale (queued earlier)");
        CHECK(r.onFrame(mode(REM, 0x00), 2, 100, 5300, -60) == Why::Stale, "sync == start count -> stale");
        CHECK(r.onFrame(mode(REM, 0x00), 3, 101, 5300, -60) == Why::Accepted, "sync after start accepted");
        // next step is armed by that packet: an older queued packet cannot complete it
        CHECK(r.onFrame(mode(REM, 0x04), 4, 100, 5700, -60) == Why::Stale && r.step == 1, "packet older than the arming packet -> stale");
        CHECK(r.onFrame(mode(REM, 0x04), 5, 102, 5700, -60) == Why::Accepted, "newer packet completes step 2");
        Read q; q.start(2, REM, 8000, 0);                                  // radio without sync numbers
        CHECK(q.onFrame(mode(REM, 0x00), 1, 0, 8100, -60) == Why::Stale, "no sync numbers: 250 ms guard after arming");
        CHECK(q.onFrame(mode(REM, 0x00), 2, 0, 8300, -60) == Why::Accepted, "after the guard accepted");
    }

    // ---- timeout, cancel, restart ---------------------------------------------------------
    {
        Read r; r.start(3, REM, 0, 0);
        r.tick(59999); CHECK(r.st == St::Active && r.leftMs(59999) == 1, "still active before 60 s");
        r.tick(60000); CHECK(r.st == St::Timeout && r.leftMs(60000) == 0, "timeout after 60 s without a valid press");
        CHECK(r.onFrame(mode(REM, 0x00), 1, 5, 61000, -60) == Why::None && r.step == 0, "no progress after timeout");
        r.start(4, REM, 70000, 0);
        CHECK(r.st == St::Active && r.sid == 4 && r.why == Why::None && r.others == 0, "restart clears the previous session");
        r.onFrame(mode(REM, 0x00), 1, 0, 71000, -60);
        r.tick(71000 + 59999); CHECK(r.st == St::Active, "each accepted step re-arms the 60 s timer");
        r.cancel(); CHECK(r.st == St::Cancelled, "cancel");
        CHECK(r.onFrame(mode(REM, 0x04), 2, 0, 72000, -60) == Why::None && r.step == 1, "no progress after cancel");
        r.cancel(); CHECK(r.st == St::Cancelled, "cancel is idempotent");
    }

    // ---- RF interruption: no frames means no progress, only timeout ---------------------------
    {
        Read r; r.start(5, REM, 0, 0);
        r.onFrame(mode(REM, 0x00), 1, 0, 1000, -60);
        for (uint32_t t = 2000; t < 70000; t += 1000) r.tick(t);
        CHECK(r.st == St::Timeout && r.step == 1, "RF silence: step 2 never completes, session times out");
    }

    // ---- ids, tokens, request guard ------------------------------------------------------------
    {
        DeviceId d;
        CHECK(parseId("29:000101", d) && d == REM, "parseId valid");
        CHECK(parseId("63:262143", d) && d.serial == 0x3FFFF, "parseId max serial");
        const char* bad[] = { "", "29:00010", "29-000101", "64:000001", ("29:26214" "4") /* serial 2^18, split: no ID-shaped literal */, "29:00010a", " 29:000101", "29:0001011" };
        bool none = true; for (const char* b : bad) if (parseId(b, d)) none = false;
        CHECK(none && !parseId(nullptr, d), "parseId rejects malformed/out-of-range");
        CHECK(tokenEqual("abc123", "abc123") && !tokenEqual("abc123", "abc124") && !tokenEqual("abc", "abc1") && !tokenEqual("", ""),
              "token compare");
        CHECK(requestAllowed("application/json", "", "192.0.2.5"), "JSON without Origin (same-origin, older browser)");
        CHECK(requestAllowed("application/json; charset=utf-8", "http://192.0.2.5", "192.0.2.5"), "JSON from own origin");
        CHECK(!requestAllowed("text/plain", "", "192.0.2.5"), "simple (CSRF-able) content type refused");
        CHECK(!requestAllowed("application/jsonx", "", "192.0.2.5"), "look-alike content type refused");
        CHECK(!requestAllowed("application/json", "http://203.0.113.9", "192.0.2.5"), "foreign Origin refused");
        CHECK(!requestAllowed("application/json", "https://192.0.2.5", "192.0.2.5"), "scheme mismatch refused");
        CHECK(!requestAllowed("application/json", "http://192.0.2.5", ""), "Origin without Host refused");
        CHECK(hostAllowed("192.0.2.5", "192.0.2.5") && hostAllowed("192.0.2.5:80", "192.0.2.5"), "own IP (with port) allowed");
        CHECK(hostAllowed("orcon15rf.local", "192.0.2.5"), "mDNS name allowed");
        CHECK(!hostAllowed("192.0.2.55", "192.0.2.5") && !hostAllowed("192.0.2.", "192.0.2.5"), "other/prefix IP refused");
        CHECK(!hostAllowed("evil.example", "192.0.2.5") && !hostAllowed(".local", "192.0.2.5") && !hostAllowed("", "192.0.2.5"),
              "foreign names (DNS rebinding) refused");
    }

    // ---- pairing ----------------------------------------------------------------------------
    {
        CHECK(Pair::isBindCombo((1u << K_AUTO) | (1u << K_1)), "Auto + 1 is the bind combination");
        CHECK(!Pair::isBindCombo(1u << K_AUTO) && !Pair::isBindCombo(1u << K_1) &&
              !Pair::isBindCombo((1u << K_AUTO) | (1u << K_1) | (1u << K_2)), "single keys or extra keys are not");
        Pair p; const DeviceId bcast(63, 0x3FFFF);
        p.start(9, 0);
        CHECK(!strcmp(p.status(false, bcast), "ready"), "start: ready, nothing sent");
        p.onFrame(fr(MsgType::W, UNIT, US, 0x1FC9, {0x00}), US);
        CHECK(!p.confirmed, "a 1FC9 before our offer was sent is not a confirmation");
        p.bindResult(true, 1000);
        CHECK(!strcmp(p.status(false, bcast), "bind_sent"), "bind request sent != confirmed");
        p.onFrame(fr(MsgType::RQ, UNIT, US, 0x1FC9, {0x00}), US);           CHECK(!p.confirmed, "RQ 1FC9 is not a confirmation");
        p.onFrame(fr(MsgType::W, UNIT, REM, 0x1FC9, {0x00}), US);           CHECK(!p.confirmed, "1FC9 addressed to another device ignored");
        p.onFrame(fr(MsgType::I, US, US, 0x1FC9, {0x00}), US);              CHECK(!p.confirmed, "our own offer (echo) ignored");
        p.onFrame(fr(MsgType::I, UNIT, UNIT, 0x22F1, {0x00, 0x04, 0x04}), US); CHECK(!p.confirmed, "unit traffic is not a confirmation");
        p.onFrame(fr(MsgType::W, UNIT, US, 0x1FC9, {0x00}), US);
        CHECK(p.confirmed && p.from == UNIT && !strcmp(p.status(false, bcast), "rf_confirmed"), "W 1FC9 unit -> us: RF confirmation");
        CHECK(!p.paired(true, REM), "firmware learned a different unit: not paired");
        CHECK(!p.paired(false, UNIT), "firmware has not stored the unit: not paired");
        p.tick(2000, true, UNIT);
        CHECK(p.st == St::Done && !strcmp(p.status(true, UNIT), "paired"), "confirmation + firmware unit match -> paired");

        Pair f; f.start(10, 0); f.bindResult(false, 10);
        CHECK(!strcmp(f.status(false, bcast), "tx_failed") && !f.sent, "TX failure is reported, not hidden");
        Pair t; t.start(11, 0); t.bindResult(true, 0);
        t.tick(Pair::WINDOW_MS - 1, false, bcast); CHECK(t.st == St::Active, "window still open");
        t.tick(Pair::WINDOW_MS, false, bcast);     CHECK(!strcmp(t.status(false, bcast), "timeout"), "no confirmation in window -> timeout");
        Pair c; c.start(12, 0); c.cancel(); CHECK(!strcmp(c.status(false, bcast), "cancelled"), "cancel");
    }

    // ---- virtual remote command status --------------------------------------------------------
    {
        Cmd c;
        CHECK(!c.tooSoon(0), "first command allowed");
        c.record(1, K_2, 0, true, 1000, 40);
        CHECK(c.confirm == Cmd::Pending, "sent mode command: confirmation pending");
        CHECK(c.tooSoon(2000) && !c.tooSoon(2500), "1.5 s minimum gap between commands");
        c.onUnitMode(0x01, 41); CHECK(c.confirm == Cmd::Pending, "unit reports another mode: still pending");
        c.onUnitMode(0x02, 40); CHECK(c.confirm == Cmd::Pending, "matching report from before the send: ignored");
        c.onUnitMode(0x02, 41); CHECK(c.confirm == Cmd::Confirmed, "matching 31D9 after the send: confirmed");
        Cmd n; n.record(2, K_AWAY, 0, true, 0, 0);
        n.tick(Cmd::CONFIRM_MS - 1); CHECK(n.confirm == Cmd::Pending, "still waiting before 10 s");
        n.tick(Cmd::CONFIRM_MS);     CHECK(n.confirm == Cmd::NotConfirmed, "no report within 10 s: not confirmed");
        n.onUnitMode(0x00, 0);       CHECK(n.confirm == Cmd::NotConfirmed, "late report does not flip it");
        Cmd z; z.record(3, K_AUTO, 0, true, 0, 0); z.onUnitMode(0x04, 0);
        CHECK(z.confirm == Cmd::Confirmed, "radio without sync numbers: matching report confirms");
        Cmd tm; tm.record(4, K_TIMER, 30, true, 0, 0);
        CHECK(tm.confirm == Cmd::NotAvailable && tm.minutes == 30, "timer: no unit confirmation exists, never guessed");
        Cmd fl; fl.record(5, K_1, 0, false, 0, 0);
        CHECK(fl.confirm == Cmd::None && !fl.sent, "not sent: nothing to confirm");
        fl.onUnitMode(0x01, 1); CHECK(fl.confirm == Cmd::None, "unsent command never becomes confirmed");
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
