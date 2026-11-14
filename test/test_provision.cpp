// Ownership and installation rules (src/provision.*): stored state and migration, which wizard
// route is allowed in which situation, and a factory reset that survives a power cut at every step.
#include "provision.h"
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>
using namespace provision;
static int fails = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static bool is(const char* got, const char* want) { return want ? got && !strcmp(got, want) : got == nullptr; }

// Flash as the reset sees it; "budget" operations succeed, then the power fails (every later op fails).
struct Flash : ResetOps {
    std::set<std::string> ns{"orcon", "orconauth", "orconown", "orconprofiles", "nvs.net80211", "orconother"};
    bool marked = false, dump = true, preOwner = true, dead = false, unreadable = false;
    int budget = 1 << 30;
    std::vector<std::string> log;
    bool step(const std::string& what) { if (dead || budget-- <= 0) { dead = true; return false; } log.push_back(what); return true; }
    Marker marker_() const { return unreadable ? Marker::Unreadable : marked ? Marker::Set : Marker::Absent; }
    Marker marker() override { return dead ? Marker::Unreadable : marker_(); }
    bool mark() override { if (!step("mark")) return false; marked = true; return true; }
    bool eraseNamespace(const char* n) override { if (!step(std::string("erase ") + n)) return false; ns.erase(n); return true; }
    bool eraseCrashDump() override { if (!step("dump")) return false; dump = false; return true; }
    bool retirePreOwnerSlot() override { if (!step("slot")) return false; preOwner = false; return true; }
    bool clear() override { if (!step("clear")) return false; marked = false; return true; }
    void reboot() { dead = false; budget = 1 << 30; }
    bool wiped() const { return ns.size() == 1 && ns.count("orconother") && !dump && !preOwner; }   // only a foreign namespace is left
    bool untouched() const { return ns.size() == 6 && dump && preOwner && !marked; }
};

// The two sentinel sectors with power cuts: after `budget` byte steps the power fails. An erase clears byte by
// byte, a write programs byte by byte; the byte being worked on at the cut is half done (`mask`: its bits that
// made it). Reads fail while the power is off, and for the sectors in `readErr` (bit i: sector i). The next
// `lostWrites` writes report success but change nothing (a silent flash fault).
struct SimFlash : SentinelFlash {
    uint8_t sec[2][SENTINEL_LEN];
    long budget = -1;                                     // -1: no cut
    uint8_t mask = 0, readErr = 0;
    int lostWrites = 0;
    bool dead = false;
    SimFlash() { memset(sec, 0xFF, sizeof(sec)); }
    bool step() { if (dead || budget == 0) { dead = true; return false; } if (budget > 0) --budget; return true; }
    bool read(uint8_t i, uint8_t out[SENTINEL_LEN]) override { if (dead || (readErr >> i & 1)) return false; memcpy(out, sec[i], SENTINEL_LEN); return true; }
    bool erase(uint8_t i) override {
        if (dead) return false;
        for (size_t b = 0; b < SENTINEL_LEN; ++b) { if (!step()) { sec[i][b] |= mask; return false; } sec[i][b] = 0xFF; }
        return true;
    }
    bool write(uint8_t i, const uint8_t in[SENTINEL_LEN]) override {
        if (dead) return false;
        if (lostWrites > 0) { --lostWrites; return true; }
        for (size_t b = 0; b < SENTINEL_LEN; ++b) { if (!step()) { sec[i][b] &= uint8_t(in[b] | uint8_t(~mask)); return false; } sec[i][b] &= in[b]; }
        return true;
    }
    void reboot() { dead = false; budget = -1; }
};
enum { UNOWNED = 0, OWNED = 1, PENDING = 3, ABSENT = 4, UNREADABLE = 5 };   // Ok: owned | pending << 1
static int st(SimFlash& f) {
    const SentinelView v = sentinelLoad(f);
    if (v.read != SentinelRead::Ok) return v.read == SentinelRead::Absent ? ABSENT : UNREADABLE;
    return (v.cur.owned ? 1 : 0) | (v.cur.resetPending ? 2 : 0);
}
static int g_cuts = 0;
// `op` with the power cut at every step (and every half-programmed byte pattern). After the reboot the
// storage reads one of `allowed`; once the op ran to its end, it reports success, reads `done`, redundantly.
template <class Op> static bool cutsOk(const SimFlash& from, Op op, std::set<int> allowed, int done) {
    bool ok = true;
    for (uint8_t mask : {0x00, 0x0F, 0xF0, 0xA5}) for (long k = 0;; ++k) {
        SimFlash f = from; f.budget = k; f.mask = mask;
        const bool r = op(f), finished = !f.dead;
        f.reboot(); ++g_cuts;
        const int s = st(f);
        ok = ok && allowed.count(s) && (!r || s == done);
        if (finished) { ok = ok && r && s == done && sentinelLoad(f).redundant; break; }
    }
    return ok;
}
// After a completed write, damage to either sector (erased, any single bit, garbage) never changes the state.
static bool survivesDamage(const SimFlash& d, int want) {
    SimFlash a = d;
    bool ok = st(a) == want && sentinelLoad(a).redundant;
    for (int i = 0; i < 2; ++i) {
        SimFlash e = d; memset(e.sec[i], 0xFF, SENTINEL_LEN); ok = ok && st(e) == want;
        SimFlash g = d; for (size_t b = 0; b < SENTINEL_LEN; ++b) g.sec[i][b] = uint8_t(b * 37 + 11); ok = ok && st(g) == want;
        for (size_t bit = 0; bit < SENTINEL_LEN * 8; ++bit) {
            SimFlash x = d; x.sec[i][bit / 8] ^= uint8_t(1u << (bit % 8)); ok = ok && st(x) == want;
        }
    }
    return ok;
}
static bool recordOwner(SimFlash& f) { return sentinelOwn(f); }
static bool markReset(SimFlash& f) { return sentinelWrite(f, true, true); }
static bool clearReset(SimFlash& f) { return sentinelWrite(f, false, false); }
static bool forceReset(SimFlash& f) { return sentinelForce(f, true, true); }

