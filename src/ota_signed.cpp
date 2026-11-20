// ota_signed.cpp — signed A/B OTA over WiFi and remote diagnostics (Heltec V2 + SX1276).
//
// Update: POST /api/ota/nonce -> the Mac signs a manifest (board MAC, env, approved variant,
// commit, size, SHA-256, that nonce) with its private ECDSA P-256 key -> POST /api/ota/begin
// (manifest + signature, verified here against ota_pubkey.h) -> POST /api/ota/upload streams
// the image into the inactive slot (esp_ota_*), hashing and scanning it on the way. Only a
// complete image whose hash, markers and ESP-IDF image check all pass becomes the boot
// partition. The bootloader (rollback enabled) boots it as PENDING_VERIFY; tick() confirms it
// once WiFi and RX are healthy, or rolls back. A reset before confirmation also rolls back.
// No legacy Update/ArduinoOTA code, no NVS/bootloader/partition-table writes.
#include "config.h"
#if ORCON_SIGNED_OTA
#include "ota_signed.h"
#include "ota_policy.h"
#include "fw_update.h"
#include "ota_pubkey.h"
#include "net.h"
#include "ramses_radio.h"
#include "setup_web.h"
#include "rf_console.h"
#include "tx_poc_caps.h"
#if ORCON_OWNER_MODEL
#include "owner_store.h"
#endif
#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_image_format.h>
#include <atomic>
#include <lwip/sockets.h>   // shutdown(): end a refused upload on the live socket
#include <esp_system.h>
#include <esp_core_dump.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

// Arduino core hook: with rollback enabled, WE confirm a new image (tick), not initArduino().
// C linkage is required: the core's weak default is a C symbol. A C++ definition does not
// override it, and the core would then confirm every new image at boot (no trial boot, no
// rollback). Evidence row rollback-hook-<env> checks the linked ELF.
extern "C" bool verifyRollbackLater() { return true; }

