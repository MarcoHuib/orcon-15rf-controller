// auth_store.h — the device side of the web login (ESP32 only; host tests provide fakes):
// the password record in NVS, PBKDF2-HMAC-SHA256 with a known-answer self-test and the hardware RNG.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "web_auth.h"

namespace authstore {
// Tri-state: an unreadable or malformed record is never "no password" (Codex OWNER-02): that would
// reopen the first-password step of an unfinished installation.
enum class Status : uint8_t { Absent, Ok, Unreadable };
Status status(webauth::Cred& out);
bool load(webauth::Cred& out);          // status() == Ok
bool save(const webauth::Cred& in);     // one NVS blob; only on (re)set and password change
bool kdfReady();                        // the PBKDF2 known-answer test passed (run once)
bool kdf(const char* pw, size_t n, const uint8_t* salt, size_t saltLen, uint32_t iterations, uint8_t out[32]);
void random(uint8_t* out, size_t n);
// The Siri key (docs/SIRI.md): 32 lowercase hex characters beside the password record, wiped by the
// factory reset with it. Tri-state like the password (Codex SIRI-01): an unreadable key is never "none".
Status siriKey(char out[33]);
bool setSiriKey(const char* key);   // nullptr erases it; true only if NVS then reads back exactly that state
}