int main() {
    // ---- stored state and migration
    bool mig = true;
    CHECK(fromStored(0, false, false, mig) == State::New && !mig);
    CHECK(fromStored(0, true, false, mig) == State::Legacy && mig);              // password from pre-owner firmware
    CHECK(fromStored(STORED_SETUP, false, false, mig) == State::Setup && !mig);  // step 1 interrupted before the password
    CHECK(fromStored(STORED_SETUP, true, false, mig) == State::Setup && !mig);
    CHECK(fromStored(STORED_DONE, true, false, mig) == State::Done && !mig);
    CHECK(fromStored(STORED_LEGACY, true, false, mig) == State::Legacy && !mig);
    CHECK(fromStored(7, false, false, mig) == State::Done && fromStored(255, true, false, mig) == State::Done);   // unknown: fail closed
    // Codex OWNER-07: a stored owner key always means Done (an interrupted registration, a partial erase)
    CHECK(fromStored(STORED_LEGACY, true, true, mig) == State::Done && !mig);
    CHECK(fromStored(STORED_SETUP, true, true, mig) == State::Done && !mig);
    CHECK(fromStored(0, true, true, mig) == State::Done && !mig && fromStored(0, false, true, mig) == State::Done && !mig);
    CHECK(!strcmp(name(State::New), "new") && !strcmp(name(State::Legacy), "legacy") && installing(State::Setup) && !installing(State::Done));

    // ---- route policy
    const Route ALL[] = {Route::State, Route::Password, Route::WifiTest, Route::Key, Route::KeyFile, Route::KeyVerify, Route::Recover};
    Ctx c;
    for (Route r : ALL) CHECK(is(allow(r, c), "setup-ap-only"));           // the home network never reaches the wizard
    c.viaAp = true;
    CHECK(is(allow(Route::State, c), nullptr) && is(allow(Route::Password, c), nullptr));
    c.staUp = true;                                                       // during a WiFi test: only the state poll
    for (Route r : ALL) CHECK(is(allow(r, c), r == Route::State ? nullptr : "wifi-test-running"));
    c.staUp = false;
    CHECK(is(allow(Route::WifiTest, c), "login-required") && is(allow(Route::Key, c), "password-required"));
    CHECK(is(allow(Route::Recover, c), "not-installed"));
    // Step 1 done: state Setup, logged in.
    c.state = State::Setup; c.passwordSet = true; c.loggedIn = true;
    CHECK(is(allow(Route::Password, c), "password-already-set"));          // never a second "first" password
    CHECK(is(allow(Route::WifiTest, c), nullptr) && is(allow(Route::Key, c), nullptr));
    CHECK(is(allow(Route::KeyFile, c), "no-key-pending") && is(allow(Route::KeyVerify, c), "no-key-pending"));
    c.keyPending = true;
    CHECK(is(allow(Route::KeyFile, c), nullptr) && is(allow(Route::KeyVerify, c), nullptr));
    c.loggedIn = false;
    CHECK(is(allow(Route::Key, c), "login-required") && is(allow(Route::KeyVerify, c), "login-required"));
    // Installed (Done): the wizard is closed unless the PRG menu started the recovery mode.
    c = Ctx(); c.viaAp = true; c.state = State::Done; c.passwordSet = true; c.loggedIn = true; c.keyPending = true;
    CHECK(is(allow(Route::Password, c), "not-installing") && is(allow(Route::Key, c), "physical-mode-required"));
    CHECK(is(allow(Route::Recover, c), "physical-mode-required"));
    CHECK(is(allow(Route::WifiTest, c), nullptr));                         // network recovery from the AP, logged in
    c.physical = true;
    CHECK(is(allow(Route::Key, c), nullptr) && is(allow(Route::KeyVerify, c), nullptr) && is(allow(Route::Recover, c), nullptr));
    c.loggedIn = false;
    CHECK(is(allow(Route::Recover, c), nullptr) && is(allow(Route::Key, c), "login-required"));   // recovery needs no login, a new key does
    CHECK(is(allow(Route::Password, c), "not-installing"));
    // Legacy: a key can be registered in the recovery mode; there is none to recover with yet.
    c.state = State::Legacy; c.loggedIn = true;
    CHECK(is(allow(Route::Key, c), nullptr) && is(allow(Route::Recover, c), "no-owner-key"));
    c.physical = false;
    CHECK(is(allow(Route::Key, c), "physical-mode-required"));

    // ---- factory reset
    {   // the normal case: requested by the gesture, completed at the next boot
        Flash f;
        CHECK(resume(f) && f.untouched() && f.log.empty());               // nothing pending: nothing happens
        CHECK(request(f) && f.marked && f.ns.size() == 6);                // the request itself erases nothing
        f.reboot(); CHECK(resume(f) && f.wiped() && !f.marked && f.ns.count("orconother"));
        CHECK(f.log.size() == 9 && f.log[0] == "mark" && f.log[1] == "erase orconauth" && f.log[2] == "erase orconown" && f.log.back() == "clear");
    }
    // A power cut after every possible number of steps, then normal boots until it settles.
    for (int cut = 0; cut <= 10; ++cut) {
        Flash f;
        f.budget = cut;
        const bool reqOk = request(f);
        if (reqOk) resume(f);                                             // the device would restart in between
        const bool wasMarked = f.marked;
        for (int boot = 0; boot < 3; ++boot) { f.reboot(); CHECK(resume(f)); }
        if (cut == 0) CHECK(!reqOk && f.untouched());                     // cut before the marker: no reset at all
        else CHECK(wasMarked || f.wiped() ? f.wiped() && !f.marked : f.untouched());
    }
    {   // Codex OWNER-01: an unreadable marker is never "no reset pending": not done, nothing claimed
        Flash f; request(f); f.reboot(); f.unreadable = true;
        CHECK(!resume(f) && f.ns.size() == 6);
        f.unreadable = false; CHECK(resume(f) && f.wiped());               // readable again: the reset completes
    }
    {   // cut after the first erase: no password, but the owner state stays: never a Legacy-looking state
        Flash f; request(f); f.reboot(); f.budget = 1; resume(f);
        CHECK(!f.ns.count("orconauth") && f.ns.count("orconown"));
        bool mig = false;
        CHECK(fromStored(STORED_DONE, false, false, mig) == State::Done && !mig);  // installed without a password: locked
    }
    {   // the recovery mode timer: 15 minutes, then latched off (a millis() wrap never revives it)
        PhysicalTimer p; CHECK(p.left(0) == 0);
        p.arm(true); CHECK(p.left(1000) == PHYSICAL_MS - 1000 && p.left(PHYSICAL_MS) == 0);
        CHECK(p.left(5) == 0 && p.left(PHYSICAL_MS - 1) == 0);           // after a wrap: still off
        PhysicalTimer q; q.arm(false); CHECK(q.left(10) == 0);
    }
    {   // a failing erase keeps the marker (retried at the next boot); never "done" with data left
        struct Stuck : Flash { bool eraseNamespace(const char* n) override { return strcmp(n, "orconprofiles") && Flash::eraseNamespace(n); } } f;
        request(f); f.reboot();
        CHECK(!resume(f) && f.marked && !f.ns.count("orconauth") && !f.ns.count("orconown") && f.ns.count("orconprofiles"));
    }
    // ---- the ownership sentinel (Codex OWNER-11) ----
    {
        const uint8_t kat[] = {'1','2','3','4','5','6','7','8','9'};
        CHECK(crc32(kat, sizeof(kat)) == 0xCBF43926u);                       // CRC-32/IEEE check value
        uint8_t rec[SENTINEL_LEN]; Sentinel out;
        for (int f = 0; f < 4; ++f) for (uint32_t seq : {0u, 1u, 77u, 0xFFFFFFFEu}) {
            Sentinel in; in.owned = f & 1; in.resetPending = (f & 2) != 0; in.seq = seq;
            encodeSentinel(in, rec);
            CHECK(decodeSentinel(rec, out) == Slot::Valid && out.owned == in.owned && out.resetPending == in.resetPending && out.seq == seq);
        }
        uint8_t ff[SENTINEL_LEN]; memset(ff, 0xFF, sizeof(ff));
        CHECK(decodeSentinel(ff, out) == Slot::Empty);                        // erased flash
        uint8_t foreign[SENTINEL_LEN] = {'h','s','p','i','f','f','s',0,1,2,3,4,5,6,7,8};
        CHECK(decodeSentinel(foreign, out) == Slot::Empty);                   // old SPIFFS data: not ours
        Sentinel own; own.owned = true; own.seq = 5; encodeSentinel(own, rec);
        uint8_t bad[SENTINEL_LEN];
        memcpy(bad, rec, sizeof(bad)); bad[9] ^= 1; CHECK(decodeSentinel(bad, out) == Slot::Corrupt);        // seq flipped
        memcpy(bad, rec, sizeof(bad)); bad[4] = 2; CHECK(decodeSentinel(bad, out) == Slot::Corrupt);         // version
        memcpy(bad, rec, sizeof(bad)); bad[5] |= 4; CHECK(decodeSentinel(bad, out) == Slot::Corrupt);        // unknown flag
        // A torn write programs a prefix of the record onto erased flash: never a valid record.
        bool tornOk = true;
        for (size_t k = 0; k < SENTINEL_LEN; ++k) {
            uint8_t torn[SENTINEL_LEN]; memset(torn, 0xFF, sizeof(torn)); memcpy(torn, rec, k);
            tornOk = tornOk && decodeSentinel(torn, out) != Slot::Valid;
        }
        CHECK(tornOk);
        // Damaged magic is still ours (Codex OWNER-11): 1-2 bits off is a damaged record, more is foreign data.
        uint8_t mg[SENTINEL_LEN];
        memcpy(mg, rec, sizeof(mg)); mg[0] ^= 0x01; CHECK(decodeSentinel(mg, out) == Slot::Corrupt);
        memcpy(mg, rec, sizeof(mg)); mg[1] ^= 0x10; mg[3] ^= 0x02; CHECK(decodeSentinel(mg, out) == Slot::Corrupt);
        memcpy(mg, rec, sizeof(mg)); mg[0] ^= 0x07; CHECK(decodeSentinel(mg, out) == Slot::Empty);
        // Choosing between the sectors: the newer sequence, across the wrap too (Codex OWNER-14).
        Sentinel a, b, cur; uint8_t next = 9;
        a.owned = true; a.seq = 1; b.owned = false; b.seq = 2;
        CHECK(chooseSentinel(Slot::Valid, a, Slot::Valid, b, cur, next) == SentinelRead::Ok && !cur.owned && cur.seq == 2 && next == 0);
        CHECK(chooseSentinel(Slot::Valid, a, Slot::Corrupt, b, cur, next) == SentinelRead::Ok && cur.owned && next == 1);
        CHECK(chooseSentinel(Slot::Corrupt, a, Slot::Corrupt, b, cur, next) == SentinelRead::Unreadable);   // flash damage: fail closed
        CHECK(chooseSentinel(Slot::Corrupt, a, Slot::Empty, b, cur, next) == SentinelRead::Unreadable);     // never "absent" (Codex OWNER-01)
        CHECK(chooseSentinel(Slot::Empty, a, Slot::Corrupt, b, cur, next) == SentinelRead::Unreadable);
        CHECK(chooseSentinel(Slot::Empty, a, Slot::Empty, b, cur, next) == SentinelRead::Absent && next == 0);
        a.seq = 0xFFFFFFFFu; b.seq = 0;
        CHECK(chooseSentinel(Slot::Valid, a, Slot::Valid, b, cur, next) == SentinelRead::Ok && cur.seq == 0 && next == 0);
        CHECK(chooseSentinel(Slot::Valid, b, Slot::Valid, a, cur, next) == SentinelRead::Ok && cur.seq == 0 && next == 1);

        // The whole life cycle with a power cut at every byte of every erase and write.
        SimFlash fresh;
        CHECK(st(fresh) == ABSENT);
        // Wizard step 1 on a new device. A cut: still absent, the new owner, or (a torn first record) unreadable.
        CHECK(cutsOk(fresh, recordOwner, {ABSENT, UNREADABLE, OWNED}, OWNED));
        SimFlash owned = fresh; CHECK(recordOwner(owned) && survivesDamage(owned, OWNED));
        SimFlash same = owned; CHECK(recordOwner(same) && !memcmp(same.sec, owned.sec, sizeof(same.sec)));   // already recorded: no write
        // The PRG reset request, the completed reset, and a new installation (the Codex OWNER-11 case: after
        // that, no "unowned" record is left to revive when the newest one is damaged).
        CHECK(cutsOk(owned, markReset, {OWNED, PENDING}, PENDING));
        SimFlash pending = owned; CHECK(markReset(pending) && survivesDamage(pending, PENDING));
        CHECK(cutsOk(pending, clearReset, {PENDING, UNOWNED}, UNOWNED));
        SimFlash cleared = pending; CHECK(clearReset(cleared) && survivesDamage(cleared, UNOWNED));
        CHECK(cutsOk(cleared, recordOwner, {UNOWNED, OWNED}, OWNED));
        SimFlash again = cleared; CHECK(recordOwner(again) && survivesDamage(again, OWNED));
        // A pending reset is never cleared by a non-reset write (Codex OWNER-13).
        SimFlash p2 = pending; CHECK(!recordOwner(p2) && !memcmp(p2.sec, pending.sec, sizeof(p2.sec)));
        // The boot repair: one damaged sector is rewritten, a cut never loses the owner.
        SimFlash half = owned; half.sec[0][13] ^= 0x10;
        CHECK(st(half) == OWNED && !sentinelLoad(half).redundant && cutsOk(half, recordOwner, {OWNED}, OWNED));
        SimFlash halfP = pending; memset(halfP.sec[1], 0xFF, SENTINEL_LEN);
        CHECK(st(halfP) == PENDING && !recordOwner(halfP) && cutsOk(halfP, clearReset, {PENDING, UNOWNED}, UNOWNED));
        // Unreadable storage (Codex OWNER-01): ordinary writes refuse; the PRG emergency reset rewrites it, and a
        // cut at any point reads unreadable or the pending reset, never absent. One more outcome is real: a cut
        // erase can restore a damaged bit first, which brings back the record from before the damage (here:
        // owned). That is the state before the emergency reset, which then simply did not happen yet.
        SimFlash bothBad = owned; bothBad.sec[0][14] ^= 1; bothBad.sec[1][2] ^= 4;
        SimFlash badEmpty = owned; badEmpty.sec[0][15] ^= 0x80; memset(badEmpty.sec[1], 0xFF, SENTINEL_LEN);
        SimFlash emptyBad = owned; memset(emptyBad.sec[0], 0xFF, SENTINEL_LEN); emptyBad.sec[1][0] ^= 0x01;   // damaged magic
        for (SimFlash* u : {&bothBad, &badEmpty, &emptyBad}) {
            SimFlash w = *u, o = *u;
            CHECK(st(*u) == UNREADABLE && !markReset(w) && !clearReset(w) && !recordOwner(o));
            CHECK(cutsOk(*u, forceReset, {UNREADABLE, PENDING, OWNED}, PENDING));
        }
        CHECK(cutsOk(owned, forceReset, {OWNED, PENDING}, PENDING));   // readable: it continues the sequence
        // A read error is no classification (Codex OWNER-01, round 6): the emergency rewrite refuses and erases
        // nothing, whichever sector failed. A write that silently did not happen is caught by the read-back
        // before the damaged sector (the only evidence left) is touched.
        for (SimFlash* u : {&bothBad, &badEmpty, &emptyBad, &owned}) for (uint8_t m : {1, 2, 3}) {
            SimFlash r = *u; r.readErr = m;
            CHECK(!forceReset(r) && !memcmp(r.sec, u->sec, sizeof(r.sec)));
        }
        for (SimFlash* u : {&badEmpty, &emptyBad}) {
            SimFlash l = *u; l.lostWrites = 1;
            CHECK(!forceReset(l) && st(l) == UNREADABLE && forceReset(l) && st(l) == PENDING);   // retried: done
        }
        // Sequence wrap (Codex OWNER-14): clearing at UINT32_MAX really clears.
        SimFlash wrap; Sentinel ws; ws.owned = true; ws.resetPending = true;
        ws.seq = 0xFFFFFFFEu; encodeSentinel(ws, wrap.sec[0]); ws.seq = 0xFFFFFFFFu; encodeSentinel(ws, wrap.sec[1]);
        CHECK(st(wrap) == PENDING && cutsOk(wrap, clearReset, {PENDING, UNOWNED}, UNOWNED));
        SimFlash w2 = wrap; CHECK(clearReset(w2) && st(w2) == UNOWNED && recordOwner(w2) && st(w2) == OWNED && survivesDamage(w2, OWNED));
        // A read error is unreadable, and nothing writes then.
        SimFlash re = owned; re.readErr = 3;
        CHECK(st(re) == UNREADABLE && !recordOwner(re) && !clearReset(re) && !memcmp(re.sec, owned.sec, sizeof(re.sec)));
        CHECK(g_cuts > 2000);
        // The boot decision: an owned device whose NVS lost everything is Locked, never New.
        Sentinel o; o.owned = true; Sentinel u;
        CHECK(withSentinel(State::New, SentinelRead::Ok, o) == State::Locked);
        CHECK(withSentinel(State::New, SentinelRead::Unreadable, u) == State::Locked);
        CHECK(withSentinel(State::New, SentinelRead::Absent, u) == State::New && withSentinel(State::New, SentinelRead::Ok, u) == State::New);
        CHECK(withSentinel(State::Done, SentinelRead::Unreadable, u) == State::Done && withSentinel(State::Legacy, SentinelRead::Ok, u) == State::Legacy);
        // Locked: nothing but the read-only state on the setup AP.
        Ctx k; k.viaAp = true; k.state = State::Locked; k.physical = true; k.loggedIn = true;
        bool lockedOk = !strcmp(name(State::Locked), "locked") && !installing(State::Locked) && is(allow(Route::State, k), nullptr);
        for (Route r : ALL) if (r != Route::State) lockedOk = lockedOk && is(allow(r, k), "factory-reset-required");
        CHECK(lockedOk);
    }
    printf("provision: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