namespace ota {

static WebServer* g_s = nullptr;
static const uint32_t NONCE_TTL_MS = 300000, CONFIRM_AFTER_MS = 60000, ROLLBACK_AFTER_MS = 300000;
enum class St : uint8_t { Idle, Ready, Writing, Done, Failed };
static St g_st = St::Idle;
static char g_nonce[33] = "";
static uint32_t g_nonceMs = 0;
static otap::Manifest g_m;
static otap::Scanner g_scan("");
static const esp_partition_t* g_part = nullptr;
static esp_ota_handle_t g_h = 0;
static uint32_t g_rx = 0;
static mbedtls_sha256_context g_sha;
static const char* g_err = "none";
// The one-click update (fw_update_esp.cpp) owns the writer while this is set: its worker task runs the same
// checks and writes below, and the manual routes refuse with update-busy. Set and cleared via claimDownload()
// and endDownload() only; the loop task sets it before the worker exists, so the two never race.
static std::atomic<bool> g_dl{false};   // sequentially consistent: the hand-over orders the writer fields (Codex r3)
// An image was written, verified and selected for the next boot: until the restart, nothing may open the writer again.
// esp_ota_get_next_update_partition() would return that very slot, and a second write would erase the selected image
// (Codex review round 1). Cleared only by the restart.
static std::atomic<bool> g_selected{false};   // also read by the loop watchdog on core 0 (Codex r6)

// Survives soft resets, panics and watchdogs (not power loss): previous uptime and boots.
struct Crumb { uint32_t magic, boots, lastUptimeS; };
static RTC_NOINIT_ATTR Crumb g_crumb;
static uint32_t g_prevUptimeS = 0;
static bool g_prevKnown = false;
static std::atomic<bool> g_pending{false};      // also read by the loop watchdog on core 0 (Codex r6)
static bool g_hold = false;   // rollback test: this trial boot refuses to confirm itself (RAM only)
#define ORCON_STR2(x) #x
#define ORCON_STR(x) ORCON_STR2(x)
// The release number as text in the image: tools/orcon.py release refuses a bundle whose version differs.
static const char RELEASE_STAMP[] = "ORCON-RELEASE " ORCON_STR(ORCON_RELEASE_VERSION) ";";

static const char* variantOfThisBuild() {
#if ORCON_TX_MAX_PER_BOOT
    return "rf-test";
#elif ORCON_TX_CONTROL
    return "control";
#elif RX_ONLY && !ORCON_TX_POC
    return "rx-only";
#else
    return "other";
#endif
}
static const char* resetName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON: return "power-on";   case ESP_RST_EXT: return "external";
        case ESP_RST_SW: return "software";        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "int-watchdog"; case ESP_RST_TASK_WDT: return "task-watchdog";
        case ESP_RST_WDT: return "watchdog";       case ESP_RST_DEEPSLEEP: return "deep-sleep";
        case ESP_RST_BROWNOUT: return "brownout";  case ESP_RST_SDIO: return "sdio";
        default: return "unknown";
    }
}
static const char* stateName(const esp_partition_t* p) {
    esp_ota_img_states_t st;
    if (!p || esp_ota_get_state_partition(p, &st) != ESP_OK) return "undefined";
    switch (st) {
        case ESP_OTA_IMG_NEW: return "new"; case ESP_OTA_IMG_PENDING_VERIFY: return "pending-verify";
        case ESP_OTA_IMG_VALID: return "valid"; case ESP_OTA_IMG_INVALID: return "invalid";
        case ESP_OTA_IMG_ABORTED: return "aborted"; default: return "undefined";
    }
}
void ownMac(char out[18]) {
    uint8_t m[6] = {0};
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}
// DNS rebinding: only our own address or mDNS name (an optional :port is allowed).
static bool hostOk() {
    String h = g_s->hostHeader(); const int c = h.indexOf(':'); if (c >= 0) h = h.substring(0, c);
    return h == net::ipAddr() || h.equalsIgnoreCase("orcon15rf.local");
}
static void reply(int code, const char* body) {
    g_s->sendHeader("Cache-Control", "no-store");
    g_s->send(code, "application/json", body);
}
static void fail(int code, const char* e) {
    char b[96]; snprintf(b, sizeof(b), "{\"error\":\"%s\"}", e); reply(code, b);
}
static void note(uint8_t kind, const char* fmt, ...) {
    char m[96]; va_list a; va_start(a, fmt); vsnprintf(m, sizeof(m), fmt, a); va_end(a);
    Serial.printf("[ota] %s\n", m);
#if !ORCON_TX_POC || ORCON_WEB_CONTROL
    setupweb::note(kind, m);               // the RF console exists on these builds only
#else
    (void)kind;
#endif
}

// ---- GET /api/ota ------------------------------------------------------------------
static void handleInfo() {
    if (!hostOk()) { fail(403, "forbidden-request"); return; }
    const esp_partition_t* run = esp_ota_get_running_partition();
    const esp_partition_t* boot = esp_ota_get_boot_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    char b[640], rel[16];
    fwup::semverText(ORCON_RELEASE_VERSION, rel);
    const bool dl = g_dl;   // the worker owns g_st/g_err while set: report a fixed state then (Codex r4)
    snprintf(b, sizeof(b),
             "{\"signed_ota\":\"%s\",\"fw\":\"%s\",\"variant\":\"%s\",\"running\":\"%s\",\"running_state\":\"%s\","
             "\"boot\":\"%s\",\"next\":\"%s\",\"next_state\":\"%s\",\"slot_size\":%lu,\"session\":\"%s\",\"last_error\":\"%s\","
             "\"nonce_active\":%s,\"hold\":%s,\"owner_model\":\"%s\",\"release\":%lu,\"release_stamp\":\"%s\",\"release_name\":\"%s\"}",
             otap::SIGNED_OTA_MARKER, FW_VERSION, variantOfThisBuild(), run ? run->label : "?", stateName(run),
             boot ? boot->label : "?", next ? next->label : "?", stateName(next), (unsigned long)(next ? next->size : 0),
             dl ? "download" : g_st == St::Idle ? "idle" : g_st == St::Ready ? "ready" : g_st == St::Writing ? "writing" : g_st == St::Done ? "done" : "failed",
             dl ? "none" : g_err, g_nonce[0] && millis() - g_nonceMs < NONCE_TTL_MS ? "true" : "false", g_hold ? "true" : "false",
             otap::OWNER_MODEL_MARKER, (unsigned long)ORCON_RELEASE_VERSION, RELEASE_STAMP, rel);
    reply(200, b);
}

