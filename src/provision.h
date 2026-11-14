// provision.h — ownership and installation rules (pure; host-tested: test/test_provision.cpp).
// Device glue: owner_store.cpp (NVS, RTC, flash), setup_web.cpp (routes), net.cpp (WiFi, OLED).
//
// States, stored in NVS "orconown"/"state":
//   New     nothing stored: first start or after a factory reset. Setup AP, wizard step 1.
//   Setup   1: the owner set a password; wizard steps 2-4 run. Written BEFORE the password, so
//           a password without a state can only come from firmware older than this model.
//   Done    2: the owner's public key is registered: normal operation.
//   Legacy  3: migrated from a pre-owner firmware (a password existed, no state). Works as Done;
//           the old OTA-key-signed password recovery stays until the owner registers a key.
// Anything else stored reads as Done without a key: fail closed (no wizard, no vendor recovery).
//
// The wizard's secret-bearing routes (first password, WiFi password, key file, recovery) need all of:
//   - the request came in on the setup access point (local address = AP address, client in the AP
//     subnet: checked on the socket, not from headers);
//   - the station interface is down, so the device has no path to the home network at that moment;
//   - an explicit installation mode: state New/Setup, or the physical recovery mode that only the
//     PRG button menu can start (RTC flag, one boot, 15 minutes).
// No HTTP route can start a factory reset or return the device to New.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace provision {
// Locked: NVS lost its owner without a completed factory reset (the Arduino core formats a full or newer
// NVS before setup(), Codex OWNER-11), while the ownership sentinel below says the device has an owner.
// Nothing reopens: no wizard, no login, no recovery. Only the PRG factory reset clears it.
enum class State : uint8_t { New, Setup, Done, Legacy, Locked };
constexpr uint8_t STORED_SETUP = 1, STORED_DONE = 2, STORED_LEGACY = 3;
constexpr uint32_t PHYSICAL_MS = 15u * 60u * 1000u;      // the button-started recovery mode lasts 15 minutes

// The button-started recovery mode: active for PHYSICAL_MS after boot, then off for good. Latched,
// so a 49.7-day millis() wrap never revives it (Codex OWNER-04); the main loop asks every pass.
class PhysicalTimer {
public:
    void arm(bool on) { on_ = on; over_ = false; }
    uint32_t left(uint32_t now) {
        if (!on_ || over_) return 0;
        if (now >= PHYSICAL_MS) { over_ = true; return 0; }
        return PHYSICAL_MS - now;
    }
private:
    bool on_ = false, over_ = false;
};

// From NVS at boot. migrate: store STORED_LEGACY now (a pre-owner password, nothing else).
// pubPresent: an owner key is stored (or unreadable: count it). A stored key always means Done, so an
// interrupted registration (key written, state not) never leaves Setup or Legacy (Codex OWNER-07).
State fromStored(uint8_t stored, bool credPresent, bool pubPresent, bool& migrate);
const char* name(State);
inline bool installing(State s) { return s == State::New || s == State::Setup; }

struct Ctx {
    State state = State::New;
    bool viaAp = false;        // this request arrived on the setup AP (socket addresses)
    bool staUp = false;        // the station interface is associated (a path to the home network)
    bool physical = false;     // the PRG-menu recovery mode is active
    bool loggedIn = false;     // a valid session with its CSRF token
    bool passwordSet = false;
    bool keyPending = false;   // a key was generated this boot and awaits the owner's verification
};
enum class Route : uint8_t { State, Password, WifiTest, Key, KeyFile, KeyVerify, Recover };
// nullptr: allowed. Otherwise the error code for the reply.
const char* allow(Route, const Ctx&);

