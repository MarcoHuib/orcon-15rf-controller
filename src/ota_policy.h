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

// Only explicitly approved firmware variants can be installed over the air.
enum class Variant : uint8_t { None, RxOnly, RfTest };
const char* variantName(Variant v);

struct Manifest {
    uint8_t  v = 0;
    char     mac[18] = "";          // lower-case aa:bb:cc:dd:ee:ff of the one board it is for
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

// Manifest against this device: board, one-time nonce, slot size, variant/env/allowTx pairing.
const char* checkManifest(const Manifest& m, const char* ownMac, const char* issuedNonce, uint32_t slotSize);

// Streaming search over the uploaded image (chunks of any size, patterns may straddle chunks).
class Scanner {
public:
    explicit Scanner(const char* commit);
    void feed(const uint8_t* data, size_t n);
    bool rxRadio = false;     // "SX1276 direct RX (TX disabled)"
    bool rxBanner = false;    // "RX-ONLY TEST FIRMWARE"
    bool rfTest = false;      // capped RF test marker
    bool txCode = false;      // "txpoc packet TX": the RF backend is linked
    bool dryRun = false;      // "tx-poc-dryrun"
    bool legacyOta = false;   // unauthenticated HTTP/Arduino OTA strings
    bool signedOta = false;   // SIGNED_OTA_MARKER
    bool commitStamp = false; // the manifest's 40-hex commit
private:
    void match(const uint8_t* buf, size_t n);
    char commit_[41];
    uint8_t tail_[64]; size_t tailN_ = 0;
};

// The image as received against its manifest: size, hash, variant markers, OTA marker, commit.
const char* checkImage(const Manifest& m, const Scanner& s, uint32_t received, const uint8_t sha256[32]);

}  // namespace otap