// ---- POST /api/ota/hold: rollback test. Only during a trial boot, and it can only make the
// update FAIL (the previous image returns after ROLLBACK_AFTER_MS), never install anything.
// Still a way to undo an update, so only a logged-in owner may use it (session, CSRF token,
// same origin; never from the setup AP on web-control builds). Registered where the login exists.
#if !ORCON_TX_POC || ORCON_WEB_CONTROL
static void handleHold() {
    if (!setupweb::authorize(true)) return;
    if (!g_pending) { fail(409, "not-a-trial-boot"); return; }
    g_hold = true;
    note(rfcon::WARN, "rollbacktest: bevestiging tegengehouden; vorige firmware terug na %lu s", (unsigned long)(ROLLBACK_AFTER_MS / 1000));
    char b[64]; snprintf(b, sizeof(b), "{\"ok\":true,\"rollback_in_s\":%ld}", long(ROLLBACK_AFTER_MS / 1000) - long(millis() / 1000));
    reply(200, b);
}
#endif

// ---- POST /api/ota/nonce -----------------------------------------------------------------
// The release key proves an image is official; it does not authorise installing it here. On
// ownership-model builds every install step needs the owner's logged-in session (with its CSRF
// token), so neither a LAN attacker nor the release-key holder can install anything on their own.
static bool ownerConsent() {
#if ORCON_OWNER_MODEL
    return setupweb::authorize(true);
#else
    if (!hostOk()) { fail(403, "forbidden-request"); return false; }
    return true;
#endif
}
// The manual install steps: never while the download owns the writer, a selected image waits for its restart, any
// restart is queued, the running image is still on trial, or a read/pair session or RF command is in flight.
// The bootloader fell back from the selected slot (e.g. unreadable there): that slot is still the boot choice and is the
// next update target, so bytes written there could boot before every check passed (Codex r4). Refuse until the
// running image is the boot choice again (USB recovery, docs/AB_OVERGANG.md).
static bool bootMismatch() { return esp_ota_get_boot_partition() != esp_ota_get_running_partition(); }
static const char* manualBlocked() {
    if (g_dl) return "update-busy";
    if (g_selected || net::restartQueued()) return "restart-pending";
    if (bootMismatch()) return "boot-slot-mismatch";
    if (g_st == St::Writing) return "update-busy";
    if (g_pending) return "confirm-running-image-first";
#if !ORCON_TX_POC || ORCON_WEB_CONTROL
    if (setupweb::busy()) return "session-busy";
#endif
    return nullptr;
}
static void handleNonce() {
    if (!ownerConsent()) return;
    if (const char* e = manualBlocked()) { fail(409, e); return; }
    uint8_t r[16]; esp_fill_random(r, sizeof(r));
    for (int i = 0; i < 16; ++i) snprintf(g_nonce + 2 * i, 3, "%02x", r[i]);
    g_nonceMs = millis(); g_st = St::Idle; g_err = "none";
    char b[96]; snprintf(b, sizeof(b), "{\"nonce\":\"%s\",\"ttl_s\":%lu}", g_nonce, (unsigned long)(NONCE_TTL_MS / 1000));
    reply(200, b);
}

bool verifySignature(const char* text, size_t n, const char* sigHex) {
    const size_t hexLen = strlen(sigHex);
    if (hexLen < 16 || hexLen > 2 * 80 || hexLen % 2) return false;
    uint8_t sig[80]; const size_t sigLen = hexLen / 2;
    for (size_t i = 0; i < sigLen; ++i) {
        char h[3] = {sigHex[2 * i], sigHex[2 * i + 1], 0}; char* e = nullptr;
        sig[i] = (uint8_t)strtoul(h, &e, 16); if (*e) return false;
    }
    uint8_t hash[32];
    mbedtls_sha256_ret((const unsigned char*)text, n, hash, 0);
    mbedtls_pk_context pk; mbedtls_pk_init(&pk);
    bool ok = mbedtls_pk_parse_public_key(&pk, (const unsigned char*)OTA_PUBLIC_KEY_PEM, sizeof(OTA_PUBLIC_KEY_PEM)) == 0 &&
              mbedtls_pk_can_do(&pk, MBEDTLS_PK_ECKEY) &&
              mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sigLen) == 0;
    mbedtls_pk_free(&pk);
    return ok;
}

