#include "provision.h"

namespace provision {

State fromStored(uint8_t stored, bool credPresent, bool pubPresent, bool& migrate) {
    migrate = false;
    if (pubPresent) return State::Done;           // an owner key exists: whatever the state says, never less
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
        case State::Locked: return "locked";
        default: return "done";
    }
}

const char* allow(Route r, const Ctx& c) {
    if (!c.viaAp) return "setup-ap-only";
    if (r == Route::State) return nullptr;        // read-only, no secrets: shows what the AP client can do
    if (c.state == State::Locked) return "factory-reset-required";   // only the PRG factory reset reopens anything
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

uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) { c ^= p[i]; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    return ~c;
}
static void le32(uint8_t* o, uint32_t v) { o[0] = uint8_t(v); o[1] = uint8_t(v >> 8); o[2] = uint8_t(v >> 16); o[3] = uint8_t(v >> 24); }
static uint32_t rd32(const uint8_t* i) { return uint32_t(i[0]) | uint32_t(i[1]) << 8 | uint32_t(i[2]) << 16 | uint32_t(i[3]) << 24; }
void encodeSentinel(const Sentinel& s, uint8_t out[SENTINEL_LEN]) {
    out[0] = 'O'; out[1] = 'R'; out[2] = 'C'; out[3] = 'S'; out[4] = 1;
    out[5] = uint8_t((s.owned ? 1 : 0) | (s.resetPending ? 2 : 0)); out[6] = out[7] = 0;
    le32(out + 8, s.seq); le32(out + 12, crc32(out, 12));
}
Slot decodeSentinel(const uint8_t in[SENTINEL_LEN], Sentinel& out) {
    out = Sentinel();
    if (in[0] != 'O' || in[1] != 'R' || in[2] != 'C' || in[3] != 'S') return Slot::Empty;   // erased or foreign data
    if (in[4] != 1 || in[6] || in[7] || (in[5] & ~3) || rd32(in + 12) != crc32(in, 12)) return Slot::Corrupt;
    out.owned = in[5] & 1; out.resetPending = (in[5] & 2) != 0; out.seq = rd32(in + 8);
    return Slot::Valid;
}
SentinelRead chooseSentinel(Slot a, const Sentinel& ra, Slot b, const Sentinel& rb, Sentinel& out, uint8_t& next) {
    out = Sentinel(); next = 0;
    if (a == Slot::Valid && (b != Slot::Valid || ra.seq >= rb.seq)) { out = ra; next = 1; return SentinelRead::Ok; }
    if (b == Slot::Valid) { out = rb; next = 0; return SentinelRead::Ok; }
    return a == Slot::Corrupt && b == Slot::Corrupt ? SentinelRead::Unreadable : SentinelRead::Absent;
}
State withSentinel(State nvs, SentinelRead r, const Sentinel& s) {
    if (nvs != State::New) return nvs;            // NVS still knows its owner
    return r == SentinelRead::Unreadable || (r == SentinelRead::Ok && s.owned) ? State::Locked : State::New;
}

bool resume(ResetOps& o) {
    const Marker m = o.marker();
    if (m == Marker::Absent) return true;
    if (m == Marker::Unreadable) return false;
    for (size_t i = 0; i < WIPE_N; ++i) if (!o.eraseNamespace(WIPE[i])) return false;
    return o.eraseCrashDump() && o.retirePreOwnerSlot() && o.clear();
}

}  // namespace provision
