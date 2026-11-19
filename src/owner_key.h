// owner_key.h — the owner's recovery key file: a per-device ECDSA P-256 key as a standard encrypted
// PKCS#8 PEM ("-----BEGIN ENCRYPTED PRIVATE KEY-----", RFC 5958 + RFC 8018 PBES2 with
// PBKDF2-HMAC-SHA256 and AES-256-CBC: what `openssl pkcs8 -topk8 -v2 aes-256-cbc -v2prf
// hmacWithSHA256` writes, and OpenSSL/Python read it back).
//
// Pure encoding logic (host-tested against OpenSSL: test/test_owner_key.cpp). The cryptographic
// primitives are injected: mbedTLS + the hardware RNG on the device (owner_store.cpp), OpenSSL in
// the host test. Nothing here invents cryptography; it only builds and parses the standard DER/PEM
// containers around those primitives.
//
// The private key is created on the device, leaves it once (encrypted with the owner's passphrase,
// over the WPA2 setup access point) and is never stored there; only the public key is.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace ownerkey {
constexpr size_t PRIV_LEN = 32, PUB_LEN = 65;           // P-256 scalar; uncompressed point 04|X|Y
constexpr size_t PHRASE_MIN = 10, PHRASE_MAX = 64;          // the passphrase of the key file
constexpr uint32_t KDF_ITERATIONS = 50000;              // written; ~6 s on the ESP32 (hardware SHA)
constexpr uint32_t ITER_MIN = 2048, ITER_MAX = 200000;  // read: OpenSSL's default upward, bounded work
constexpr size_t PEM_MAX = 900;                         // our file is ~560 B including the text header

struct Crypto {
    bool (*pbkdf2)(const uint8_t* pw, size_t n, const uint8_t* salt, size_t sn, uint32_t iter, uint8_t out[32]);  // HMAC-SHA256
    bool (*aes256cbc)(bool encrypt, const uint8_t key[32], const uint8_t iv[16], const uint8_t* in, size_t n, uint8_t* out);
    void (*random)(uint8_t* out, size_t n);
    bool (*publicKey)(const uint8_t d[32], uint8_t q[65]);  // q = d*G; false unless 0 < d < n
    void (*sha256)(const uint8_t* in, size_t n, uint8_t out[32]);
};

bool passphraseOk(const char* p);                       // 10..64 bytes, no control characters

// The key file for (d, q). out receives a short explanatory text, then the PEM block.
bool encryptPem(const uint8_t d[32], const uint8_t q[65], const char* pass, const Crypto&, char* out, size_t cap);

// Reads a key file (ours or OpenSSL's) and returns d and q = d*G (computed, never trusted from the
// file). Errors: "bad-file" (not an encrypted P-256 PKCS#8 PEM), "bad-passphrase" (wrong passphrase
// or damaged file), "bad-key" (the key inside is invalid).
const char* decryptPem(const char* pem, const char* pass, const Crypto&, uint8_t d[32], uint8_t q[65]);

// Short form for people: "ab12-cd34-ef56" (first 6 bytes of SHA-256 over the public key).
void fingerprint(const uint8_t q[65], const Crypto&, char out[15]);
bool samePub(const uint8_t a[65], const uint8_t b[65]);   // constant time
void wipe(void* p, size_t n);                             // not optimised away
}  // namespace ownerkey
