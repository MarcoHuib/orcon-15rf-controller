#include "ota_policy.h"
#include <ArduinoJson.h>
#include <string.h>
#include <ctype.h>

namespace otap {

const char* variantName(Variant v) {
    return v == Variant::RxOnly ? "rx-only" : v == Variant::RfTest ? "rf-test" : v == Variant::Control ? "control" : "none";
}

static bool hexOk(const char* s, size_t n) {
    if (strlen(s) != n) return false;
    for (size_t i = 0; i < n; ++i) if (!isxdigit((unsigned char)s[i])) return false;
    return true;
}
static uint8_t nib(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }

const char* parseManifest(const char* json, Manifest& m) {
    m = Manifest();
    static const char* const K1[] = {"v", "mac", "env", "variant", "allow_tx", "commit", "size", "sha256", "nonce"};
    static const char* const K2[] = {"v", "kind", "env", "variant", "allow_tx", "commit", "version", "size", "sha256"};
    constexpr size_t KN = sizeof(K1) / sizeof(K1[0]);
    static_assert(sizeof(K2) == sizeof(K1), "both forms have nine fields");
    JsonDocument d;
    if (!json || deserializeJson(d, json) || !d.is<JsonObject>()) return "bad-manifest";
    const uint8_t v = d["v"].is<uint8_t>() ? d["v"].as<uint8_t>() : 0;
    const char* const* K = v == 2 ? K2 : K1;      // anything but 2 is judged as v1 (checkManifest refuses v != 1)
    for (JsonPair kv : d.as<JsonObject>()) {
        bool known = false;
        for (size_t i = 0; i < KN; ++i) if (!strcmp(kv.key().c_str(), K[i])) known = true;
        if (!known) return "unknown-field";
    }
    if (d.as<JsonObject>().size() != KN) return "missing-field";
    if (!d["v"].is<uint8_t>() || !d["size"].is<uint32_t>() || !d["allow_tx"].is<bool>()) return "bad-manifest";
    for (const char* k : {"env", "variant", "commit", "sha256"}) if (!d[k].is<const char*>()) return "bad-manifest";
    m.v = d["v"]; m.size = d["size"]; m.allowTx = d["allow_tx"];
    const char* env = d["env"]; const char* var = d["variant"]; const char* commit = d["commit"]; const char* sha = d["sha256"];
    if (strlen(env) >= sizeof(m.env) || !hexOk(commit, 40) || !hexOk(sha, 64)) return "bad-manifest";
    if (m.v == 2) {
        if (!d["kind"].is<const char*>() || strcmp(d["kind"].as<const char*>(), "orcon-release") || !d["version"].is<uint32_t>()) return "bad-manifest";
        m.version = d["version"];
    } else {
        if (!d["mac"].is<const char*>() || !d["nonce"].is<const char*>()) return "bad-manifest";
        const char* mac = d["mac"]; const char* nonce = d["nonce"];
        if (strlen(mac) != 17 || !hexOk(nonce, 32)) return "bad-manifest";
        for (size_t i = 0; i < 17; ++i) m.mac[i] = (char)tolower((unsigned char)mac[i]);
        strcpy(m.nonce, nonce);
    }
    strcpy(m.env, env); strcpy(m.commit, commit);
    for (int i = 0; i < 32; ++i) m.sha256[i] = uint8_t(nib(sha[2 * i]) << 4 | nib(sha[2 * i + 1]));
    m.variant = !strcmp(var, "rx-only") ? Variant::RxOnly : !strcmp(var, "rf-test") ? Variant::RfTest :
                !strcmp(var, "control") ? Variant::Control : Variant::None;
    if (m.variant == Variant::None) return "variant-not-approved";
    return nullptr;
}

const char* checkManifest(const Manifest& m, const char* ownMac, const char* issuedNonce, uint32_t slotSize, uint32_t ownVersion) {
    if (m.v == 2) {
        if (m.version < ownVersion) return "older-release";
        if (m.variant == Variant::RfTest) return "variant-not-released";   // test images are never published
    } else if (m.v == 1) {
        char own[18] = "";
        if (!ownMac || strlen(ownMac) != 17) return "wrong-board";
        for (size_t i = 0; i < 17; ++i) own[i] = (char)tolower((unsigned char)ownMac[i]);
        if (strcmp(own, m.mac)) return "wrong-board";
        // One-time nonce, compared without an early exit.
        if (!issuedNonce || strlen(issuedNonce) != 32) return "nonce-required";
        uint8_t diff = 0;
        for (size_t i = 0; i < 32; ++i) diff |= uint8_t(issuedNonce[i] ^ m.nonce[i]);
        if (diff) return "nonce-mismatch";
    } else {
        return "manifest-version";
    }
    if (!m.size || m.size > slotSize) return "image-too-large";
    // Each approved variant has exactly one env and one TX statement; RX-only never says TX.
    if (m.variant == Variant::RxOnly && (strcmp(m.env, "heltec_v2_sx1276") || m.allowTx)) return "variant-mismatch";
    if (m.variant == Variant::RfTest && (strcmp(m.env, "heltec_v2_sx1276_rftest") || !m.allowTx)) return "variant-mismatch";
    if (m.variant == Variant::Control && (strcmp(m.env, "heltec_v2_sx1276_control") || !m.allowTx)) return "variant-mismatch";
    return nullptr;
}

// Patterns are stored XOR 0x5A: the plain strings must not appear in this image, or the
// flash/OTA marker checks would find the scanner itself (e.g. "txpoc" in an RX-only image).
static const uint8_t XK = 0x5A;
static const uint8_t P_RX[] = {0x09,0x02,0x6b,0x68,0x6d,0x6c,0x7a,0x3e,0x33,0x28,0x3f,0x39,0x2e,0x7a,0x08,0x02,0x7a,0x72,0x0e,0x02,0x7a,0x3e,0x33,0x29,0x3b,0x38,0x36,0x3f,0x3e,0x73};
static const uint8_t P_BANNER[] = {0x08,0x02,0x77,0x15,0x14,0x16,0x03,0x7a,0x0e,0x1f,0x09,0x0e,0x7a,0x1c,0x13,0x08,0x17,0x0d,0x1b,0x08,0x1f};
static const uint8_t P_RF[] = {0x09,0x02,0x6b,0x68,0x6d,0x6c,0x7a,0x3e,0x33,0x28,0x3f,0x39,0x2e,0x7a,0x08,0x02,0x7a,0x71,0x7a,0x2e,0x22,0x2a,0x35,0x39,0x7a,0x2a,0x3b,0x39,0x31,0x3f,0x2e,0x7a,0x0e,0x02,0x7a,0x72,0x08,0x1c,0x7a,0x0e,0x1f,0x09,0x0e,0x60,0x7a,0x37,0x3b,0x22,0x7a};
static const uint8_t P_CTL[] = {0x2e,0x22,0x2a,0x35,0x39,0x7a,0x2a,0x3b,0x39,0x31,0x3f,0x2e,0x7a,0x0e,0x02,0x7a,0x72,0x19,0x15,0x14,0x0e,0x08,0x15,0x16,0x60,0x7a};
static const uint8_t P_TX[] = {0x2e,0x22,0x2a,0x35,0x39,0x7a,0x2a,0x3b,0x39,0x31,0x3f,0x2e,0x7a,0x0e,0x02};
static const uint8_t P_DRY[] = {0x2e,0x22,0x77,0x2a,0x35,0x39,0x77,0x3e,0x28,0x23,0x28,0x2f,0x34};
static const uint8_t P_L0[] = {0x1c,0x33,0x28,0x37,0x2d,0x3b,0x28,0x3f,0x7a,0x2f,0x2a,0x3e,0x3b,0x2e,0x3f,0x7a,0x72,0x15,0x0e,0x1b,0x73};
static const uint8_t P_L1[] = {0x1b,0x28,0x3e,0x2f,0x33,0x34,0x35,0x15,0x0e,0x1b};
static const uint8_t P_L2[] = {0x3b,0x2f,0x2e,0x32,0x05,0x2f,0x2a,0x36,0x35,0x3b,0x3e};
static const uint8_t P_L3[] = {0x0f,0x2a,0x3e,0x3b,0x2e,0x3f,0x28,0x74,0x39,0x2a,0x2a};
struct Pat { const uint8_t* p; size_t n; };
#define PAT(x) Pat{x, sizeof(x)}
static const Pat LEGACY[] = {PAT(P_L0), PAT(P_L1), PAT(P_L2), PAT(P_L3)};

Scanner::Scanner(const char* commit) {
    strncpy(commit_, commit ? commit : "", sizeof(commit_) - 1); commit_[sizeof(commit_) - 1] = 0;
}
static bool has(const uint8_t* b, size_t n, const char* p) {
    const size_t k = strlen(p);
    if (!k || k > n) return false;
    for (size_t i = 0; i + k <= n; ++i) if (b[i] == (uint8_t)p[0] && !memcmp(b + i, p, k)) return true;
    return false;
}
static bool has(const uint8_t* b, size_t n, const Pat& x) {
    char p[64];                                   // decoded on the stack only while scanning
    if (x.n >= sizeof(p)) return false;
    for (size_t i = 0; i < x.n; ++i) p[i] = char(x.p[i] ^ XK);
    p[x.n] = 0;
    const bool r = has(b, n, p);
    memset(p, 0, sizeof(p));
    return r;
}
void Scanner::match(const uint8_t* b, size_t n) {
    rxRadio |= has(b, n, PAT(P_RX)); rxBanner |= has(b, n, PAT(P_BANNER)); rfTest |= has(b, n, PAT(P_RF));
    control |= has(b, n, PAT(P_CTL));
    txCode |= has(b, n, PAT(P_TX)); dryRun |= has(b, n, PAT(P_DRY)); signedOta |= has(b, n, SIGNED_OTA_MARKER);
    ownerModel |= has(b, n, OWNER_MODEL_MARKER);
    for (const Pat& p : LEGACY) legacyOta |= has(b, n, p);
    if (commit_[0]) commitStamp |= has(b, n, commit_);
}
void Scanner::feed(const uint8_t* data, size_t n) {
    // Search the carried tail plus a bounded window of new bytes, so patterns that straddle
    // chunk boundaries are found without buffering the image (the longest pattern < 64 B).
    uint8_t win[64 + 256];
    while (n) {
        const size_t take = n < 256 ? n : 256;
        memcpy(win, tail_, tailN_); memcpy(win + tailN_, data, take);
        const size_t len = tailN_ + take;
        match(win, len);
        tailN_ = len < sizeof(tail_) ? len : sizeof(tail_);
        memcpy(tail_, win + len - tailN_, tailN_);
        data += take; n -= take;
    }
}

const char* checkImage(const Manifest& m, const Scanner& s, uint32_t received, const uint8_t sha256[32]) {
    if (received != m.size) return "size-mismatch";
    uint8_t diff = 0;
    for (int i = 0; i < 32; ++i) diff |= uint8_t(sha256[i] ^ m.sha256[i]);
    if (diff) return "hash-mismatch";
    if (!s.commitStamp) return "commit-not-in-image";
    if (!s.signedOta) return "image-without-signed-ota";      // would take away the WiFi update path
    if (s.legacyOta) return "image-has-unauthenticated-ota";
    if (!s.ownerModel) return "image-predates-owner-model";     // its recovery would trust the release key again
    if (m.variant == Variant::RxOnly && !(s.rxRadio && s.rxBanner && !s.txCode && !s.rfTest && !s.control && !s.dryRun))
        return "image-not-rx-only";
    if (m.variant == Variant::RfTest && !(s.rfTest && s.txCode && !s.control && !s.rxRadio && !s.dryRun))
        return "image-not-rf-test";
    if (m.variant == Variant::Control && !(s.control && s.txCode && !s.rfTest && !s.rxRadio && !s.dryRun))
        return "image-not-control";
    return nullptr;
}

}  // namespace otap
