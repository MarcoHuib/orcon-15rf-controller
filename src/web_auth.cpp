#include "web_auth.h"
#include <ArduinoJson.h>
#include <ctype.h>
#include <string.h>

namespace webauth {

void hex(const uint8_t* b, size_t n, char* out) {
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) { out[2 * i] = H[b[i] >> 4]; out[2 * i + 1] = H[b[i] & 15]; }
    out[2 * n] = 0;
}
static bool hex32(const char* s) {
    if (!s || strlen(s) != 32) return false;
    for (size_t i = 0; i < 32; ++i) if (!isxdigit((unsigned char)s[i])) return false;
    return true;
}

bool equal(const char* a, const char* b) {
    if (!a || !b) return false;
    const size_t n = strlen(a);
    if (n != strlen(b)) return false;
    uint8_t d = 0;
    for (size_t i = 0; i < n; ++i) d |= uint8_t(a[i] ^ b[i]);
    return d == 0;
}

bool passwordOk(const char* pw) {
    if (!pw) return false;
    const size_t n = strlen(pw);
    if (n < PW_MIN || n > PW_MAX) return false;
    for (size_t i = 0; i < n; ++i) { const unsigned char c = (unsigned char)pw[i]; if (c < 0x20 || c == 0x7F) return false; }
    return true;
}
bool credValid(const Cred& c) { return c.version == CRED_VERSION && c.iterations >= 1000 && c.iterations <= 1000000; }

bool makeCred(const char* pw, Kdf kdf, Rng rng, Cred& out) {
    if (!passwordOk(pw) || !kdf || !rng) return false;
    Cred c{};
    c.version = CRED_VERSION; c.iterations = KDF_ITERATIONS;
    rng(c.salt, sizeof(c.salt));
    if (!kdf(pw, strlen(pw), c.salt, sizeof(c.salt), c.iterations, c.hash)) return false;
    out = c;
    return true;
}
bool verify(const Cred& c, const char* pw, Kdf kdf) {
    if (!credValid(c) || !pw || !kdf) return false;
    const size_t n = strlen(pw);
    if (n > PW_MAX) return false;                       // bounded work per attempt
    uint8_t h[32];
    if (!kdf(pw, n, c.salt, sizeof(c.salt), c.iterations, h)) return false;
    uint8_t d = 0;
    for (size_t i = 0; i < sizeof(h); ++i) d |= uint8_t(h[i] ^ c.hash[i]);
    memset(h, 0, sizeof(h));
    return d == 0;
}

// ---- sessions ----------------------------------------------------------------------------
bool Sessions::expired(const Session& s, uint32_t now) const {
    return !s.used || uint32_t(now - s.last) >= IDLE_MS || uint32_t(now - s.created) >= LIFETIME_MS;
}
const Session& Sessions::create(uint32_t now, Rng rng) {
    Session* slot = nullptr;
    for (Session& s : s_) if (expired(s, now)) { slot = &s; break; }
    if (!slot) {                                        // all in use: drop the least recently used
        slot = &s_[0];
        for (Session& s : s_) if (uint32_t(now - s.last) > uint32_t(now - slot->last)) slot = &s;
    }
    uint8_t b[32];
    rng(b, sizeof(b));
    Session n;
    hex(b, 16, n.id); hex(b + 16, 16, n.csrf);
    n.created = n.last = now; n.used = true;
    memset(b, 0, sizeof(b));
    *slot = n;
    return *slot;
}
Session* Sessions::find(const char* id, uint32_t now) {
    if (!hex32(id)) return nullptr;
    for (Session& s : s_) {
        if (!s.used || !equal(s.id, id)) continue;
        if (expired(s, now)) { s = Session(); return nullptr; }
        s.last = now;
        return &s;
    }
    return nullptr;
}
void Sessions::remove(const char* id) { for (Session& s : s_) if (s.used && equal(s.id, id)) s = Session(); }
void Sessions::clear() { for (Session& s : s_) s = Session(); }
void Sessions::sweep(uint32_t now) { for (Session& s : s_) if (s.used && expired(s, now)) s = Session(); }
size_t Sessions::active(uint32_t now) const {
    size_t n = 0;
    for (const Session& s : s_) if (!expired(s, now)) ++n;
    return n;
}

// ---- login throttle -------------------------------------------------------------------------
uint32_t Throttle::waitMs(uint32_t now) const {
    if (!locked_) return 0;
    const int32_t d = int32_t(until_ - now);
    return d > 0 && uint32_t(d) <= MAX_LOCK_MS ? uint32_t(d) : 0;   // never more than the longest lock
}
void Throttle::failure(uint32_t now) {
    if (fails_ < 255) ++fails_;
    if (fails_ <= FREE_FAILURES) return;
    uint32_t lock = 30000;
    for (uint8_t i = FREE_FAILURES + 1; i < fails_ && lock < MAX_LOCK_MS; ++i) lock *= 2;
    until_ = now + (lock > MAX_LOCK_MS ? MAX_LOCK_MS : lock);
    locked_ = true;
}
void Throttle::sweep(uint32_t now) { if (locked_ && !waitMs(now)) locked_ = false; }

