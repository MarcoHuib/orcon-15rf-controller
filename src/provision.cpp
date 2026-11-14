#include "provision.h"
#include <string.h>

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
static const uint8_t MAGIC[4] = {'O', 'R', 'C', 'S'};
Slot decodeSentinel(const uint8_t in[SENTINEL_LEN], Sentinel& out) {
    out = Sentinel();
    bool erased = true;
    for (size_t i = 0; i < SENTINEL_LEN; ++i) erased = erased && in[i] == 0xFF;
    if (erased) return Slot::Empty;
    int off = 0;                                  // bits that differ from the magic
    for (int i = 0; i < 4; ++i) for (uint8_t x = uint8_t(in[i] ^ MAGIC[i]); x; x &= uint8_t(x - 1)) ++off;
    if (off > 2) return Slot::Empty;              // never ours (old SPIFFS data, a torn first bytes write)
    if (off || in[4] != 1 || in[6] || in[7] || (in[5] & ~3) || rd32(in + 12) != crc32(in, 12)) return Slot::Corrupt;
    out.owned = in[5] & 1; out.resetPending = (in[5] & 2) != 0; out.seq = rd32(in + 8);
    return Slot::Valid;
}
SentinelRead chooseSentinel(Slot a, const Sentinel& ra, Slot b, const Sentinel& rb, Sentinel& out, uint8_t& next) {
    out = Sentinel(); next = 0;
    if (a == Slot::Valid && (b != Slot::Valid || int32_t(ra.seq - rb.seq) >= 0)) { out = ra; next = 1; return SentinelRead::Ok; }
    if (b == Slot::Valid) { out = rb; next = 0; return SentinelRead::Ok; }
    return a == Slot::Corrupt || b == Slot::Corrupt ? SentinelRead::Unreadable : SentinelRead::Absent;
}
SentinelView sentinelLoad(SentinelFlash& f) {
    SentinelView v;
    uint8_t raw[2][SENTINEL_LEN];
    Sentinel r[2];
    for (uint8_t i = 0; i < 2; ++i) {
        if (!f.read(i, raw[i])) return v;         // a read error: unreadable
        v.slot[i] = decodeSentinel(raw[i], r[i]);
    }
    v.readBoth = true;
    v.read = chooseSentinel(v.slot[0], r[0], v.slot[1], r[1], v.cur, v.next);
    v.redundant = v.read == SentinelRead::Ok && v.slot[0] == Slot::Valid && v.slot[1] == Slot::Valid &&
                  r[0].owned == r[1].owned && r[0].resetPending == r[1].resetPending;
    return v;
}
static bool putRecord(SentinelFlash& f, uint8_t sector, bool owned, bool pending, uint32_t seq) {
    Sentinel s; s.owned = owned; s.resetPending = pending; s.seq = seq;
    uint8_t rec[SENTINEL_LEN], back[SENTINEL_LEN];
    encodeSentinel(s, rec);
    // Read back before the caller touches the other sector: that one may hold the only evidence left.
    return f.erase(sector) && f.write(sector, rec) && f.read(sector, back) && !memcmp(back, rec, SENTINEL_LEN);
}
static bool holds(SentinelFlash& f, bool owned, bool pending) {
    const SentinelView v = sentinelLoad(f);
    return v.read == SentinelRead::Ok && v.redundant && v.cur.owned == owned && v.cur.resetPending == pending;
}
static bool store(SentinelFlash& f, const SentinelView& v, bool owned, bool pending) {
    if (!v.readBoth) return false;                // never erase on a guess (Codex OWNER-01, round 6)
    const bool ok = v.read == SentinelRead::Ok;
    const uint32_t seq = ok ? v.cur.seq : 0;
    const uint8_t first = ok ? v.next : v.slot[0] == Slot::Corrupt && v.slot[1] != Slot::Corrupt ? 1 : 0;
    return putRecord(f, first, owned, pending, seq + 1) && putRecord(f, uint8_t(1 - first), owned, pending, seq + 2) &&
           holds(f, owned, pending);
}
bool sentinelWrite(SentinelFlash& f, bool owned, bool pending) {
    const SentinelView v = sentinelLoad(f);
    return v.read != SentinelRead::Unreadable && store(f, v, owned, pending);
}
bool sentinelForce(SentinelFlash& f, bool owned, bool pending) { return store(f, sentinelLoad(f), owned, pending); }
bool sentinelOwn(SentinelFlash& f) {
    const SentinelView v = sentinelLoad(f);
    if (v.read == SentinelRead::Unreadable || (v.read == SentinelRead::Ok && v.cur.resetPending)) return false;
    if (v.read == SentinelRead::Ok && v.redundant && v.cur.owned) return true;
    return store(f, v, true, false);
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
