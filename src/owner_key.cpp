#include "owner_key.h"
#include <stdio.h>
#include <string.h>

namespace ownerkey {

void wipe(void* p, size_t n) { volatile uint8_t* v = static_cast<volatile uint8_t*>(p); while (n--) *v++ = 0; }

bool passphraseOk(const char* p) {
    if (!p) return false;
    const size_t n = strlen(p);
    if (n < PHRASE_MIN || n > PHRASE_MAX) return false;
    for (size_t i = 0; i < n; ++i) { const unsigned char c = (unsigned char)p[i]; if (c < 0x20 || c == 0x7F) return false; }
    return true;
}
bool samePub(const uint8_t a[65], const uint8_t b[65]) {
    uint8_t d = 0;
    for (size_t i = 0; i < PUB_LEN; ++i) d |= uint8_t(a[i] ^ b[i]);
    return d == 0;
}

// ---- the fixed DER pieces --------------------------------------------------------------------
static const uint8_t OID_PBES2[] = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x05,0x0d};       // 1.2.840.113549.1.5.13
static const uint8_t OID_PBKDF2[] = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x05,0x0c};      // 1.2.840.113549.1.5.12
static const uint8_t OID_HMAC_SHA256[] = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x02,0x09};      // 1.2.840.113549.2.9
static const uint8_t OID_AES256_CBC[] = {0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x01,0x2a};  // aes256-CBC (NIST)
// PrivateKeyInfo for a P-256 key exactly as OpenSSL writes it: version 0, {ecPublicKey,
// prime256v1}, ECPrivateKey {1, d (32 B), [1] publicKey}. d at offset 36, the point at 73.
static const uint8_t PKI_HEAD[36] = {0x30,0x81,0x87,0x02,0x01,0x00,0x30,0x13,0x06,0x07,0x2a,0x86,0x48,0xce,0x3d,0x02,0x01,
                                     0x06,0x08,0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07,0x04,0x6d,0x30,0x6b,0x02,0x01,0x01,0x04,0x20};
static const uint8_t PKI_MID[5] = {0xa1,0x44,0x03,0x42,0x00};
constexpr size_t PKI_D = 36, PKI_Q = 73, PKI_LEN = 138, SALT_LEN = 16, CT_MAX = 256, DER_MAX = 512;
static const char BEGIN[] = "-----BEGIN ENCRYPTED PRIVATE KEY-----", END[] = "-----END ENCRYPTED PRIVATE KEY-----";

