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
// An image that runs the ownership model WITH owner-key recovery and without the release-key reset. Unlike the marker
// above (which every build with this scanner contains), only ORCON_OWNER_MODEL builds contain it in plain text: the
// scanner looks for an encoded copy. Required for every incoming image (Codex legacy r3); older owner-model images
// without it stay valid rollback targets.
static const char OWNER_KEY_MARKER[] = "ORCON-OWNER-KEY v1";

// Only explicitly approved firmware variants can be installed over the air. ControlMatter is the control firmware with
// Matter (docs/MATTER.md, env heltec_v2_matter): its own variant, so no update adds or removes Matter by itself.
enum class Variant : uint8_t { None, RxOnly, RfTest, Control, ControlMatter };
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
    uint8_t  pre = 0;                // v2 beta (kind "orcon-prerelease"): beta N = 1..99 of `version`; 0 = stable
    char     mac[18] = "";          // v1: lower-case MAC (six hex pairs) of the one board it is for
    char     env[40] = "";
    Variant  variant = Variant::None;
    bool     allowTx = false;        // must be true exactly for a TX-capable variant
    char     commit[41] = "";
    uint32_t size = 0;
    uint8_t  sha256[32] = {0};
    char     nonce[33] = "";        // one-time value issued by this device (no replay/downgrade)
};

// A local install of another variant than the running build (with or without Matter, control or RX-only) only with the
// owner's explicit confirmation (the page names what changes): nullptr when allowed. The update check never offers one.
const char* variantChange(Variant manifest, const char* ownVariant, bool confirmed);

// Parse the signed manifest text. Returns nullptr or an error code (unknown/missing/bad fields).
const char* parseManifest(const char* json, Manifest& m);

// The order of releases: a stable release above every beta of the same version, so
// 1.0.1 < 1.1.0-beta.1 < 1.1.0-beta.2 < 1.1.0. One rule for the check (fw_update.cpp judge) and every install.
inline uint64_t releaseRank(uint32_t version, uint8_t pre) { return uint64_t(version) * 100u + (pre ? pre : 100u); }

// Manifest against this device. v1: board and one-time nonce. v2: never below the own release (releaseRank), a beta
// only when the owner allows betas ("beta-disabled"), released variants only (never the RF test image). Both: slot
// size, variant/env/allowTx pairing.
const char* checkManifest(const Manifest& m, const char* ownMac, const char* issuedNonce, uint32_t slotSize, uint32_t ownVersion = 0,
                          uint8_t ownPre = 0, bool betaOk = false);

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
    bool ownerKey = false;    // OWNER_KEY_MARKER
    bool releaseKeyReset = false;   // the removed password reset with the firmware signing key (its request kind)
    bool commitStamp = false; // the manifest's 40-hex commit
    bool matter = false;      // the Matter build's marker (idf/components/orcon_matter: orcon_matter_marker)
    bool matterIdentity = false;   // an image that itself holds a Matter identity (a private migration build)
private:
    void match(const uint8_t* buf, size_t n);
    char commit_[41];
    uint8_t tail_[64]; size_t tailN_ = 0;
};

// The image as received against its manifest: size, hash, variant markers, OTA marker, commit.
const char* checkImage(const Manifest& m, const Scanner& s, uint32_t received, const uint8_t sha256[32]);
// An image already on the board that keeps an owner recovery, so it may stay a rollback target: this firmware (owner
// keys), or one of the two older ownership images (b75fad3, 2edb423) whose recovery still trusted the release key.
// Never a build without the ownership model that only carries OWNER_MODEL_MARKER (Codex legacy r4).
inline bool ownerRecovery(const Scanner& s) { return s.ownerModel && (s.ownerKey || s.releaseKeyReset); }
// A rollback image that may stay after a factory reset: one with an owner recovery, without an embedded Matter identity.
// A migration build booted after the reset would move its identity onto the device again: the previous owner's.
inline bool keepAfterReset(const Scanner& s) { return ownerRecovery(s) && !s.matterIdentity; }

// One trial-boot decision per main-loop pass (ota_signed.cpp tick). A healthy image may try to confirm itself after
// confirmAfter ms, at most once per second; the caller reads the owner key right before each attempt. Past rollbackAfter
// an image that is not confirmed returns to the previous one. lastTry: 0 before the first attempt.
enum class Trial : uint8_t { Wait, TryConfirm, Rollback };
Trial trialStep(uint32_t now, bool healthy, uint32_t& lastTry, uint32_t confirmAfter, uint32_t rollbackAfter);

// The running image's OTA state, from esp_ota_get_state_partition (IDF values: PENDING_VERIFY 1, VALID 2, UNDEFINED
// 0xFFFFFFFF, the record boot_app0.bin writes for a USB install). Only a read that says VALID or UNDEFINED is settled;
// PENDING_VERIFY is a trial. Anything else stays unknown at every uptime, a read error and a missing record included
// (IDF 4.4 reports both as ESP_ERR_NOT_FOUND): no update starts and the state is read again (Codex legacy r6).
enum class RunState : uint8_t { Unknown, Trial, Settled };
RunState runState(bool readOk, uint32_t state);

}  // namespace otap