// ---- recovery --------------------------------------------------------------------------------
static const char ALPHABET[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";   // 32 symbols, no 0/O/1/I
const char* Recovery::issueNonce(uint32_t now, Rng rng) {
    uint8_t b[16];
    rng(b, sizeof(b));
    hex(b, sizeof(b), nonce_);
    nonceMs_ = now;
    return nonce_;
}
bool Recovery::takeNonce(const char* n, uint32_t now) {
    const bool ok = nonce_[0] && uint32_t(now - nonceMs_) < NONCE_TTL_MS && n && equal(n, nonce_);
    nonce_[0] = 0;                                      // one attempt per nonce, valid or not
    return ok;
}
const char* Recovery::issueCode(uint32_t now, Rng rng) {
    uint8_t b[8];
    rng(b, sizeof(b));
    for (int i = 0; i < 8; ++i) code_[i] = ALPHABET[b[i] & 31];
    code_[8] = 0;
    codeMs_ = now; tries_ = 0;
    static char shown[10];
    memcpy(shown, code_, 4); shown[4] = '-'; memcpy(shown + 5, code_ + 4, 4); shown[9] = 0;
    return shown;
}
Recovery::Check Recovery::use(const char* in, uint32_t now) {
    if (!code_[0]) return Check::None;
    if (uint32_t(now - codeMs_) >= CODE_TTL_MS) { code_[0] = 0; return Check::Expired; }
    char norm[9] = "";
    size_t k = 0;
    bool fits = in != nullptr;
    for (const char* p = in; fits && *p; ++p) {
        if (*p == '-' || *p == ' ') continue;
        if (k == 8) { fits = false; break; }
        norm[k++] = (char)toupper((unsigned char)*p);
    }
    norm[k] = 0;
    if (fits && k == 8 && equal(norm, code_)) { code_[0] = 0; return Check::Ok; }
    if (++tries_ >= CODE_TRIES) { code_[0] = 0; return Check::Exhausted; }
    return Check::Wrong;
}
void Recovery::clear() { memset(nonce_, 0, sizeof(nonce_)); memset(code_, 0, sizeof(code_)); tries_ = 0; }
void Recovery::sweep(uint32_t now) {
    if (nonce_[0] && uint32_t(now - nonceMs_) >= NONCE_TTL_MS) nonce_[0] = 0;
    if (code_[0] && uint32_t(now - codeMs_) >= CODE_TTL_MS) memset(code_, 0, sizeof(code_));
}
const char* checkName(Recovery::Check c) {
    switch (c) {
        case Recovery::Check::Ok: return "ok";
        case Recovery::Check::None: return "code-required";
        case Recovery::Check::Expired: return "code-expired";
        case Recovery::Check::Wrong: return "bad-code";
        case Recovery::Check::Exhausted: return "code-exhausted";
    }
    return "bad-code";
}

const char* checkResetRequest(const char* json, const char* ownMac, Recovery& r, uint32_t now) {
    JsonDocument d;
    if (!json || deserializeJson(d, json) || !d.is<JsonObject>()) return "bad-request";
    static const char* const K[] = {"v", "kind", "mac", "nonce"};
    JsonObject o = d.as<JsonObject>();
    if (o.size() != sizeof(K) / sizeof(K[0])) return "bad-request";
    for (JsonPair kv : o) {
        bool known = false;
        for (const char* k : K) if (!strcmp(kv.key().c_str(), k)) known = true;
        if (!known) return "bad-request";
    }
    if (!d["v"].is<int>() || d["v"].as<int>() != 1) return "bad-request";
    if (!d["kind"].is<const char*>() || !d["mac"].is<const char*>() || !d["nonce"].is<const char*>()) return "bad-request";
    if (strcmp(d["kind"].as<const char*>(), "orcon-auth-reset")) return "bad-request";
    const char* mac = d["mac"];
    if (!ownMac || strlen(mac) != 17 || strlen(ownMac) != 17) return "wrong-board";
    for (size_t i = 0; i < 17; ++i)
        if (tolower((unsigned char)mac[i]) != tolower((unsigned char)ownMac[i])) return "wrong-board";
    if (!r.takeNonce(d["nonce"].as<const char*>(), now)) return "nonce-mismatch";
    return nullptr;
}

bool cookieSid(const char* h, char out[33]) {
    out[0] = 0;
    if (!h) return false;
    static const char NAME[] = "orcon_sid=";
    for (const char* p = h; *p;) {
        while (*p == ' ' || *p == ';') ++p;
        if (!strncmp(p, NAME, sizeof(NAME) - 1)) {
            p += sizeof(NAME) - 1;
            size_t n = 0;
            while (p[n] && p[n] != ';' && p[n] != ' ') ++n;
            if (n != 32) return false;
            memcpy(out, p, 32); out[32] = 0;
            if (!hex32(out)) { out[0] = 0; return false; }
            return true;
        }
        while (*p && *p != ';') ++p;
    }
    return false;
}

}  // namespace webauth