// ---- DER writer: content first, then wrapped (every piece here is < 64 KiB) ------------------
struct W { uint8_t* p; size_t cap, n; bool ok; };
static void put(W& w, const void* b, size_t k) {
    if (!w.ok || k > w.cap - w.n) { w.ok = false; return; }
    memcpy(w.p + w.n, b, k); w.n += k;
}
static void tlv(W& w, uint8_t tag, const void* c, size_t k) {
    uint8_t h[4] = {tag, 0, 0, 0};
    size_t hn = 2;
    if (k < 128) h[1] = uint8_t(k);
    else if (k < 256) { h[1] = 0x81; h[2] = uint8_t(k); hn = 3; }
    else { h[1] = 0x82; h[2] = uint8_t(k >> 8); h[3] = uint8_t(k); hn = 4; }
    put(w, h, hn); put(w, c, k);
}
// Positive INTEGER, minimal: a leading 00 only when the top bit is set.
static size_t intBytes(uint32_t v, uint8_t out[5]) {
    uint8_t b[5] = {0, uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
    size_t i = 1;
    while (i < 4 && b[i] == 0) ++i;
    if (b[i] & 0x80) --i;
    memcpy(out, b + i, 5 - i);
    return 5 - i;
}

// ---- DER reader: strict (definite, minimal lengths), one expected tag at a time ---------------
struct R { const uint8_t* p; size_t n; };
static bool get(R& r, uint8_t tag, R& c) {
    if (r.n < 2 || r.p[0] != tag) return false;
    size_t len = r.p[1], hn = 2;
    if (len == 0x81) { if (r.n < 3 || r.p[2] < 0x80) return false; len = r.p[2]; hn = 3; }
    else if (len == 0x82) { if (r.n < 4) return false; len = size_t(r.p[2]) << 8 | r.p[3]; if (len < 0x100) return false; hn = 4; }
    else if (len & 0x80) return false;
    if (r.n - hn < len) return false;
    c.p = r.p + hn; c.n = len;
    r.p += hn + len; r.n -= hn + len;
    return true;
}
static bool oid(R& r, const uint8_t* o, size_t n) { R c; return get(r, 0x06, c) && c.n == n && !memcmp(c.p, o, n); }
static bool uint(R& r, uint32_t& v) {
    R c;
    if (!get(r, 0x02, c) || !c.n || c.n > 5 || (c.p[0] & 0x80)) return false;   // positive only
    if (c.n > 1 && c.p[0] == 0 && !(c.p[1] & 0x80)) return false;                // minimal
    if (c.n == 5 && c.p[0] != 0) return false;
    v = 0;
    for (size_t i = 0; i < c.n; ++i) v = v << 8 | c.p[i];
    return true;
}

// ---- base64 (RFC 4648, standard alphabet) ----------------------------------------------------
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int b64v(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}
static bool b64dec(const char* s, size_t n, uint8_t* out, size_t cap, size_t& len) {
    uint32_t acc = 0;
    int bits = 0;
    size_t syms = 0, pad = 0, k = 0;
    for (size_t i = 0; i < n; ++i) {
        const char ch = s[i];
        if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') continue;
        if (ch == '=') { ++pad; continue; }
        const int v = b64v(ch);
        if (v < 0 || pad) return false;                  // foreign character, or data after padding
        acc = acc << 6 | uint32_t(v); bits += 6; ++syms;
        if (bits >= 8) { bits -= 8; if (k == cap) return false; out[k++] = uint8_t(acc >> bits); }
    }
    if (pad > 2 || (syms + pad) % 4 || (acc & ((1u << bits) - 1))) return false;
    len = k;
    return true;
}

bool encryptPem(const uint8_t d[32], const uint8_t q[65], const char* pass, const Crypto& c, char* out, size_t cap) {
    if (!passphraseOk(pass) || !d || !q || q[0] != 0x04 || !out) return false;
    uint8_t pki[PKI_LEN + 16], salt[SALT_LEN], iv[16], key[32], ct[PKI_LEN + 16];
    memcpy(pki, PKI_HEAD, sizeof(PKI_HEAD)); memcpy(pki + PKI_D, d, 32);
    memcpy(pki + 68, PKI_MID, sizeof(PKI_MID)); memcpy(pki + PKI_Q, q, 65);
    const size_t padN = 16 - PKI_LEN % 16;                  // PKCS#7: 6 bytes of 0x06 here
    memset(pki + PKI_LEN, int(padN), padN);
    const size_t ctN = PKI_LEN + padN;
    c.random(salt, sizeof(salt)); c.random(iv, sizeof(iv));
    bool ok = c.pbkdf2((const uint8_t*)pass, strlen(pass), salt, sizeof(salt), KDF_ITERATIONS, key) &&
              c.aes256cbc(true, key, iv, pki, ctN, ct);
    wipe(key, sizeof(key)); wipe(pki, sizeof(pki));
    if (!ok) return false;
    // EncryptedPrivateKeyInfo { {PBES2, { {PBKDF2, {salt, iterations, {hmacWithSHA256, NULL}}}, {aes256-CBC, iv} }}, ct }
    uint8_t b1[64], b2[64], b3[96], b4[96], b5[128], der[DER_MAX], it[5];
    const uint8_t NUL[1] = {0};
    W prf{b1, sizeof(b1), 0, true}; tlv(prf, 0x06, OID_HMAC_SHA256, sizeof(OID_HMAC_SHA256)); tlv(prf, 0x05, NUL, 0);
    W kp{b2, sizeof(b2), 0, true}; tlv(kp, 0x04, salt, sizeof(salt)); tlv(kp, 0x02, it, intBytes(KDF_ITERATIONS, it)); tlv(kp, 0x30, b1, prf.n);
    W kdf{b3, sizeof(b3), 0, true}; tlv(kdf, 0x06, OID_PBKDF2, sizeof(OID_PBKDF2)); tlv(kdf, 0x30, b2, kp.n);
    W enc{b4, sizeof(b4), 0, true}; tlv(enc, 0x06, OID_AES256_CBC, sizeof(OID_AES256_CBC)); tlv(enc, 0x04, iv, sizeof(iv));
    W pb{b5, sizeof(b5), 0, true}; tlv(pb, 0x30, b3, kdf.n); tlv(pb, 0x30, b4, enc.n);
    uint8_t b6[160]; W alg{b6, sizeof(b6), 0, true}; tlv(alg, 0x06, OID_PBES2, sizeof(OID_PBES2)); tlv(alg, 0x30, b5, pb.n);
    uint8_t b7[DER_MAX]; W body{b7, sizeof(b7), 0, true}; tlv(body, 0x30, b6, alg.n); tlv(body, 0x04, ct, ctN);
    W all{der, sizeof(der), 0, true}; tlv(all, 0x30, b7, body.n);
    if (!(prf.ok && kp.ok && kdf.ok && enc.ok && pb.ok && alg.ok && body.ok && all.ok)) return false;
    char fp[15]; fingerprint(q, c, fp);
    int n = snprintf(out, cap, "Orcon 15RF herstelsleutel (eigenaarssleutel)\nVingerafdruk: %s\n"
                               "Versleuteld met je wachtwoordzin. Bewaar dit bestand en de wachtwoordzin apart en veilig.\n%s\n",
                     fp, BEGIN);
    if (n < 0 || size_t(n) >= cap) return false;
    size_t o = size_t(n), col = 0;
    for (size_t i = 0; i < all.n; i += 3) {                 // base64, 64 characters per line
        const uint32_t v = uint32_t(der[i]) << 16 | (i + 1 < all.n ? der[i + 1] << 8 : 0) | (i + 2 < all.n ? der[i + 2] : 0);
        const char q4[4] = {B64[v >> 18 & 63], B64[v >> 12 & 63], i + 1 < all.n ? B64[v >> 6 & 63] : '=', i + 2 < all.n ? B64[v & 63] : '='};
        if (o + 6 >= cap) return false;
        memcpy(out + o, q4, 4); o += 4; col += 4;
        if (col == 64) { out[o++] = '\n'; col = 0; }
    }
    if (col) out[o++] = '\n';
    n = snprintf(out + o, cap - o, "%s\n", END);
    return n > 0 && size_t(n) < cap - o;
}

const char* decryptPem(const char* pem, const char* pass, const Crypto& c, uint8_t d[32], uint8_t q[65]) {
    if (!pem || !pass) return "bad-file";
    const char* b = strstr(pem, BEGIN);
    const char* e = b ? strstr(b, END) : nullptr;
    uint8_t der[DER_MAX];
    size_t derN = 0;
    if (!e || !b64dec(b + strlen(BEGIN), size_t(e - b) - strlen(BEGIN), der, sizeof(der), derN)) return "bad-file";
    R all{der, derN}, epki, alg, pbes, kdf, kp, prf, enc, ct, salt, iv;
    uint32_t iter = 0;
    if (!get(all, 0x30, epki) || all.n || !get(epki, 0x30, alg) || !get(epki, 0x04, ct) || epki.n) return "bad-file";
    if (!oid(alg, OID_PBES2, sizeof(OID_PBES2)) || !get(alg, 0x30, pbes) || alg.n) return "bad-file";
    if (!get(pbes, 0x30, kdf) || !get(pbes, 0x30, enc) || pbes.n) return "bad-file";
    if (!oid(kdf, OID_PBKDF2, sizeof(OID_PBKDF2)) || !get(kdf, 0x30, kp) || kdf.n) return "bad-file";
    if (!get(kp, 0x04, salt) || salt.n < 8 || salt.n > 64 || !uint(kp, iter)) return "bad-file";
    if (kp.n && kp.p[0] == 0x02) { uint32_t kl = 0; if (!uint(kp, kl) || kl != 32) return "bad-file"; }   // optional keyLength
    // The PRF is required: when absent PBKDF2 means HMAC-SHA1, which we never write or accept.
    if (!get(kp, 0x30, prf) || kp.n || !oid(prf, OID_HMAC_SHA256, sizeof(OID_HMAC_SHA256))) return "bad-file";
    if (prf.n) { R nul; if (!get(prf, 0x05, nul) || nul.n || prf.n) return "bad-file"; }
    if (!oid(enc, OID_AES256_CBC, sizeof(OID_AES256_CBC)) || !get(enc, 0x04, iv) || iv.n != 16 || enc.n) return "bad-file";
    if (!ct.n || ct.n % 16 || ct.n > CT_MAX || iter < ITER_MIN || iter > ITER_MAX) return "bad-file";
    uint8_t key[32], pt[CT_MAX];
    const bool ok = c.pbkdf2((const uint8_t*)pass, strlen(pass), salt.p, salt.n, iter, key) && c.aes256cbc(false, key, iv.p, ct.p, ct.n, pt);
    wipe(key, sizeof(key));
    if (!ok) { wipe(pt, sizeof(pt)); return "crypto-unavailable"; }
    // A wrong passphrase decrypts to noise: bad padding or no PrivateKeyInfo. Both read the same.
    const uint8_t padN = pt[ct.n - 1];
    bool good = padN >= 1 && padN <= 16 && ct.n - padN == PKI_LEN;
    for (size_t i = 0; good && i < padN; ++i) good = pt[ct.n - 1 - i] == padN;
    good = good && !memcmp(pt, PKI_HEAD, sizeof(PKI_HEAD)) && !memcmp(pt + 68, PKI_MID, sizeof(PKI_MID)) && pt[PKI_Q] == 0x04;
    if (!good) { wipe(pt, sizeof(pt)); return "bad-passphrase"; }
    uint8_t mine[65];
    const bool keyOk = c.publicKey(pt + PKI_D, mine) && samePub(mine, pt + PKI_Q);   // never trust the stored point
    if (keyOk) { memcpy(d, pt + PKI_D, 32); memcpy(q, mine, 65); }
    wipe(pt, sizeof(pt));
    return keyOk ? nullptr : "bad-key";
}

void fingerprint(const uint8_t q[65], const Crypto& c, char out[15]) {
    uint8_t h[32];
    c.sha256(q, PUB_LEN, h);
    snprintf(out, 15, "%02x%02x-%02x%02x-%02x%02x", h[0], h[1], h[2], h[3], h[4], h[5]);
}

}  // namespace ownerkey
