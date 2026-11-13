// Web login logic: password policy, credential check, sessions, throttle, recovery code and
// the signed recovery request. The KDF and RNG are deterministic stand-ins (the device uses
// mbedTLS PBKDF2-HMAC-SHA256 with a known-answer self-test and the hardware RNG).
#include "web_auth.h"
#include <cstdio>
#include <cstring>
#include <string>
using namespace webauth;
static int fails = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static uint32_t g_seed = 0x12345678u;                   // xorshift32: deterministic, no short cycle
static void rng(uint8_t* b, size_t n) {
    for (size_t i = 0; i < n; ++i) { g_seed ^= g_seed << 13; g_seed ^= g_seed >> 17; g_seed ^= g_seed << 5; b[i] = uint8_t(g_seed >> 24); }
}
static int g_kdfCalls = 0;
static bool kdf(const char* pw, size_t n, const uint8_t* salt, size_t sn, uint32_t it, uint8_t out[32]) {
    ++g_kdfCalls;
    uint32_t h = 2166136261u ^ it;                      // FNV-style mixing: deterministic, not crypto
    for (int r = 0; r < 32; ++r) {
        for (size_t i = 0; i < n; ++i) h = (h ^ uint8_t(pw[i])) * 16777619u;
        for (size_t i = 0; i < sn; ++i) h = (h ^ salt[i]) * 16777619u;
        h = (h ^ uint32_t(r)) * 16777619u;
        out[r] = uint8_t(h >> 13);
    }
    return true;
}
static bool kdfFails(const char*, size_t, const uint8_t*, size_t, uint32_t, uint8_t[32]) { return false; }

