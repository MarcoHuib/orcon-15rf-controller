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
        // Choosing between the sectors.
        Sentinel a, b, cur; uint8_t next = 9;
        a.owned = true; a.seq = 1; b.owned = false; b.seq = 2;
        CHECK(chooseSentinel(Slot::Valid, a, Slot::Valid, b, cur, next) == SentinelRead::Ok && !cur.owned && cur.seq == 2 && next == 0);
        CHECK(chooseSentinel(Slot::Valid, a, Slot::Corrupt, b, cur, next) == SentinelRead::Ok && cur.owned && next == 1);
        CHECK(chooseSentinel(Slot::Corrupt, a, Slot::Corrupt, b, cur, next) == SentinelRead::Unreadable);   // flash damage: fail closed
        CHECK(chooseSentinel(Slot::Corrupt, a, Slot::Empty, b, cur, next) == SentinelRead::Absent);        // a torn first write
        CHECK(chooseSentinel(Slot::Empty, a, Slot::Empty, b, cur, next) == SentinelRead::Absent && next == 0);
        // Writes with a power cut at every point of the erase and of the write: the result is always the
        // old state or the new one, never unreadable and never anything else.
        struct Dev { uint8_t sec[2][SENTINEL_LEN]; };
        auto read = [](const Dev& d, Sentinel& c, uint8_t& n) {
            Sentinel ra, rb; const Slot sa = decodeSentinel(d.sec[0], ra), sb = decodeSentinel(d.sec[1], rb);
            return chooseSentinel(sa, ra, sb, rb, c, n);
        };
        bool cutOk = true; int cuts = 0;
        for (int from = 0; from < 2; ++from) {
            Dev d; memset(&d, 0xFF, sizeof(d));
            Sentinel first; first.owned = from == 1; first.seq = 1;
            encodeSentinel(first, d.sec[0]); first.seq = 2; encodeSentinel(first, d.sec[1]);   // first write fills both
            Sentinel was; uint8_t tgt = 0; read(d, was, tgt);
            Sentinel want; want.owned = !was.owned; want.resetPending = true; want.seq = was.seq + 1;
            uint8_t nrec[SENTINEL_LEN]; encodeSentinel(want, nrec);
            for (size_t e = 0; e <= SENTINEL_LEN; ++e) for (size_t w = 0; w <= (e == SENTINEL_LEN ? SENTINEL_LEN : 0); ++w) {
                Dev x = d;
                for (size_t i = 0; i < e; ++i) x.sec[tgt][i] = 0xFF;          // the erase got this far
                for (size_t i = 0; i < w; ++i) x.sec[tgt][i] = nrec[i];       // then the write
                Sentinel c; uint8_t n;
                const SentinelRead r = read(x, c, n); ++cuts;
                const bool isOld = r == SentinelRead::Ok && c.owned == was.owned && c.resetPending == was.resetPending;
                const bool isNew = r == SentinelRead::Ok && c.owned == want.owned && c.resetPending == want.resetPending;
                cutOk = cutOk && (isOld || isNew) && (w < SENTINEL_LEN || isNew);
            }
        }
        CHECK(cutOk && cuts == 66);
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
