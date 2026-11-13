// ota_pubkey.h — PUBLIC key that signed OTA manifests must verify against (ECDSA P-256).
// The private key never enters the repository: ~/.orcon/ota-signing-key.pem (mode 600) on the
// developer Mac. Generated with: tools/orcon.py keygen (see docs/OTA.md). Public by design.
#pragma once
static const char OTA_PUBLIC_KEY_PEM[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEXFuaOSaHd13GiScHH7DcMHt1wFpR\n"
    "2xjaeQRD0lzvog6nln41eRyYm6zud8hb4qr4S7MixpVlLNASZB4d5vHtXA==\n"
    "-----END PUBLIC KEY-----\n";
