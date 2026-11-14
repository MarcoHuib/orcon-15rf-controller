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
#include "ota_pubkey.h"
#include "net.h"
#include "ramses_radio.h"
#include "setup_web.h"
#include "rf_console.h"
#include "tx_poc_caps.h"
#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>
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

// Survives soft resets, panics and watchdogs (not power loss): previous uptime and boots.
struct Crumb { uint32_t magic, boots, lastUptimeS; };
static RTC_NOINIT_ATTR Crumb g_crumb;
static uint32_t g_prevUptimeS = 0;
static bool g_prevKnown = false, g_pending = false;
static bool g_hold = false;   // rollback test: this trial boot refuses to confirm itself (RAM only)

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
    char b[512];
    snprintf(b, sizeof(b),
             "{\"signed_ota\":\"%s\",\"fw\":\"%s\",\"variant\":\"%s\",\"running\":\"%s\",\"running_state\":\"%s\","
             "\"boot\":\"%s\",\"next\":\"%s\",\"next_state\":\"%s\",\"slot_size\":%lu,\"session\":\"%s\",\"last_error\":\"%s\","
             "\"nonce_active\":%s,\"hold\":%s}",
             otap::SIGNED_OTA_MARKER, FW_VERSION, variantOfThisBuild(), run ? run->label : "?", stateName(run),
             boot ? boot->label : "?", next ? next->label : "?", stateName(next), (unsigned long)(next ? next->size : 0),
             g_st == St::Idle ? "idle" : g_st == St::Ready ? "ready" : g_st == St::Writing ? "writing" : g_st == St::Done ? "done" : "failed",
             g_err, g_nonce[0] && millis() - g_nonceMs < NONCE_TTL_MS ? "true" : "false", g_hold ? "true" : "false");
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
static void handleNonce() {
    if (!hostOk()) { fail(403, "forbidden-request"); return; }
    if (g_st == St::Writing) { fail(409, "update-busy"); return; }
    if (g_pending) { fail(409, "confirm-running-image-first"); return; }
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

// ---- POST /api/ota/begin {"manifest":"<json text>","sig":"<DER hex>"} --------------------
static void handleBegin() {
    if (!hostOk()) { fail(403, "forbidden-request"); return; }
    if (g_st == St::Writing) { fail(409, "update-busy"); return; }
    if (g_pending) { fail(409, "confirm-running-image-first"); return; }
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
    g_st = St::Idle;
    if (!verifySignature(text, strlen(text), sig)) { g_err = "bad-signature"; note(rfcon::WARN, "OTA geweigerd: ongeldige handtekening"); fail(403, g_err); return; }
    otap::Manifest m;
    const char* e = otap::parseManifest(text, m);
    char mac[18]; ownMac(mac);
    g_part = esp_ota_get_next_update_partition(nullptr);
    if (!e) e = otap::checkManifest(m, mac, fresh ? nonce : "", g_part ? g_part->size : 0);
    if (e) { g_err = e; note(rfcon::WARN, "OTA geweigerd: %s", e); fail(409, e); return; }
    g_m = m; g_scan = otap::Scanner(m.commit); g_st = St::Ready; g_err = "none";
    note(rfcon::INFO, "OTA klaar voor upload: %s %.7s (%lu B) naar %s", otap::variantName(m.variant), m.commit,
         (unsigned long)m.size, g_part->label);
    char b[160]; snprintf(b, sizeof(b), "{\"ok\":true,\"partition\":\"%s\",\"size\":%lu,\"variant\":\"%s\"}",
                          g_part->label, (unsigned long)m.size, otap::variantName(m.variant));
    reply(200, b);
}

// ---- POST /api/ota/upload (multipart, one file) -----------------------------------------
static void abortWrite(const char* e) {
    if (g_st == St::Writing) esp_ota_abort(g_h);
    g_st = St::Failed; g_err = e;
}
static void handleUploadData() {
    HTTPUpload& up = g_s->upload();
    if (up.status == UPLOAD_FILE_START) {
        if (g_st != St::Ready || !hostOk()) { g_st = St::Failed; g_err = "not-ready"; return; }
        if (esp_ota_begin(g_part, OTA_WITH_SEQUENTIAL_WRITES, &g_h) != ESP_OK) { g_st = St::Failed; g_err = "ota-begin-failed"; return; }
        mbedtls_sha256_init(&g_sha); mbedtls_sha256_starts_ret(&g_sha, 0);
        g_rx = 0; g_st = St::Writing;
        note(rfcon::INFO, "OTA schrijft %s (RF-ontvangst kan tijdens het schrijven haperen)", g_part->label);
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (g_st != St::Writing) return;
        if (g_rx + up.currentSize > g_m.size) { abortWrite("image-too-large"); return; }
        if (esp_ota_write(g_h, up.buf, up.currentSize) != ESP_OK) { abortWrite("flash-write-failed"); return; }
        mbedtls_sha256_update_ret(&g_sha, up.buf, up.currentSize);
        g_scan.feed(up.buf, up.currentSize);
        g_rx += up.currentSize;
    } else if (up.status == UPLOAD_FILE_END) {
        if (g_st != St::Writing) return;
        uint8_t hash[32]; mbedtls_sha256_finish_ret(&g_sha, hash); mbedtls_sha256_free(&g_sha);
        const char* e = otap::checkImage(g_m, g_scan, g_rx, hash);
        if (e) { abortWrite(e); return; }
        // ESP-IDF validates the image (header, segments, appended SHA-256) before it can boot.
        if (esp_ota_end(g_h) != ESP_OK) { g_st = St::Failed; g_err = "image-invalid"; return; }
        if (esp_ota_set_boot_partition(g_part) != ESP_OK) { g_st = St::Failed; g_err = "set-boot-failed"; return; }
        g_st = St::Done;
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        abortWrite("upload-aborted");
    }
}
static void handleUploadDone() {
    if (g_st == St::Writing) abortWrite("upload-incomplete");
    if (g_st != St::Done) {
        note(rfcon::WARN, "OTA mislukt: %s (opstartpartitie ongewijzigd)", g_err);
        fail(g_st == St::Failed && !strcmp(g_err, "not-ready") ? 409 : 422, g_err); g_st = St::Idle; return;
    }
    note(rfcon::INFO, "OTA geschreven en geverifieerd: herstart naar %s (proefstart met rollback)", g_part->label);
    char b[128]; snprintf(b, sizeof(b), "{\"ok\":true,\"restarting\":true,\"boot\":\"%s\"}", g_part->label);
    g_s->sendHeader("Connection", "close");
    reply(200, b);
    g_st = St::Idle;
    net::restartSoon();
}

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

void begin(WebServer& s) {
    g_s = &s;
    // Boot breadcrumb in RTC memory: no flash wear, survives everything but power loss.
    if (g_crumb.magic == 0x4F524331u) { g_prevKnown = true; g_prevUptimeS = g_crumb.lastUptimeS; ++g_crumb.boots; }
    else { g_crumb.magic = 0x4F524331u; g_crumb.boots = 1; }
    g_crumb.lastUptimeS = 0;
    const esp_partition_t* run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    g_pending = esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
    note(rfcon::INFO, "opgestart: %s, reden %s, vorige sessie %ld s, %s%s", run->label, resetName(esp_reset_reason()),
         g_prevKnown ? long(g_prevUptimeS) : -1L, variantOfThisBuild(), g_pending ? ", proefstart na OTA" : "");
    s.on("/api/ota",        HTTP_GET,  handleInfo);
    s.on("/api/ota/nonce",  HTTP_POST, handleNonce);
    s.on("/api/ota/begin",  HTTP_POST, handleBegin);
    s.on("/api/ota/upload", HTTP_POST, handleUploadDone, handleUploadData);
#if !ORCON_TX_POC || ORCON_WEB_CONTROL
    s.on("/api/ota/hold",   HTTP_POST, handleHold);
#endif
    s.on("/api/diag",       HTTP_GET,  handleDiag);
}

void tick(uint32_t now) {
    static uint32_t lastCrumb = 0;
    if (now - lastCrumb >= 5000) { lastCrumb = now; g_crumb.lastUptimeS = now / 1000; }
    if (g_nonce[0] && now - g_nonceMs >= NONCE_TTL_MS) g_nonce[0] = 0;   // a 49.7-day millis() wrap must not revive it
    if (!g_pending) return;
    // Healthy = this image runs, joined the network (so a later OTA can still reach it) and
    // the receiver is up. Only then is it confirmed; otherwise the previous image returns.
    const bool healthy = !g_hold && now >= CONFIRM_AFTER_MS && net::wifiConnected() && radio::rxDiagnostics().rxState == 0;
    if (healthy) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) { g_pending = false; note(rfcon::INFO, "nieuwe firmware bevestigd (gezond na %lu s)", (unsigned long)(now / 1000)); }
    } else if (now >= ROLLBACK_AFTER_MS) {
        note(rfcon::ERR, "nieuwe firmware niet gezond binnen %lu s: terug naar de vorige", (unsigned long)(ROLLBACK_AFTER_MS / 1000));
        delay(200);
        const esp_err_t e = esp_ota_mark_app_invalid_rollback_and_reboot();   // returns only on failure
        // No usable previous otadata entry: restart anyway. The bootloader then marks this
        // PENDING_VERIFY image ABORTED and starts another bootable slot.
        Serial.printf("[ota] rollback via otadata failed (0x%x): restart, bootloader aborts this image\n", (unsigned)e);
        delay(100);
        esp_restart();
    }
}

}  // namespace ota
#endif  // ORCON_SIGNED_OTA