// The signed manifest against this device: the release key, the manifest rules and the inactive slot. Shared by
// POST /api/ota/begin (v1 targeted or v2 release) and the one-click update (v2 release only). On success the
// writer is Ready for exactly this image.
static const char* acceptManifest(const char* text, const char* sig, const char* nonce, bool releaseOnly) {
    g_st = St::Idle;
    if (!verifySignature(text, strlen(text), sig)) return "bad-signature";
    otap::Manifest m;
    const char* e = otap::parseManifest(text, m);
    if (!e && releaseOnly && m.v != 2) e = "manifest-version";
    char mac[18]; ownMac(mac);
    g_part = esp_ota_get_next_update_partition(nullptr);
    if (!e) e = otap::checkManifest(m, mac, nonce, g_part ? g_part->size : 0, ORCON_RELEASE_VERSION);
    if (e) return e;
    g_m = m; g_scan = otap::Scanner(m.commit); g_st = St::Ready; g_err = "none";
    return nullptr;
}

// ---- POST /api/ota/begin {"manifest":"<json text>","sig":"<DER hex>"} --------------------
static void handleBegin() {
    if (!ownerConsent()) return;
    if (const char* e = manualBlocked()) { fail(409, e); return; }
    const String body = g_s->arg("plain");
    if (body.length() > 1024) { fail(413, "body-too-large"); return; }
    JsonDocument d;
    if (deserializeJson(d, body) || !d["manifest"].is<const char*>() || !d["sig"].is<const char*>() || d.as<JsonObject>().size() != 2) {
        fail(400, "bad-json"); return;
    }
    const char* text = d["manifest"]; const char* sig = d["sig"];
    char nonce[33]; strcpy(nonce, g_nonce);
    const bool fresh = nonce[0] && millis() - g_nonceMs < NONCE_TTL_MS;
    g_nonce[0] = 0;                                   // one attempt per nonce, valid or not
    if (const char* e = acceptManifest(text, sig, fresh ? nonce : "", false)) {
        g_err = e;
        if (!strcmp(e, "bad-signature")) { note(rfcon::WARN, "OTA geweigerd: ongeldige handtekening"); fail(403, e); }
        else { note(rfcon::WARN, "OTA geweigerd: %s", e); fail(409, e); }
        return;
    }
    note(rfcon::INFO, "OTA klaar voor upload: %s %.7s (%lu B) naar %s", otap::variantName(g_m.variant), g_m.commit,
         (unsigned long)g_m.size, g_part->label);
    char b[160]; snprintf(b, sizeof(b), "{\"ok\":true,\"partition\":\"%s\",\"size\":%lu,\"variant\":\"%s\"}",
                          g_part->label, (unsigned long)g_m.size, otap::variantName(g_m.variant));
    reply(200, b);
}