int main() {
    // Password policy: 8..64 bytes, UTF-8 allowed, no control characters.
    CHECK(!passwordOk(nullptr) && !passwordOk("") && !passwordOk("1234567"));
    CHECK(passwordOk("12345678") && passwordOk("lange zin met spaties") && passwordOk("wachtwoörd!"));
    CHECK(!passwordOk(std::string(65, 'a').c_str()) && passwordOk(std::string(64, 'a').c_str()));
    CHECK(!passwordOk("tab\there!") && !passwordOk("del\x7fpass1"));
    // Credential: salted, verified in constant time, wrong or unusable input refused.
    Cred c{};
    CHECK(!makeCred("kort", kdf, rng, c));
    CHECK(makeCred("geheim-wachtwoord", kdf, rng, c) && credValid(c) && c.iterations == KDF_ITERATIONS);
    CHECK(verify(c, "geheim-wachtwoord", kdf));
    CHECK(!verify(c, "geheim-wachtwoorD", kdf) && !verify(c, "", kdf) && !verify(c, nullptr, kdf));
    CHECK(!verify(c, "geheim-wachtwoord", kdfFails));
    Cred c2{}; CHECK(makeCred("geheim-wachtwoord", kdf, rng, c2) && memcmp(c.salt, c2.salt, 16) && memcmp(c.hash, c2.hash, 32));
    Cred bad = c; bad.version = 9; CHECK(!credValid(bad) && !verify(bad, "geheim-wachtwoord", kdf));
    bad = c; bad.iterations = 10; CHECK(!credValid(bad));
    g_kdfCalls = 0; CHECK(!verify(c, std::string(200, 'x').c_str(), kdf) && g_kdfCalls == 0);   // no work for oversize
    CHECK(equal("abc", "abc") && !equal("abc", "abd") && !equal("abc", "abcd") && !equal(nullptr, "a"));
    // Sessions: 128-bit id and CSRF token, idle and absolute expiry, LRU eviction.
    Sessions s;
    const uint32_t T = 1000;
    const Session a = s.create(T, rng);
    CHECK(strlen(a.id) == 32 && strlen(a.csrf) == 32 && strcmp(a.id, a.csrf));
    CHECK(s.find(a.id, T + 1) && !s.find("0123", T) && !s.find(nullptr, T));
    CHECK(!s.find("00000000000000000000000000000000", T));
    CHECK(s.find(a.id, T + IDLE_MS - 1));                                // refreshed by use
    CHECK(s.find(a.id, T + IDLE_MS - 1 + IDLE_MS - 1));
    CHECK(!s.find(a.id, T + IDLE_MS - 1 + IDLE_MS - 1 + IDLE_MS));       // idle too long
    const Session b = s.create(T, rng);
    for (uint32_t t = T; t < T + LIFETIME_MS; t += IDLE_MS / 2) CHECK(s.find(b.id, t));
    CHECK(!s.find(b.id, T + LIFETIME_MS));                               // 30 days at most
    s.clear();
    std::string ids[SESSIONS + 1];
    for (size_t i = 0; i < SESSIONS; ++i) ids[i] = s.create(T + uint32_t(i), rng).id;
    CHECK(s.active(T + 10) == SESSIONS);
    CHECK(s.find(ids[0].c_str(), T + 20));                               // used: no longer the oldest
    ids[SESSIONS] = s.create(T + 30, rng).id;                            // evicts ids[1], the LRU
    CHECK(!s.find(ids[1].c_str(), T + 31) && s.find(ids[0].c_str(), T + 31) && s.find(ids[SESSIONS].c_str(), T + 31));
    s.remove(ids[0].c_str()); CHECK(!s.find(ids[0].c_str(), T + 32));
    const uint32_t W = 0xFFFFFF00u;                                      // millis() wrap
    s.clear(); const Session w = s.create(W, rng); CHECK(s.find(w.id, W + 0x200u));
    // Throttle: four free failures, then 30 s doubling up to 15 minutes; success resets.
    Throttle th;
    for (int i = 0; i < 4; ++i) { CHECK(th.waitMs(T) == 0); th.failure(T); }
    CHECK(th.waitMs(T) == 0);
    th.failure(T); CHECK(th.waitMs(T) == 30000 && th.waitMs(T + 29999) == 1 && th.waitMs(T + 30000) == 0);
    th.failure(T + 30000); CHECK(th.waitMs(T + 30000) == 60000);
    for (int i = 0; i < 10; ++i) th.failure(T + 100000);
    CHECK(th.waitMs(T + 100000) == MAX_LOCK_MS);
    th.success(); CHECK(th.waitMs(T + 100000) == 0 && th.failures() == 0);
    // millis() wraps after 2^32 ms (49.7 days). Codex SEC-02/SEC-03: an expired lock must not come
    // back (signed deadline arithmetic flips after ~24.8 days), and swept material stays gone.
    const uint32_t DAY = 86400000u;
    Throttle lk;
    for (int i = 0; i < 5; ++i) lk.failure(T);
    CHECK(lk.waitMs(T) == 30000);
    lk.sweep(T + DAY);                                                   // the main loop sweeps all the time
    CHECK(lk.waitMs(T + DAY) == 0 && lk.waitMs(T + 26 * DAY) == 0 && lk.waitMs(T + 0x80000000u + 1000) == 0);
    CHECK(lk.waitMs(T + 0xFFFFFFFFu) == 0 && lk.failures() == 5);        // the count stays: the next lock is 60 s
    lk.failure(T + 27 * DAY); CHECK(lk.waitMs(T + 27 * DAY) == 60000);
    Throttle unswept;                                                    // even without a sweep: never above the longest lock
    for (int i = 0; i < 5; ++i) unswept.failure(T);
    CHECK(unswept.waitMs(T + 26 * DAY) == 0);
    Sessions sw;
    const Session old = sw.create(T, rng);
    sw.sweep(T + IDLE_MS);                                               // idle a week: erased, not just "looks expired"
    CHECK(!sw.find(old.id, uint32_t(T + 0x100000001ull)) && sw.active(T + IDLE_MS) == 0);
    const Session live = sw.create(T, rng);
    sw.sweep(T + IDLE_MS - 1); CHECK(sw.find(live.id, T + IDLE_MS - 1));   // a used session is kept
    Recovery rs;
    std::string rn = rs.issueNonce(T, rng);
    rs.sweep(T + NONCE_TTL_MS);
    CHECK(!rs.takeNonce(rn.c_str(), uint32_t(T + 0x100000001ull)));        // 2^32 + 1 ms later: still dead
    std::string rc = rs.issueCode(T, rng);
    rs.sweep(T + CODE_TTL_MS);
    CHECK(rs.use(rc.c_str(), uint32_t(T + 0x100000001ull)) == Recovery::Check::None);
    rn = rs.issueNonce(T, rng); rc = rs.issueCode(T, rng);
    rs.sweep(T + 1000);                                                  // fresh material survives a sweep
    CHECK(rs.takeNonce(rn.c_str(), T + 2000) && rs.use(rc.c_str(), T + 2000) == Recovery::Check::Ok);
    // Recovery: one-time nonce with TTL, one-time code with 5 tries and 10 minutes.
    Recovery r;
    std::string n1 = r.issueNonce(T, rng);
    CHECK(n1.size() == 32 && r.takeNonce(n1.c_str(), T + 1) && !r.takeNonce(n1.c_str(), T + 2));
    n1 = r.issueNonce(T, rng); CHECK(!r.takeNonce(n1.c_str(), T + NONCE_TTL_MS));
    n1 = r.issueNonce(T, rng); CHECK(!r.takeNonce("ffffffffffffffffffffffffffffffff", T) && !r.takeNonce(n1.c_str(), T));
    CHECK(r.use("ABCD-EFGH", T) == Recovery::Check::None);
    std::string code = r.issueCode(T, rng);
    CHECK(code.size() == 9 && code[4] == '-');
    for (char ch : code) CHECK(ch == '-' || strchr("ABCDEFGHJKLMNPQRSTUVWXYZ23456789", ch));
    std::string lower = code; for (char& ch : lower) ch = char(tolower(ch));
    std::string bare = code.substr(0, 4) + " " + code.substr(5);
    CHECK(r.use("WRONG-CODE", T + 1) == Recovery::Check::Wrong);
    CHECK(r.use((code + "X").c_str(), T + 1) == Recovery::Check::Wrong);
    CHECK(r.use(lower.c_str(), T + 2) == Recovery::Check::Ok);           // case and dash do not matter
    CHECK(r.use(code.c_str(), T + 3) == Recovery::Check::None);         // one use
    code = r.issueCode(T, rng);
    CHECK(r.use(bare.c_str(), T) != Recovery::Check::Ok);
    code = r.issueCode(T, rng);
    for (int i = 0; i < 4; ++i) CHECK(r.use("AAAA-AAAA", T) == Recovery::Check::Wrong);
    CHECK(r.use("AAAA-AAAA", T) == Recovery::Check::Exhausted && r.use(code.c_str(), T) == Recovery::Check::None);
    code = r.issueCode(T, rng); CHECK(r.use(code.c_str(), T + CODE_TTL_MS) == Recovery::Check::Expired);
    CHECK(!strcmp(checkName(Recovery::Check::Exhausted), "code-exhausted"));
    // The signed recovery request: exact fields, this board, a fresh nonce (taken once).
    const char* MAC = "02:00:00:00:00:0a";
    auto req = [&](const std::string& nonce, const char* kind = "orcon-auth-reset", const char* mac = "02:00:00:00:00:0a", const char* extra = "") {
        return std::string("{\"v\":1,\"kind\":\"") + kind + "\",\"mac\":\"" + mac + "\",\"nonce\":\"" + nonce + "\"" + extra + "}";
    };
    std::string n = r.issueNonce(T, rng);
    CHECK(checkResetRequest(req(n).c_str(), MAC, r, T + 1) == nullptr);
    CHECK(!strcmp(checkResetRequest(req(n).c_str(), MAC, r, T + 2), "nonce-mismatch"));   // replay
    n = r.issueNonce(T, rng); CHECK(!strcmp(checkResetRequest(req(n, "orcon-ota").c_str(), MAC, r, T), "bad-request"));
    n = r.issueNonce(T, rng); CHECK(!strcmp(checkResetRequest(req(n, "orcon-auth-reset", "02:00:00:00:00:09").c_str(), MAC, r, T), "wrong-board"));
    n = r.issueNonce(T, rng); CHECK(checkResetRequest(req(n, "orcon-auth-reset", "02:00:00:00:00:0A").c_str(), MAC, r, T) == nullptr);
    n = r.issueNonce(T, rng); CHECK(!strcmp(checkResetRequest(req(n, "orcon-auth-reset", MAC, ",\"x\":1").c_str(), MAC, r, T), "bad-request"));
    n = r.issueNonce(T, rng);
    CHECK(!strcmp(checkResetRequest(("{\"v\":2,\"kind\":\"orcon-auth-reset\",\"mac\":\"02:00:00:00:00:0a\",\"nonce\":\"" + n + "\"}").c_str(), MAC, r, T), "bad-request"));
    CHECK(!strcmp(checkResetRequest("{\"v\":1}", MAC, r, T), "bad-request") && !strcmp(checkResetRequest("nope", MAC, r, T), "bad-request"));
    // An OTA manifest is never a recovery request (different fields), whatever its signature.
    CHECK(!strcmp(checkResetRequest("{\"v\":1,\"mac\":\"02:00:00:00:00:0a\",\"env\":\"x\",\"variant\":\"rx-only\",\"allow_tx\":false,"
                                    "\"commit\":\"0\",\"size\":1,\"sha256\":\"0\",\"nonce\":\"0\"}", MAC, r, T), "bad-request"));
    // Cookie header parsing.
    char sid[33];
    const std::string h = "0123456789abcdef0123456789abcdef";
    CHECK(cookieSid(("orcon_sid=" + h).c_str(), sid) && h == sid);
    CHECK(cookieSid(("theme=dark; orcon_sid=" + h + "; x=1").c_str(), sid) && h == sid);
    CHECK(!cookieSid("orcon_sid=short", sid) && !cookieSid("xorcon_sid=0123456789abcdef0123456789abcdef", sid));
    CHECK(!cookieSid(("orcon_sid=" + h + "00").c_str(), sid) && !cookieSid(nullptr, sid) && !cookieSid("", sid));
    CHECK(!cookieSid("orcon_sid=0123456789abcdef0123456789abcdeZ", sid));
    printf("web auth: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
