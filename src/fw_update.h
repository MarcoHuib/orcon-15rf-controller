// fw_update.h — official-release update check and one-click download (pure logic, host-tested).
//
// The control image asks GitHub, over HTTPS, for the newest official release of THIS repository and, only after the
// logged-in owner clicks "Update installeren", streams that release's signed bundle (.orconfw) into the existing
// signed-OTA path (ota_signed.cpp). Nothing here installs on its own, and nothing here trusts the network: the
// release key signature (ota_pubkey.h), the variant/env/version rules (ota_policy.h) and the image checks decide.
//
//   check:   GET https://github.com/<repo>/releases/latest/download/orcon-update.json  (redirects to the asset host)
//            {"schema":1,"manifest":"<signed v2 manifest text>","sig":"<DER hex>","notes":"<short text>"}
//   install: GET https://github.com/<repo>/releases/download/v<semver>/orcon-control-v<semver>-<commit7>.orconfw
//            The URL is built here from the signed manifest of the check, never taken from a browser. The bundle's
//            first line must carry exactly that manifest and signature; then exactly `size` image bytes follow.
//
// Transport (TLS with pinned roots), signature check and the flash writer are interfaces, so the whole flow runs on
// the host against a mock server (test/test_fw_update.cpp). Every buffer is fixed-size; nothing grows with input.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ota_policy.h"

#ifndef ORCON_RELEASE_REPO
#define ORCON_RELEASE_REPO "MarcoHuib/orcon-15rf-controller"   // owner/name on github.com; CI checks it (release_ci.py)
#endif

