#include "provision.h"

namespace provision {

State fromStored(uint8_t stored, bool credPresent, bool& migrate) {
    migrate = false;
    switch (stored) {
        case 0:
            if (!credPresent) return State::New;
            migrate = true;                       // only firmware without this model wrote a password alone
            return State::Legacy;
        case STORED_SETUP: return State::Setup;
        case STORED_LEGACY: return State::Legacy;
        default: return State::Done;              // STORED_DONE, or unknown: fail closed
    }
}
const char* name(State s) {
    switch (s) {
        case State::New: return "new";
        case State::Setup: return "setup";
        case State::Legacy: return "legacy";
        default: return "done";
    }
}

const char* allow(Route r, const Ctx& c) {
    if (!c.viaAp) return "setup-ap-only";
    if (r == Route::State) return nullptr;        // read-only, no secrets: shows what the AP client can do
    if (c.staUp) return "wifi-test-running";      // never while a path to the home network exists
    const bool owner = c.state == State::Done || c.state == State::Legacy;
    switch (r) {
        case Route::Password:                      // wizard step 1: the first password, once
            if (!installing(c.state)) return "not-installing";
            return c.passwordSet ? "password-already-set" : nullptr;
        case Route::WifiTest:                      // step 2, and network recovery from the AP
            return c.loggedIn ? nullptr : "login-required";
        case Route::Key:                           // step 4, or a new key in the button-started recovery mode
        case Route::KeyFile:
        case Route::KeyVerify:
            if (!(c.state == State::Setup || (owner && c.physical))) return installing(c.state) ? "password-required" : "physical-mode-required";
            if (!c.loggedIn) return "login-required";
            return r == Route::Key || c.keyPending ? nullptr : "no-key-pending";
        case Route::Recover:                       // forgotten password: the recovery key file, no login
            if (c.state != State::Done) return c.state == State::Legacy ? "no-owner-key" : "not-installed";
            return c.physical ? nullptr : "physical-mode-required";
        default: return "bad-route";
    }
}

bool request(ResetOps& o) { return o.mark() && o.marker() == Marker::Set; }

bool resume(ResetOps& o) {
    const Marker m = o.marker();
    if (m == Marker::Absent) return true;
    if (m == Marker::Unreadable) return false;
    for (size_t i = 0; i < WIPE_N; ++i) if (!o.eraseNamespace(WIPE[i])) return false;
    return o.eraseCrashDump() && o.erasePreOwnerSlot() && o.clear();
}

}  // namespace provision