// ---- the image writer: the inactive slot, hashed and scanned on the way (upload and one-click update) -----------
// A rejected or interrupted image never stays a complete ESP image in the inactive slot: the bootloader's last-resort
// scan (when the selected slot fails to load) would start it without the release checks (Codex r3). Erase its first
// sector. shortcut: a power cut between the last byte and the checks still leaves one; withholding the header until
// the checks pass would close that too.
static void scrub() {
    if (g_part && esp_partition_erase_range(g_part, 0, SPI_FLASH_SEC_SIZE) != ESP_OK) Serial.println("[ota] could not erase the rejected image's first sector");
}
static void abortWrite(const char* e) {
    if (g_st == St::Writing) { esp_ota_abort(g_h); mbedtls_sha256_free(&g_sha); scrub(); }
    g_st = St::Failed; g_err = e;
}
static const char* startWrite() {   // Ready -> Writing
    if (g_selected) { g_st = St::Failed; return g_err = "restart-pending"; }
    if (g_st != St::Ready) return "not-ready";
    if (esp_ota_begin(g_part, OTA_WITH_SEQUENTIAL_WRITES, &g_h) != ESP_OK) { g_st = St::Failed; return g_err = "ota-begin-failed"; }
    mbedtls_sha256_init(&g_sha); mbedtls_sha256_starts_ret(&g_sha, 0);
    g_rx = 0; g_st = St::Writing;
    return nullptr;
}
static const char* writeChunk(const uint8_t* d, size_t n) {
    if (g_st != St::Writing) return g_err;
    if (g_rx + n > g_m.size) { abortWrite("image-too-large"); return g_err; }
    if (esp_ota_write(g_h, d, n) != ESP_OK) { abortWrite("flash-write-failed"); return g_err; }
    mbedtls_sha256_update_ret(&g_sha, d, n);
    g_scan.feed(d, n);
    g_rx += n;
    return nullptr;
}
// Size, SHA-256 and image markers (ota_policy), the ESP-IDF image check, then the boot slot. Only now does
// anything change what boots next: as a PENDING_VERIFY trial with bootloader rollback.
static const char* endWrite() {
    if (g_st != St::Writing) return g_err;
    uint8_t hash[32]; mbedtls_sha256_finish_ret(&g_sha, hash); mbedtls_sha256_free(&g_sha);
    const char* e = otap::checkImage(g_m, g_scan, g_rx, hash);
    if (e) { esp_ota_abort(g_h); scrub(); g_st = St::Failed; return g_err = e; }
    // ESP-IDF validates the image (header, segments, appended SHA-256) before it can boot.
    if (esp_ota_end(g_h) != ESP_OK) { scrub(); g_st = St::Failed; return g_err = "image-invalid"; }
    if (esp_ota_set_boot_partition(g_part) != ESP_OK) { scrub(); g_st = St::Failed; return g_err = "set-boot-failed"; }
    g_selected = true;
    g_st = St::Done;
    return nullptr;
}

// ---- POST /api/ota/upload (multipart, one file) -----------------------------------------
// WebServer::client() returns a copy, and stop() on a copy only drops that copy: the core would go on reading the body
// on the live socket (Codex r6). Shut the socket itself down, so the core's next read ends the request at once.
static void dropClient() {
    WiFiClient c = g_s->client();
    if (c.fd() >= 0) shutdown(c.fd(), SHUT_RDWR);
    c.stop();
}
static void handleUploadData() {
    HTTPUpload& up = g_s->upload();
    if (up.status == UPLOAD_FILE_START) {
        // A refused upload is closed here, before its body: the WebServer reads a multipart body without any
        // time limit, so an idle client could otherwise hold the loop (confirmation, rollback, restarts; Codex r4).
        if (g_dl) { dropClient(); return; }   // the download owns the writer: leave its state alone
        if (g_st != St::Ready || !hostOk()) { g_st = St::Failed; g_err = "not-ready"; dropClient(); return; }
        if (const char* e = manualBlocked()) { g_st = St::Failed; g_err = e; dropClient(); return; }
#if ORCON_OWNER_MODEL
        if (!setupweb::uploadAuthorized()) { g_st = St::Failed; g_err = "login-required"; dropClient(); return; }
#endif
        if (startWrite()) return;
        heartbeat();   // an accepted upload runs inside one handleClient(): only its chunks count as progress (Codex r6)
        note(rfcon::INFO, "OTA schrijft %s (RF-ontvangst kan tijdens het schrijven haperen)", g_part->label);
    } else if (g_dl) {
        return;
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (g_st == St::Writing) heartbeat();
        writeChunk(up.buf, up.currentSize);
    } else if (up.status == UPLOAD_FILE_END) {
        // Selected for the next boot: restart from here, not only from the reply handler. The WebServer skips that
        // handler when the client disconnects after the last byte (Codex r2), and the board would otherwise boot the
        // new image at some later, unexpected restart.
        if (!endWrite()) net::restartSoon();
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        if (g_st != St::Done) abortWrite("upload-aborted");   // an image already selected stays selected (restart pending)
    }
}
static void handleUploadDone() {
    if (g_dl) { fail(409, "update-busy"); return; }
    if (g_st == St::Writing) abortWrite("upload-incomplete");
    if (g_st != St::Done) {
        note(rfcon::WARN, "OTA mislukt: %s (opstartpartitie ongewijzigd)", g_err);
        fail(g_st == St::Failed && !strcmp(g_err, "login-required") ? 401 : g_st == St::Failed && !strcmp(g_err, "not-ready") ? 409 : 422, g_err);
        g_st = St::Idle; return;
    }
    note(rfcon::INFO, "OTA geschreven en geverifieerd: herstart naar %s (proefstart met rollback)", g_part->label);
    char b[128]; snprintf(b, sizeof(b), "{\"ok\":true,\"restarting\":true,\"boot\":\"%s\"}", g_part->label);
    g_s->sendHeader("Connection", "close");
    reply(200, b);
    g_st = St::Idle;
    net::restartSoon();
}

