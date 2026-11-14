// ota_policy.h — pure rules for signed OTA updates (host-testable, no ESP-IDF).
// The device verifies the ECDSA signature over the manifest text (ota_signed.cpp);
// everything decided from the manifest and the streamed image lives here.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace otap {

// The marker every OTA-capable image carries. An update without it would take away
// the WiFi update path itself, so it is refused (USB would be the only way back).
static const char SIGNED_OTA_MARKER[] = "ORCON-SIGNED-OTA v1";
// Every image of the ownership model carries this (docs/PROVISIONING.md). Images without it predate
// per-device owner keys (their password recovery trusts the release key) and are never installed
// over the air; a factory reset erases such an image from the other slot.
static const char OWNER_MODEL_MARKER[] = "ORCON-OWNER-MODEL v1";

// Only explicitly approved firmware variants can be installed over the air.
enum class Variant : uint8_t { None, RxOnly, RfTest, Control };
const char* variantName(Variant v);

// Two signed manifest forms, both verified against the release key compiled into the firmware:
//   v1 "targeted": one board (MAC) and a one-time nonce from it: the developer path (tools/orcon.py).
//   v2 "release": {"v":2,"kind":"orcon-release",...,"version":N}: one signature for every board, so a
//      published release needs no per-device signing. The owner's logged-in session authorises the
//      install (ota_signed.cpp), and it is never older than the running release (ownVersion).
// Neither form is ever accepted without the owner's session on images of the ownership model.
struct Manifest {
    uint8_t  v = 0;
    uint32_t version = 0;            // v2: the release number
    char     mac[18] = "";          // v1: lower-case MAC (six hex pairs) of the one board it is for
    char     env[40] = "";
    Variant  variant = Variant::None;
    bool     allowTx = false;        // must be true exactly for a TX-capable variant
    char     commit[41] = "";
    uint32_t size = 0;
    uint8_t  sha256[32] = {0};
    char     nonce[33] = "";        // one-time value issued by this device (no replay/downgrade)
};

// Parse the signed manifest text. Returns nullptr or an error code (unknown/missing/bad fields).
const char* parseManifest(const char* json, Manifest& m);

// Manifest against this device. v1: board and one-time nonce. v2: no older release than ownVersion,
// released variants only (never the RF test image). Both: slot size, variant/env/allowTx pairing.
const char* checkManifest(const Manifest& m, const char* ownMac, const char* issuedNonce, uint32_t slotSize, uint32_t ownVersion = 0);

// Streaming search over the uploaded image (chunks of any size, patterns may straddle chunks).
class Scanner {
public:
    explicit Scanner(const char* commit);
    void feed(const uint8_t* data, size_t n);
    bool rxRadio = false;     // "SX1276 direct RX (TX disabled)"
    bool rxBanner = false;    // "RX-ONLY TEST FIRMWARE"
    bool rfTest = false;      // capped RF test marker
    bool control = false;     // daily-use control image marker ("txpoc packet TX (CONTROL: ")
    bool txCode = false;      // "txpoc packet TX": the RF backend is linked
    bool dryRun = false;      // "tx-poc-dryrun"
    bool legacyOta = false;   // unauthenticated HTTP/Arduino OTA strings
    bool signedOta = false;   // SIGNED_OTA_MARKER
    bool ownerModel = false;  // OWNER_MODEL_MARKER
    bool commitStamp = false; // the manifest's 40-hex commit
private:
    void match(const uint8_t* buf, size_t n);
    char commit_[41];
    uint8_t tail_[64]; size_t tailN_ = 0;
};

// The image as received against its manifest: size, hash, variant markers, OTA marker, commit.
const char* checkImage(const Manifest& m, const Scanner& s, uint32_t received, const uint8_t sha256[32]);

}  // namespace otap
