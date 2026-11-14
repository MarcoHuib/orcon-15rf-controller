// The owner's recovery key file (src/owner_key.*), with OpenSSL as the primitive provider and as
// the independent judge: OpenSSL must read our files, we must read OpenSSL's, and anything weaker
// (HMAC-SHA1 PRF, AES-128, too few iterations) or damaged is refused. The known-answer constants
// that the device self-test uses (owner_store.cpp) are checked here against OpenSSL too.
#include "owner_key.h"
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
using namespace ownerkey;
static int fails = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

// ---- OpenSSL primitives ---------------------------------------------------------------------------
static bool pbkdf2(const uint8_t* pw, size_t n, const uint8_t* s, size_t sn, uint32_t it, uint8_t out[32]) {
    return PKCS5_PBKDF2_HMAC((const char*)pw, int(n), s, int(sn), int(it), EVP_sha256(), 32, out) == 1;
}
static bool aes(bool encrypt, const uint8_t key[32], const uint8_t iv[16], const uint8_t* in, size_t n, uint8_t* out) {
    EVP_CIPHER_CTX* x = EVP_CIPHER_CTX_new();
    int a = 0, b = 0;
    bool ok = x && EVP_CipherInit_ex(x, EVP_aes_256_cbc(), nullptr, key, iv, encrypt) == 1 && EVP_CIPHER_CTX_set_padding(x, 0) == 1 &&
              EVP_CipherUpdate(x, out, &a, in, int(n)) == 1 && EVP_CipherFinal_ex(x, out + a, &b) == 1 && size_t(a + b) == n;
    EVP_CIPHER_CTX_free(x);
    return ok;
}
static void rnd(uint8_t* o, size_t n) { RAND_bytes(o, int(n)); }
static bool pub(const uint8_t d[32], uint8_t q[65]) {
    EC_GROUP* g = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
    BIGNUM* k = BN_bin2bn(d, 32, nullptr);
    EC_POINT* p = g ? EC_POINT_new(g) : nullptr;
    const bool ok = g && k && p && !BN_is_zero(k) && BN_cmp(k, EC_GROUP_get0_order(g)) < 0 &&
                    EC_POINT_mul(g, p, k, nullptr, nullptr, nullptr) == 1 &&
                    EC_POINT_point2oct(g, p, POINT_CONVERSION_UNCOMPRESSED, q, 65, nullptr) == 65;
    EC_POINT_free(p); BN_free(k); EC_GROUP_free(g);
    return ok;
}
static void sha(const uint8_t* in, size_t n, uint8_t out[32]) { unsigned l = 0; EVP_Digest(in, n, out, &l, EVP_sha256(), nullptr); }
static const Crypto C{pbkdf2, aes, rnd, pub, sha};