// ---- the one-click update (fw_update_esp.cpp): the same checks and writer as begin + upload, v2 releases only ----
bool pendingVerify() { return g_pending; }
bool writerBusy() { return g_dl || g_st == St::Writing || g_selected; }
uint32_t updateSlotSize() { const esp_partition_t* p = esp_ota_get_next_update_partition(nullptr); return p ? p->size : 0; }
const char* claimDownload() {   // loop task, before the worker task exists
    if (g_selected) return "restart-pending";
    if (bootMismatch()) return "boot-slot-mismatch";
    if (g_dl || g_st == St::Writing) return "update-busy";
    if (g_pending) return "confirm-running-image-first";
    g_nonce[0] = 0; g_st = St::Idle; g_err = "none";   // a manual begin that was never uploaded is dropped
    g_dl = true;
    return nullptr;
}
void endDownload() {            // the worker, last: the writer is free again (a written image stays the boot slot)
    if (g_st == St::Writing) abortWrite("download-ended");
    g_st = St::Idle;
    g_dl = false;
}
namespace {
struct ReleaseSink : fwup::Sink {
    const char* begin(const char* manifest, const char* sig) override {
        if (!g_dl) return "not-ready";
        if (const char* e = acceptManifest(manifest, sig, "", true)) { g_err = e; return e; }
        return startWrite();
    }
    const char* write(const uint8_t* d, size_t n) override { return writeChunk(d, n); }
    const char* finish() override { return endWrite(); }
    void abort(const char* why) override { if (g_st == St::Writing) abortWrite(why); }
};
}  // namespace
fwup::Sink& releaseSink() { static ReleaseSink s; return s; }

// ---- GET /api/diag: why the last boot happened, crash summary, update state --------------
static void handleDiag() {
    if (!hostOk()) { fail(403, "forbidden-request"); return; }
    const radio::RxDiagnostics r = radio::rxDiagnostics();
    String o; o.reserve(900);
    char b[300];
    snprintf(b, sizeof(b), "{\"fw\":\"%s\",\"variant\":\"%s\",\"uptime_s\":%lu,\"reset_reason\":\"%s\",\"boots\":%lu,"
             "\"previous_uptime_s\":%ld,\"heap_free\":%lu,\"heap_min\":%lu,\"running\":\"%s\",\"running_state\":\"%s\",",
             FW_VERSION, variantOfThisBuild(), (unsigned long)(millis() / 1000), resetName(esp_reset_reason()),
             (unsigned long)g_crumb.boots, g_prevKnown ? long(g_prevUptimeS) : -1L, (unsigned long)ESP.getFreeHeap(),
             (unsigned long)ESP.getMinFreeHeap(), esp_ota_get_running_partition()->label, stateName(esp_ota_get_running_partition()));
    o += b;
    snprintf(b, sizeof(b), "\"rx\":{\"state\":%u,\"frames\":%lu,\"recoveries\":%u,\"lost\":%lu,\"uart_overflows\":%lu,\"queue_drops\":%lu},",
             r.rxState, (unsigned long)r.frames, r.rxRecoveries, (unsigned long)r.rxLost, (unsigned long)r.uartOverflows, (unsigned long)r.queueDrops);
    o += b;
    net::wifiDiag(b, sizeof(b));
    o += b;
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) == ESP_OK && size) {
        esp_core_dump_summary_t s;
        if (esp_core_dump_get_summary(&s) == ESP_OK) {
            snprintf(b, sizeof(b), "\"coredump\":{\"size\":%u,\"task\":\"%.15s\",\"pc\":\"0x%08lx\",\"cause\":%lu,\"vaddr\":\"0x%08lx\",\"backtrace\":[",
                     (unsigned)size, s.exc_task, (unsigned long)s.exc_pc, (unsigned long)s.ex_info.exc_cause, (unsigned long)s.ex_info.exc_vaddr);
            o += b;
            for (uint32_t i = 0; i < s.exc_bt_info.depth && i < 16; ++i) { snprintf(b, sizeof(b), "%s\"0x%08lx\"", i ? "," : "", (unsigned long)s.exc_bt_info.bt[i]); o += b; }
            o += "]}";
        } else { snprintf(b, sizeof(b), "\"coredump\":{\"size\":%u,\"summary\":false}", (unsigned)size); o += b; }
    } else o += "\"coredump\":null";
    o += "}";
    g_s->sendHeader("Cache-Control", "no-store");
    g_s->send(200, "application/json", o);
}