// ---- factory reset: a marker first, then everything with ownership or personal data, the
// marker last. Each step is idempotent, so a reset interrupted by a power cut resumes at the next
// boot and always completes; a reset that was never marked never happens.
// The marker is read tri-state: unreadable is never "no reset pending" (Codex OWNER-01).
enum class Marker : uint8_t { Absent, Set, Unreadable };
struct ResetOps {
    virtual Marker marker() = 0;
    virtual bool mark() = 0;                              // set (and read back) the marker
    virtual bool eraseNamespace(const char* ns) = 0;      // every key in one NVS namespace
    virtual bool eraseCrashDump() = 0;                    // a crash dump may hold secrets from RAM
    // The other OTA slot: no image from before this model may stay bootable there. It is replaced by a
    // copy of the running, confirmed image (not erased), so the device keeps two bootable images and
    // the rollback/fallback safety net survives the reset.
    virtual bool retirePreOwnerSlot() = 0;
    virtual bool clear() = 0;                             // remove the marker
    virtual ~ResetOps() {}
};
// The password first, then the owner state and key: a reset cut off in between leaves an
// installed device without a password (locked, fail closed), never "a password without a state",
// which is what a pre-owner (Legacy) device looks like. Then pairing and profiles, WiFi and
// identities, and the WiFi driver's own copy of the home network and AP credentials (Arduino
// stores them there unless WiFi.persistent(false); net.cpp now turns that off, older boots wrote them).
constexpr const char* WIPE[] = {"orconauth", "orconown", "orconprofiles", "orcon", "nvs.net80211"};
constexpr size_t WIPE_N = sizeof(WIPE) / sizeof(WIPE[0]);
bool request(ResetOps&);   // the confirmed PRG gesture: only sets the marker (the work runs at the next boot)

// ---- the ownership sentinel (Codex OWNER-11) ----------------------------------------------------------
// Two raw 4 KB sectors at the end of the spiffs partition (unused by this firmware; outside NVS, so an
// NVS auto-format cannot touch them) hold "this device has an owner" and "a confirmed factory reset is
// pending" (the reset marker lives here). Both sectors always hold the same state (sequence number + CRC);
// a change goes to the sector without the current record first, so a power cut never loses the last state.
struct Sentinel { bool owned = false; bool resetPending = false; uint32_t seq = 0; };
constexpr size_t SENTINEL_LEN = 16;   // "ORCS", version, flags, 2 reserved, seq (LE), CRC-32 (LE) over 0..11
// Empty: erased, or data that was never ours (magic more than 2 bits off: old SPIFFS content).
// Corrupt: our record, damaged (magic within 2 bits, or a bad CRC/version/flag) - never read as Empty.
enum class Slot : uint8_t { Empty, Valid, Corrupt };
void encodeSentinel(const Sentinel&, uint8_t out[SENTINEL_LEN]);
Slot decodeSentinel(const uint8_t in[SENTINEL_LEN], Sentinel& out);
enum class SentinelRead : uint8_t { Absent, Ok, Unreadable };
// The current record (the valid one with the newer sequence, wrap-aware) and the sector to write next.
// No valid record but a damaged one: unreadable (fail closed), never absent.
SentinelRead chooseSentinel(Slot a, const Sentinel& ra, Slot b, const Sentinel& rb, Sentinel& out, uint8_t& next);
// The storage behind it (two sectors; the device uses raw flash, the test a simulation with power cuts).
struct SentinelFlash {
    virtual bool read(uint8_t sector, uint8_t out[SENTINEL_LEN]) = 0;
    virtual bool erase(uint8_t sector) = 0;
    virtual bool write(uint8_t sector, const uint8_t in[SENTINEL_LEN]) = 0;
    virtual ~SentinelFlash() {}
};
struct SentinelView {
    SentinelRead read = SentinelRead::Unreadable;
    Sentinel cur;
    uint8_t next = 0;
    Slot slot[2] = {Slot::Corrupt, Slot::Corrupt};
    bool redundant = false;   // both sectors hold cur's state (no older state left to fall back to)
};
SentinelView sentinelLoad(SentinelFlash&);
// A new state into BOTH sectors, the one holding the current record last, then verified. A power cut at any
// point leaves the old state or the new one; afterwards no other state remains to revive. Refuses when unreadable.
bool sentinelWrite(SentinelFlash&, bool owned, bool resetPending);
// The same, also over unreadable storage (only the PRG factory reset): a non-damaged sector first, so until
// that record is down the other stays damaged and the storage reads unreadable, never absent.
bool sentinelForce(SentinelFlash&, bool owned, bool resetPending);
// Records ownership (wizard step 1, the boot repair) and restores redundancy. Never clears a pending reset
// (Codex OWNER-13): refuses then, and when unreadable. Writes nothing when both sectors already say so.
bool sentinelOwn(SentinelFlash&);
// NVS says New, but the sentinel says the device has an owner (or cannot be read): Locked, never New.
State withSentinel(State nvs, SentinelRead r, const Sentinel& s);
uint32_t crc32(const uint8_t* p, size_t n);
bool resume(ResetOps&);    // at boot, before anything reads NVS: true = nothing pending or completed now;
                           // false = not done (also an unreadable marker): do not start normally
}  // namespace provision