namespace fwup {

// ---- versions: SemVer MAJOR.MINOR.PATCH <-> the uint32 release number of the signed manifest ----------------------
// number = MAJOR*1000000 + MINOR*1000 + PATCH, each part 0..999 without leading zeros, not 0.0.0 (0 = development
// build). So 1.2.3 = 1002003 and the order of numbers is the SemVer order. tools/release_ci.py uses the same rule.
bool parseSemver(const char* s, uint32_t& out);   // "1.2.3" or "v1.2.3"; false for anything else
void semverText(uint32_t v, char out[16]);         // "1.2.3"; 0 -> "dev"; above 999.999.999 -> "?"
// Channels: also "1.2.3-beta.N" (N 1..99, no leading zero; pre = N, 0 for a stable "1.2.3"). Nothing else: no
// "-rc.1", no "-beta.0", no build metadata. tools/release_ci.py must use the same rule once the workflow publishes betas.
bool parseSemverPre(const char* s, uint32_t& out, uint8_t& pre);
void semverTextPre(uint32_t v, uint8_t pre, char out[24]);   // "1.2.3" or "1.2.3-beta.4"

// ---- names and URLs, built from the signed manifest only ----------------------------------------------------------
static const char INDEX_ASSET[] = "orcon-update.json";
// A board with Matter (variant control-matter) reads its own index: one release carries both variants, each with its
// own signed manifest, and a board without Matter (v1.0.x too) keeps reading orcon-update.json exactly as before.
static const char INDEX_ASSET_MATTER[] = "orcon-update-matter.json";
const char* indexAsset(otap::Variant v);
// The beta channel reads one fixed address, the pre-release "channel-beta" whose index the workflow replaces with
// every publication (stable and beta), so it always names the highest of both. Stable reads "latest" as before.
static const char BETA_TAG[] = "channel-beta";
bool bundleName(const otap::Manifest& m, char* out, size_t n);   // orcon-control-v1.2.3-abcdef1.orconfw (or -v1.2.3-beta.4-)
bool indexUrl(char* out, size_t n, bool beta = false, otap::Variant v = otap::Variant::Control);
bool bundleUrl(const otap::Manifest& m, char* out, size_t n);

static const size_t HOST_MAX = 64, PATH_MAX_LEN = 1600;   // a signed asset-host redirect is ~950 characters today
struct Url { char host[HOST_MAX]; char path[PATH_MAX_LEN]; };
// https only: lower-case host of [a-z0-9.-], no user info, no port other than 443, a path of visible ASCII.
bool parseUrl(const char* url, Url& u);
// The only hosts ever contacted: github.com under /<repo>/releases/, and GitHub's release-asset hosts.
bool allowedUrl(const Url& u);
// A redirect target: an absolute https URL, or an absolute path on the same host. Then allowedUrl() decides.
bool resolveLocation(const char* location, const Url& base, Url& out);

// ---- HTTP/1.1 response head, fed byte by byte with hard limits --------------------------------------------------
class Head {
public:
    // 0: need more, 1: complete (the next byte is body), <0: refused (see error()).
    int push(char c);
    void reset() { status = 0; length = -1; chunked = encoded = done_ = hasLocation_ = cr_ = skip_ = named_ = false; location[0] = 0; n_ = total_ = lines_ = 0; err_ = nullptr; }
    int status = 0;
    int64_t length = -1;          // Content-Length; -1 when absent
    bool chunked = false;         // any Transfer-Encoding other than identity (refused for bodies)
    bool encoded = false;         // a Content-Encoding other than identity (refused for bodies)
    char location[PATH_MAX_LEN + 16] = "";
    const char* error() const { return err_; }
private:
    int line();
    // One line at a time: the status line, a field name, and the value of the four fields read below (a longer one is
    // refused, never cut). Any other field's value is checked byte by byte and not kept (GitHub sends a 4,203-byte CSP).
    char buf_[PATH_MAX_LEN + 48]; size_t n_ = 0;
    size_t total_ = 0, lines_ = 0;
    bool done_ = false, hasLocation_ = false;
    bool cr_ = false;      // the previous byte was a CR: only an LF may follow
    bool skip_ = false;    // this line's field is not read: its value is checked, not stored
    bool named_ = false;   // this line's field name (up to the first colon) has been checked
    const char* err_ = nullptr;
};

// Exactly one flat JSON object as this code writes it: double-quoted keys without escapes (unique), values that are a
// double-quoted string (standard escapes, never \u0000) or a non-negative integer, then only whitespace. ArduinoJson is
// more lenient (single quotes, bare keys, later duplicates win); the index and the bundle head must pass this first.
bool flatObject(const char* s, size_t n);

// ---- the transport (device: WiFiClientSecure with the pinned roots; host tests: a scripted mock) ------------------
struct Transport {
    virtual ~Transport() {}
    virtual bool connect(const char* host, uint32_t timeoutMs) = 0;   // TLS to host:443, chain + host name verified
    virtual bool write(const char* data, size_t n) = 0;
    virtual int read(uint8_t* buf, size_t n, uint32_t timeoutMs) = 0;  // >0 bytes, 0 closed by peer, <0 timeout/error
    virtual void stop() = 0;
    virtual const char* connectError() = 0;   // after a failed connect: "dns-failed", "tls-failed" or "connect-failed"
};
using Clock = uint32_t (*)();   // milliseconds (millis() on the device)
using Verify = bool (*)(const char* text, size_t n, const char* sigHex);   // ota::verifySignature on the device

struct Limits {
    uint32_t connectMs = 15000, stallMs = 15000;    // per connection/handshake, and per silent read
    uint32_t checkMs = 45000, installMs = 600000;   // the whole check, the whole download
};

// GET url, following at most 3 allowed redirects. On success the transport is open at the start of the body and
// bodyLen is its exact length (Content-Length is required). Errors: no-network, dns-failed, tls-failed, connect-failed,
// timeout, connection-lost, bad-response, redirect-not-allowed, too-many-redirects, not-found, rate-limited,
// server-error, http-error, unsupported-encoding, response-too-large.
const char* httpGet(Transport& t, const char* url, Clock now, uint32_t deadline, const Limits& lim,
                    uint32_t maxBody, uint32_t& bodyLen, Url& scratch, Head& head);

// ---- what this device runs, and the offer of a release ----------------------------------------------------------
// An aggregate (no member initialisers: PlatformIO builds C++11): fields left out of a brace list are zero/false.
struct Own { uint32_t version; otap::Variant variant; const char* env; uint32_t slotSize;
             uint8_t pre; bool beta; };   // this image's beta number (0 = stable), and the owner's beta channel
static const size_t INDEX_MAX = 2048, MANIFEST_MAX = 600, SIG_MAX = 160, NOTES_MAX = 400;
struct Offer {
    char manifest[MANIFEST_MAX + 1] = "";
    char sig[SIG_MAX + 1] = "";
    char notes[NOTES_MAX + 1] = "";
    otap::Manifest m;
};
// The index file: exactly {"schema":1,"manifest","sig","notes"}, bounded sizes, notes without control characters.
const char* parseIndex(const char* json, size_t n, Offer& o);
// After the signature: the signed manifest against this device. nullptr = a newer official release for exactly this
// variant and env; "up-to-date" = the same release; otherwise why it is not offered.
const char* judge(const Offer& o, const Own& own);

// Free internal 8-bit RAM for one whole job with the owner's 20 KB reserve left at its peak: worker stack, Ctx and one
// TLS session with dynamic buffers, roots on demand and no kept peer chain (QEMU peak 47.6 KB), the WiFi RX buffers
// during TLS (up to 8 KB, estimate) and 20 KB: 75.6 KB. One 16 KB TLS record must still fit in one piece. Checked before
// a job starts and again before each TLS connect; "low-memory" otherwise. Lower it only after board measurements.
static const uint32_t MIN_FREE_HEAP = 76000, MIN_FREE_BLOCK = 32000;
const char* memoryOk(uint32_t freeHeap, uint32_t largestBlock);

// ---- The update check next to Matter, through ONE restart without Matter --------------------------
// With Matter running the gate above stays shut, so the owner may ask for one restart in which Matter stays off: the
// check runs at once, a newer release can be installed through the normal page, then the device starts normally again.
// The device glue (fw_update_esp.cpp) keeps the request in RTC memory for exactly one software restart.
struct UpdateBootAsk { bool matterBuild, roomNow, jobBusy, restartPending, trialBoot, matterBusy, sessionBusy; };
inline const char* updateBootRefusal(const UpdateBootAsk& a) {   // nullptr: restart for the check now
    if (!a.matterBuild || a.roomNow) return "not-needed";        // "Controleer op updates" works without a restart
    if (a.jobBusy || a.restartPending) return "update-busy";
    if (a.trialBoot) return "confirm-running-image-first";       // a trial boot confirms only with Matter running
    if (a.matterBusy) return "matter-busy";                      // a controller is commissioning this device
    if (a.sessionBusy) return "session-busy";                    // a read/pair session or an RF command in flight
    return nullptr;
}
static const uint32_t UPDATE_BOOT_MAX_MS = 600000;   // Matter is off for at most 10 minutes
static const uint32_t UPDATE_BOOT_REST_MS = 60000;   // after a check without a newer release (the page shows why)
// In the update boot: back to the normal start with Matter now? Never during a download, its restart or an RF command.
inline bool updateBootDone(uint32_t sinceBootMs, bool checked, bool newer, uint32_t checkedAtMs, bool busy) {
    if (busy) return false;
    if (sinceBootMs >= UPDATE_BOOT_MAX_MS) return true;
    return checked && !newer && sinceBootMs - checkedAtMs >= UPDATE_BOOT_REST_MS;
}

enum class Found : uint8_t { None, Newer, UpToDate };
// The whole check. On Newer, `offer` holds the verified release. Errors as httpGet, plus bad-index, bad-signature,
// wrong-variant, older-release, and the manifest errors of ota_policy.
const char* check(Transport& t, Verify verify, Clock now, const Limits& lim, const Own& own, Offer& offer, Found& found,
                  char* io, size_t ioLen, Url& scratch, Head& head);

// ---- install: the existing signed-OTA writer behind an interface ------------------------------------------------------
struct Sink {
    virtual ~Sink() {}
    // Signature, manifest and device checks of ota_signed.cpp (the same as a manual .orconfw upload), then the
    // inactive slot is opened. nullptr = ready for exactly m.size bytes.
    virtual const char* begin(const char* manifest, const char* sig) = 0;
    virtual const char* write(const uint8_t* d, size_t n) = 0;
    virtual const char* finish() = 0;   // size, SHA-256, image markers, ESP-IDF image check, then the boot slot
    virtual void abort(const char* why) = 0;   // nothing was activated; the running image stays the boot image
};
using Progress = void (*)(uint32_t done, uint32_t total);
// Download the offered bundle and stream it into the sink. nullptr = written, verified and selected for the next
// boot (trial boot with rollback). Any failure aborts the sink. Errors as httpGet, plus bundle-mismatch, bad-bundle,
// size-mismatch, and the sink's own.
const char* install(Transport& t, Sink& sink, Clock now, const Limits& lim, const Offer& offer, Progress progress,
                    uint8_t* io, size_t ioLen, Url& scratch, Head& head);

}  // namespace fwup
