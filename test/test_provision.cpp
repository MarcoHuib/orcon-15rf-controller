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
    printf("provision: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