// ---- loop watchdog (Codex r5) ---------------------------------------------------------------------------------------
// Arduino 2.0.16's WebServer can loop forever inside handleClient() on a malformed multipart request, before any route
// code runs. A stuck loop would stop the trial-boot confirmation, the 300 s rollback and a scheduled restart. This task
// (core 0, above the loop's priority) restarts the board when the loop stops beating: after 20 s while a trial boot, a
// selected image or a scheduled restart waits (a restart in a trial boot is a rollback), after 120 s otherwise.
static std::atomic<uint32_t> g_beat{0};
void heartbeat() { g_beat = millis(); }
static void loopWatch(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        // The heartbeat first, then the clock: a beat from the other core can never be "newer" than the time it is
        // compared with (an unsigned wrap would restart a healthy board; Codex r6). Checked twice before acting.
        const uint32_t limit = g_pending || g_selected || net::restartQueued() ? 20000u : 120000u;
        const uint32_t beat = g_beat.load();
        const uint32_t still = millis() - beat;
        if (still <= limit) continue;
        const uint32_t beat2 = g_beat.load();             // again in this order, as separate statements (Codex r7)
        const uint32_t still2 = millis() - beat2;
        if (beat2 == beat && still2 > limit) {            // no beat in between: the loop is really stuck
            Serial.printf("[ota] main loop stuck for %lu s: restart\n", (unsigned long)(still / 1000));
            esp_restart();
        }
    }
}

void begin(WebServer& s) {
    g_s = &s;
    heartbeat();
    // Boot breadcrumb in RTC memory: no flash wear, survives everything but power loss.
    if (g_crumb.magic == 0x4F524331u) { g_prevKnown = true; g_prevUptimeS = g_crumb.lastUptimeS; ++g_crumb.boots; }
    else { g_crumb.magic = 0x4F524331u; g_crumb.boots = 1; }
    g_crumb.lastUptimeS = 0;
    const esp_partition_t* run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    g_pending = esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
    // After the trial state is known, so the watchdog never starts with the wrong limit (Codex r6).
    if (xTaskCreatePinnedToCore(loopWatch, "loop_watch", 2048, nullptr, 2, nullptr, 0) != pdPASS) Serial.println("[ota] loop watchdog not started");
    note(rfcon::INFO, "opgestart: %s, reden %s, vorige sessie %ld s, %s%s", run->label, resetName(esp_reset_reason()),
         g_prevKnown ? long(g_prevUptimeS) : -1L, variantOfThisBuild(), g_pending ? ", proefstart na OTA" : "");
    s.on("/api/ota",        HTTP_GET,  handleInfo);
#if ORCON_OWNER_MODEL
    // Writing firmware needs the owner's session and CSRF token besides the signature. Builds without the ownership
    // model (the old PoC portals) have no network writer at all: the signing key alone never installs anything there
    // (Codex legacy r2). Those boards are installed over USB.
    s.on("/api/ota/nonce",  HTTP_POST, handleNonce);
    s.on("/api/ota/begin",  HTTP_POST, handleBegin);
    s.on("/api/ota/upload", HTTP_POST, handleUploadDone, handleUploadData);
#endif
#if !ORCON_TX_POC || ORCON_WEB_CONTROL
    s.on("/api/ota/hold",   HTTP_POST, handleHold);
#endif
    s.on("/api/diag",       HTTP_GET,  handleDiag);
}