static std::string hexs(const uint8_t* b, size_t n) { std::string s; char t[3]; for (size_t i = 0; i < n; ++i) { snprintf(t, 3, "%02x", b[i]); s += t; } return s; }
static void unhex(const char* h, uint8_t* out) { for (size_t i = 0; h[2 * i]; ++i) sscanf(h + 2 * i, "%2hhx", &out[i]); }
static std::string sh(const std::string& cmd) {   // stdout of a shell command
    std::string out; char buf[4096];
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return out;
    size_t k;
    while ((k = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, k);
    pclose(f);
    return out;
}
static std::string slurp(const std::string& path) { return sh("cat '" + path + "'"); }
static void spit(const std::string& path, const std::string& s) { FILE* f = fopen(path.c_str(), "w"); fwrite(s.data(), 1, s.size(), f); fclose(f); }

int main() {
    // ---- known answers the device self-test relies on, checked against OpenSSL
    {   // NIST SP 800-38A F.2.5 CBC-AES256.Encrypt, block 1
        uint8_t k[32], iv[16], p[16], want[16], got[16];
        unhex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4", k);
        unhex("000102030405060708090a0b0c0d0e0f", iv); unhex("6bc1bee22e409f96e93d7e117393172a", p);
        unhex("f58c4c04d6e5f1ba779eabfb5f7bfbd6", want);
        CHECK(aes(true, k, iv, p, 16, got) && !memcmp(got, want, 16));
        CHECK(aes(false, k, iv, want, 16, got) && !memcmp(got, p, 16));
    }
    {   // P-256: 1*G is the generator; 0 and the group order are not private keys
        uint8_t d[32] = {0}, q[65], g[65];
        d[31] = 1;
        unhex("046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
              "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5", g);
        CHECK(pub(d, q) && !memcmp(q, g, 65));
        uint8_t z[32] = {0}, n[32];
        unhex("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551", n);
        CHECK(!pub(z, q) && !pub(n, q));
    }
    // ---- passphrase policy
    CHECK(!passphraseOk(nullptr) && !passphraseOk("kort-9tkn") && passphraseOk("tien-tekens"));
    CHECK(passphraseOk(std::string(64, 'x').c_str()) && !passphraseOk(std::string(65, 'x').c_str()));
    CHECK(!passphraseOk("met\nregeleinde!") && passphraseOk("spaties mogen ook"));

    // ---- round trip with a fixed key
    uint8_t d[32], q[65];
    unhex("c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721", d);   // RFC 6979 A.2.5 key
    CHECK(pub(d, q));
    const char* PASS = "mijn geheime zin 2026";
    char pem[PEM_MAX];
    CHECK(encryptPem(d, q, PASS, C, pem, sizeof(pem)));
    CHECK(strlen(pem) < PEM_MAX && strstr(pem, "-----BEGIN ENCRYPTED PRIVATE KEY-----\n") && strstr(pem, "-----END ENCRYPTED PRIVATE KEY-----\n"));
    char fp[15]; fingerprint(q, C, fp);
    CHECK(strlen(fp) == 14 && fp[4] == '-' && fp[9] == '-' && strstr(pem, fp));
    CHECK(!strstr(pem, hexs(d, 32).c_str()));                                   // the scalar is not in clear
    uint8_t d2[32], q2[65];
    CHECK(decryptPem(pem, PASS, C, d2, q2) == nullptr && !memcmp(d2, d, 32) && samePub(q2, q));
    char pem2[PEM_MAX];
    CHECK(encryptPem(d, q, PASS, C, pem2, sizeof(pem2)) && strcmp(pem, pem2));   // fresh salt and IV every time
    CHECK(!encryptPem(d, q, "te kort", C, pem2, sizeof(pem2)));
    CHECK(!encryptPem(d, q, PASS, C, pem2, 200));                               // never a truncated file
    // ---- refusals
    const char* e = decryptPem(pem, "mijn geheime zin 2027", C, d2, q2);
    CHECK(e && !strcmp(e, "bad-passphrase"));
    CHECK(!strcmp(decryptPem("geen sleutel", PASS, C, d2, q2), "bad-file"));
    CHECK(!strcmp(decryptPem("", PASS, C, d2, q2), "bad-file") && !strcmp(decryptPem(nullptr, PASS, C, d2, q2), "bad-file"));
    std::string cut(pem); cut.erase(cut.find("-----END") - 20, 10);
    CHECK(decryptPem(cut.c_str(), PASS, C, d2, q2) != nullptr);
    std::string bad(pem); bad[bad.find("-----BEGIN ENCRYPTED PRIVATE KEY-----\n") + 40] = '*';
    CHECK(!strcmp(decryptPem(bad.c_str(), PASS, C, d2, q2), "bad-file"));
    uint8_t q3[65]; uint8_t d3[32]; unhex("0000000000000000000000000000000000000000000000000000000000000002", d3); pub(d3, q3);
    CHECK(encryptPem(d, q3, PASS, C, pem2, sizeof(pem2)));                     // a file whose point is not d*G
    CHECK(!strcmp(decryptPem(pem2, PASS, C, d2, q2), "bad-key"));
    // Every single-character change in the body: an error, never a different key.
    {
        std::string body(pem);
        const size_t from = body.find("-----BEGIN ENCRYPTED PRIVATE KEY-----\n") + 38, to = body.find("-----END");
        int tried = 0, wrongOk = 0, sameOk = 0;
        for (size_t i = from; i < to; i += 3) {
            if (body[i] == '\n') continue;
            std::string m = body; m[i] = m[i] == 'A' ? 'B' : 'A';
            const char* r = decryptPem(m.c_str(), PASS, C, d2, q2);
            ++tried;
            if (!r) { if (memcmp(d2, d, 32)) ++wrongOk; else ++sameOk; }
        }
        CHECK(tried > 90 && wrongOk == 0);
        printf("  mutation sweep: %d single-character changes, %d still decrypt to the same key (padding bits), 0 to another\n", tried, sameOk);
    }
    // ---- interoperability with the openssl CLI (the independent implementation)
    const std::string ver = sh("openssl version 2>/dev/null");
    if (ver.find("OpenSSL") == std::string::npos) {
        printf("  openssl CLI interop: NOT TESTED (no openssl in PATH)\n");
    } else {
        char dir[] = "/tmp/orcon-ownerkey-XXXXXX";
        CHECK(mkdtemp(dir) != nullptr);
        const std::string D = dir;
        spit(D + "/ours.pem", pem);
        spit(D + "/pass", PASS);
        // OpenSSL reads our file and derives the same public key ...
        const std::string der = sh("openssl pkey -in " + D + "/ours.pem -passin file:" + D + "/pass -pubout -outform DER 2>/dev/null | xxd -p | tr -d '\\n'");
        CHECK(der.size() > 130 && der.substr(der.size() - 130) == hexs(q, 65));
        // ... and sees the parameters we promise: PBES2, PBKDF2 with HMAC-SHA256, 50000 rounds, AES-256-CBC.
        const std::string asn = sh("openssl asn1parse -in " + D + "/ours.pem 2>/dev/null");
        CHECK(asn.find("PBES2") != std::string::npos && asn.find("PBKDF2") != std::string::npos);
        CHECK(asn.find("hmacWithSHA256") != std::string::npos && asn.find("aes-256-cbc") != std::string::npos);
        CHECK(asn.find(":C350") != std::string::npos);                          // 50000
        // We read OpenSSL's own encrypted PKCS#8 (fresh key) and get its public key.
        sh("openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out " + D + "/plain.pem 2>/dev/null");
        const std::string want = sh("openssl pkey -in " + D + "/plain.pem -pubout -outform DER 2>/dev/null | xxd -p | tr -d '\\n'");
        auto conv = [&](const std::string& opts, const std::string& out) {
            sh("openssl pkcs8 -topk8 " + opts + " -in " + D + "/plain.pem -passout file:" + D + "/pass -out " + D + "/" + out + " 2>/dev/null");
            return slurp(D + "/" + out);
        };
        const std::string theirs = conv("-v2 aes-256-cbc -v2prf hmacWithSHA256 -iter 4096", "theirs.pem");
        CHECK(decryptPem(theirs.c_str(), PASS, C, d2, q2) == nullptr && want.size() > 130 && want.substr(want.size() - 130) == hexs(q2, 65));
        CHECK(!strcmp(decryptPem(theirs.c_str(), "verkeerde zin!", C, d2, q2), "bad-passphrase"));
        // Weaker or different parameters are refused as "not our format", before any work.
        CHECK(!strcmp(decryptPem(conv("-v2 aes-256-cbc -v2prf hmacWithSHA1 -iter 4096", "sha1.pem").c_str(), PASS, C, d2, q2), "bad-file"));
        CHECK(!strcmp(decryptPem(conv("-v2 aes-128-cbc -v2prf hmacWithSHA256 -iter 4096", "aes128.pem").c_str(), PASS, C, d2, q2), "bad-file"));
        CHECK(!strcmp(decryptPem(conv("-v2 aes-256-cbc -v2prf hmacWithSHA256 -iter 1000", "weak.pem").c_str(), PASS, C, d2, q2), "bad-file"));
        CHECK(!strcmp(decryptPem(slurp(D + "/plain.pem").c_str(), PASS, C, d2, q2), "bad-file"));   // unencrypted: refused
        // A non-P-256 key (P-384) is not an owner key.
        sh("openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-384 -out " + D + "/p384.pem 2>/dev/null");
        sh("openssl pkcs8 -topk8 -v2 aes-256-cbc -v2prf hmacWithSHA256 -iter 4096 -in " + D + "/p384.pem -passout file:" + D + "/pass -out " + D + "/p384e.pem 2>/dev/null");
        const char* r384 = decryptPem(slurp(D + "/p384e.pem").c_str(), PASS, C, d2, q2);
        CHECK(r384 && strcmp(r384, "bad-key"));                                 // bad-passphrase (shape) or bad-file: never accepted
        sh("rm -rf '" + D + "'");
        printf("  openssl CLI interop: %s", ver.c_str());
    }
    printf("owner key: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
