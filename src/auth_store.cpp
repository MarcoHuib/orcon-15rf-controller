#include "config.h"
#if ORCON_WEB_AUTH
#include "auth_store.h"
#include "ota_signed.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <string.h>

namespace authstore {

static const char NS[] = "orconauth", KEY[] = "cred";

bool load(webauth::Cred& out) {
    Preferences p;
    if (!p.begin(NS, true)) return false;            // read-only: a missing namespace is "no password"
    webauth::Cred c{};
    const bool ok = p.isKey(KEY) && p.getBytesLength(KEY) == sizeof(c) && p.getBytes(KEY, &c, sizeof(c)) == sizeof(c);
    p.end();
    if (!ok || !webauth::credValid(c)) return false;
    out = c;
    return true;
}
bool save(const webauth::Cred& in) {
    Preferences p;
    if (!p.begin(NS, false)) return false;
    const bool ok = p.putBytes(KEY, &in, sizeof(in)) == sizeof(in);
    p.end();
    webauth::Cred back{};
    return ok && load(back) && !memcmp(&back, &in, sizeof(in));   // read back what was written
}

static bool pbkdf2(const uint8_t* pw, size_t n, const uint8_t* salt, size_t saltLen, uint32_t it, uint8_t out[32]) {
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    bool ok = info && mbedtls_md_setup(&ctx, info, 1) == 0 &&
              mbedtls_pkcs5_pbkdf2_hmac(&ctx, pw, n, salt, saltLen, it, 32, out) == 0;
    mbedtls_md_free(&ctx);
    return ok;
}
bool kdfReady() {
    // PBKDF2-HMAC-SHA256("password", "salt", c=2, 32 B): the published test vector.
    static int state = 0;                            // 0 unknown, 1 ok, -1 failed (fail closed)
    if (!state) {
        static const uint8_t WANT[32] = {0xae,0x4d,0x0c,0x95,0xaf,0x6b,0x46,0xd3,0x2d,0x0a,0xdf,0xf9,0x28,0xf0,0x6d,0xd0,
                                         0x2a,0x30,0x3f,0x8e,0xf3,0xc2,0x51,0xdf,0xd6,0xe2,0xd8,0x5a,0x95,0x47,0x4c,0x43};
        uint8_t got[32];
        state = pbkdf2((const uint8_t*)"password", 8, (const uint8_t*)"salt", 4, 2, got) && !memcmp(got, WANT, 32) ? 1 : -1;
    }
    return state == 1;
}
bool kdf(const char* pw, size_t n, const uint8_t* salt, size_t saltLen, uint32_t it, uint8_t out[32]) {
    return kdfReady() && pbkdf2((const uint8_t*)pw, n, salt, saltLen, it, out);
}
void random(uint8_t* out, size_t n) { esp_fill_random(out, n); }   // hardware RNG (RF/WiFi on)
void ownMac(char out[18]) { ota::ownMac(out); }
bool verifySignature(const char* text, size_t n, const char* sigHex) { return ota::verifySignature(text, n, sigHex); }

}  // namespace authstore
#endif