// This firmware has no password reset with the firmware signing key. An older image may still have it for a
// board without an owner key: an update on such a board is never confirmed, so the previous image (and its
// recovery) returns until the owner registers a key (docs/HERSTELSLEUTEL_MIGRATIE.md). Read again before every
// confirmation attempt: a passing read error does not force a rollback, a key that became unreadable is noticed.
static bool ownerKeyRegistered() {
#if ORCON_OWNER_MODEL
    uint8_t q[65];
    // Also the recovery's own crypto self-test: a key this image could not use would be no way back (Codex legacy r2).
    const bool ok = ownerstore::state() == provision::State::Done && ownerstore::ownerKey(q) && ownerstore::cryptoReady();
    static bool told = false;
    if (!ok && !told) {
        told = true;
        note(rfcon::ERR, "geen herstelsleutel geregistreerd: deze firmware wordt niet bevestigd en de vorige keert terug. "
                         "Registreer eerst een herstelsleutel in de herstelmodus (PRG-menu).");
    }
    return ok;
#else
    return true;                                            // no ownership model: no owner key to require
#endif
}

void tick(uint32_t now) {
    heartbeat();
    static uint32_t lastCrumb = 0;
    if (now - lastCrumb >= 5000) { lastCrumb = now; g_crumb.lastUptimeS = now / 1000; }
    if (g_nonce[0] && now - g_nonceMs >= NONCE_TTL_MS) g_nonce[0] = 0;   // a 49.7-day millis() wrap must not revive it
    if (!g_pending) return;
    // Healthy = this image runs, joined the network (so a later OTA can still reach it) and the receiver is up. It is
    // confirmed only if the owner also has a recovery key; otherwise the previous image returns (otap::trialStep).
    static uint32_t lastTry = 0;
    const bool healthy = !g_hold && net::wifiConnected() && radio::rxDiagnostics().rxState == 0;
    switch (otap::trialStep(now, healthy, lastTry, CONFIRM_AFTER_MS, ROLLBACK_AFTER_MS)) {
        case otap::Trial::TryConfirm:
            if (ownerKeyRegistered() && esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                g_pending = false; note(rfcon::INFO, "nieuwe firmware bevestigd (gezond na %lu s)", (unsigned long)(now / 1000));
            }
            return;
        case otap::Trial::Rollback:   // unhealthy, no owner key, or a confirmation that keeps failing: the previous image returns
            note(rfcon::ERR, "nieuwe firmware niet bevestigd binnen %lu s: terug naar de vorige", (unsigned long)(ROLLBACK_AFTER_MS / 1000));
            rollbackNow();
            return;
        case otap::Trial::Wait:
            return;
    }
}

void rollbackNow() {
    delay(200);
    const esp_err_t e = esp_ota_mark_app_invalid_rollback_and_reboot();   // returns only on failure
    // No usable previous otadata entry: restart anyway. The bootloader then marks this
    // PENDING_VERIFY image ABORTED and starts another bootable slot.
    Serial.printf("[ota] rollback via otadata failed (0x%x): restart, bootloader aborts this image\n", (unsigned)e);
    delay(100);
    esp_restart();
}

// The other slot holds a confirmed (valid) image of the ownership model with signed OTA: only then may an unconfirmed
// image that cannot read the ownership storage return to it by itself (net.cpp; OWNER-10 never allows a return to
// a pre-owner image). Scans the slot's bytes for the two markers, as the OTA policy does for an upload.
bool previousIsOwnerModel() {
    const esp_partition_t* p = esp_ota_get_next_update_partition(nullptr);
    esp_ota_img_states_t st;
    if (!p || esp_ota_get_state_partition(p, &st) != ESP_OK || st != ESP_OTA_IMG_VALID) return false;
    esp_image_metadata_t meta;   // the bootable image itself, not stale bytes after a smaller one (Codex r5)
    const esp_partition_pos_t pos{p->address, p->size};
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) != ESP_OK) return false;
    otap::Scanner s("");
    uint8_t buf[1024];
    for (uint32_t off = 0; off < meta.image_len; off += sizeof(buf)) {
        const uint32_t n = meta.image_len - off < sizeof(buf) ? meta.image_len - off : sizeof(buf);
        if (esp_partition_read(p, off, buf, n) != ESP_OK) return false;
        s.feed(buf, n);
    }
    return s.ownerModel && s.signedOta && !s.legacyOta;
}

}  // namespace ota
#endif  // ORCON_SIGNED_OTA
