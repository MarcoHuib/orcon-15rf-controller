// Web login: password record, sessions with a CSRF token and the login throttle. Pure logic
// (host-tested). Crypto (PBKDF2, random), NVS and HTTP are injected or live in the glue
// (auth_store.cpp, setup_web.cpp).
//
// There is no default password. The first one is set in the installation wizard; a forgotten one
// is reset with the owner's own recovery key in the PRG recovery mode (provision.h). The firmware
// signing key never sets or resets a password.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace webauth {
constexpr size_t PW_MIN = 8, PW_MAX = 64;
constexpr uint32_t KDF_ITERATIONS = 10000;               // PBKDF2-HMAC-SHA256
constexpr size_t SESSIONS = 4;
constexpr uint32_t IDLE_MS = 7u * 24 * 3600 * 1000;      // unused for a week: log in again
constexpr uint32_t LIFETIME_MS = 30u * 24 * 3600 * 1000; // at most 30 days per login
constexpr uint8_t FREE_FAILURES = 4;                     // then 30 s, 60 s, ... up to 15 min
constexpr uint32_t MAX_LOCK_MS = 900000;

using Kdf = bool (*)(const char* pw, size_t pwLen, const uint8_t* salt, size_t saltLen, uint32_t iterations, uint8_t out[32]);
using Rng = void (*)(uint8_t* out, size_t n);

// Stored in NVS as one blob. Never logged, never sent.
struct Cred { uint8_t version; uint8_t pad[3]; uint32_t iterations; uint8_t salt[16]; uint8_t hash[32]; };
constexpr uint8_t CRED_VERSION = 1;

bool passwordOk(const char* pw);                          // 8..64 bytes, no control characters
bool credValid(const Cred&);
bool makeCred(const char* pw, Kdf, Rng, Cred& out);
bool verify(const Cred&, const char* pw, Kdf);            // constant-time comparison
bool equal(const char* a, const char* b);                 // constant time for equal lengths
bool siriKeyValid(const char* k);                         // the Siri key format: exactly 32 lowercase hex characters

struct Session { char id[33] = ""; char csrf[33] = ""; uint32_t created = 0, last = 0; bool used = false; };
class Sessions {
public:
    const Session& create(uint32_t now, Rng);             // evicts the least recently used when full
    Session* find(const char* id, uint32_t now);          // valid + not expired; refreshes "last"
    void remove(const char* id);
    void clear();
    void sweep(uint32_t now);                              // drop expired sessions (call often: see below)
    size_t active(uint32_t now) const;
private:
    bool expired(const Session&, uint32_t now) const;
    Session s_[SESSIONS];
};

// All times are 32-bit millis(), which wraps after 49.7 days: an age check alone would revive
// expired material then. sweep() erases it once expired; the main loop calls every sweep()
// continuously (setupweb::tick), so nothing survives anywhere near a wrap.
// Global, not per address: addresses are cheap on a LAN.
class Throttle {
public:
    uint32_t waitMs(uint32_t now) const;                  // 0: an attempt may be made now
    void failure(uint32_t now);
    void success() { fails_ = 0; locked_ = false; }
    void sweep(uint32_t now);                             // an expired lock is gone for good
    uint8_t failures() const { return fails_; }
private:
    uint8_t fails_ = 0;
    bool locked_ = false;
    uint32_t until_ = 0;
};

bool cookieSid(const char* cookieHeader, char out[33]);   // the orcon_sid value (32 hex)
void hex(const uint8_t* b, size_t n, char* out);           // out: 2n + 1 bytes
}
