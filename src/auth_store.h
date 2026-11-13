// auth_store.h — the device side of the web login (ESP32 only; host tests provide fakes):
// the password record in NVS, PBKDF2-HMAC-SHA256 with a known-answer self-test, the hardware
// RNG, and the OTA key that authorises a password (re)set.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "web_auth.h"

namespace authstore {
bool load(webauth::Cred& out);          // false: no password stored (or unreadable)
bool save(const webauth::Cred& in);     // one NVS blob; only on (re)set and password change
bool kdfReady();                        // the PBKDF2 known-answer test passed (run once)
bool kdf(const char* pw, size_t n, const uint8_t* salt, size_t saltLen, uint32_t iterations, uint8_t out[32]);
void random(uint8_t* out, size_t n);
void ownMac(char out[18]);
bool verifySignature(const char* text, size_t n, const char* sigHex);
}
