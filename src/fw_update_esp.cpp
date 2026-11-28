// fw_update_esp.cpp — see fw_update_esp.h. The control image asks GitHub for the newest official release and, only
// on the logged-in owner's explicit request, streams it into the signed-OTA writer (ota_signed.cpp).
//
// Threads: the routes and tick() run in the Arduino loop task. A check or an install runs in a worker task created for
// that one job (core 0, priority 1, below WiFi/lwIP; the RX task and the loop stay on core 1) and deleted afterwards, so
// a TLS handshake or a slow download never blocks the web page or RF reception. The worker never touches the RF
// console or the web server: it reports through g_st (under g_mux), and tick() logs and restarts in the loop task.
#include "config.h"
#if ORCON_UPDATE_CHECK
#include "fw_update_esp.h"
#include "fw_update.h"
#if ORCON_MATTER
#include "fw_update_roots_cb.h"   // the roots on demand: less TLS RAM next to Matter
#else
#include "fw_update_roots.h"
#endif
#include "ota_signed.h"
#include "ota_policy.h"
#include "net.h"
#include "setup_web.h"
#include "rf_console.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>   // the beta channel setting
#include <esp_attr.h>        // RTC_NOINIT_ATTR (the update restart without Matter)
#include <esp_heap_caps.h>
#include <esp_system.h>      // esp_reset_reason()
#include <new>
#include <atomic>

namespace updsvc {

enum class Phase : uint8_t { Idle, Checking, Downloading, Written, Failed };
enum class Job : uint8_t { Check, Install };
struct Status {
    Phase phase = Phase::Idle;
    Job job = Job::Check;
    fwup::Found found = fwup::Found::None;
    const char* err = "";          // static strings only (fw_update.cpp, ota_signed.cpp)
    bool checked = false;          // a check has finished since boot (ok or failed)
    uint32_t checkedAt = 0;        // millis() when it finished
    uint32_t done = 0, total = 0;  // download progress (image bytes)
    uint32_t heapMin = 0;          // lowest free heap seen during the last job (TLS + download)
    uint32_t blockMin = 0;         // smallest "largest free block" seen then (TLS needs 16 KB in one piece)
    uint32_t stackFree = 0;        // worker stack never used during the last job (bytes)
    bool automatic = false;
};
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static Status g_st;
static fwup::Offer g_offer;               // the release of the last successful check (found != None)
// Shared with the worker task: sequentially consistent atomics (Codex r4).
static std::atomic<bool> g_running{false};    // a worker task exists
static std::atomic<bool> g_finished{false};   // the worker finished a job; tick() reports it
static std::atomic<bool> g_install{false};    // the running job is an install (RF commands wait; a check does not block them)
static std::atomic<bool> g_restart{false};    // an image was written: busy until the restart into its trial boot
static std::atomic<uint32_t> g_writtenAt{0};  // stored before g_restart
static WebServer* g_s = nullptr;
static bool g_beta = false;   // the owner's beta channel (NVS "orcon"/"beta", read in begin(); loop task)
static uint32_t g_lastStart = 0;
static bool g_started = false, g_restartSent = false;

// Matter builds: no automatic GitHub check next to Matter until its TLS peak fits with the agreed 20 KB reserve.
// In the firmware, not a setting: no install, restart or NVS state can turn it back on. "Controleer op updates" (via
// one restart without Matter) and the local uploads (.orconfw page, board-bound OTA) stay. Other builds: unchanged.
static const bool AUTO_CHECK = !ORCON_MATTER;
static const uint32_t FIRST_CHECK_MS = 180000;             // 3 minutes after boot (after a trial boot is confirmed)
static const uint32_t EVERY_MS = 24UL * 3600UL * 1000UL;   // then once a day
static const uint32_t RETRY_MS = 3600UL * 1000UL;          // after a failed automatic check: once an hour
static const uint32_t MANUAL_GAP_MS = 60000;               // "Controleer op updates": at most once a minute
static const uint32_t RESTART_AFTER_WRITE_MS = 3000;       // the page sees "geschreven" before the restart
static const uint32_t STACK = 12288;

#define LOCKED(stmt) do { portENTER_CRITICAL(&g_mux); stmt; portEXIT_CRITICAL(&g_mux); } while (0)

// ---- TLS transport: WiFiClientSecure, chain verified against the pinned roots, host name checked (SNI) -----------
// Never the insecure mode: WiFiClientSecure gets the CA buffer FWUP_ROOTS_PEM, so mbedTLS verification is
// VERIFY_REQUIRED. Matter builds (ESP-IDF 5, mbedTLS 3) offer the same nine roots through mbedTLS's trusted-CA
// callback instead (fw_update_roots_cb.h: on demand, not parsed up front; -17.3 KB at the TLS peak).
// Known limit: this core's mbedTLS is built without MBEDTLS_HAVE_TIME_DATE, so certificate dates are not checked
// (docs/UPDATECONTROLE.md); the release signature check after the download does not depend on TLS.
static void sampleHeap();
// Byte-addressable internal RAM: what TLS, lwIP and Matter allocate from. ESP.getFreeHeap() also counts the IRAM-only
// heap (51 KB on v1.0.1, 77 KB with Matter on IDF 5), which none of them can use, so the gate measured nothing there.
static uint32_t free8() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
// The Matter build defers update jobs while a controller commissions the device (orcon_matter); others never do.
extern "C" bool orcon_fwup_defer(void) __attribute__((weak));
static const char* roomForTls() {
    if (orcon_fwup_defer && orcon_fwup_defer()) return "matter-busy";
    return fwup::memoryOk(free8(), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}
// The owner's restart for an update check next to Matter (fwup::updateBootRefusal / updateBootDone). One flag in
// RTC memory that survives esp_restart() only: read and cleared once per boot, so a crash, a watchdog reset or a power
// cut in that boot brings Matter back, and the update boot itself never asks for another one (never a loop).
struct UpdateBootFlag { uint32_t magic, check; };
RTC_NOINIT_ATTR static UpdateBootFlag g_ubFlag;
static const uint32_t UB_MAGIC = 0x4F55424Fu;
static int8_t g_updateBoot = -1;   // -1: not read yet this boot
bool updateBoot() {   // loop task (orcon_matter::begin() asks before Matter would start, tick() afterwards)
    if (g_updateBoot < 0) {
        g_updateBoot = g_ubFlag.magic == UB_MAGIC && g_ubFlag.check == ~UB_MAGIC && esp_reset_reason() == ESP_RST_SW;
        g_ubFlag.magic = g_ubFlag.check = 0;
    }
    return g_updateBoot == 1;
}
struct TlsTransport : fwup::Transport {
#if ORCON_MATTER
    rootscb::Client c;
    static constexpr const char* kRoots = nullptr;   // the callback supplies them
#else
    WiFiClientSecure c;
    static constexpr const char* kRoots = FWUP_ROOTS_PEM;
#endif
    const char* err = "connect-failed";
    bool connect(const char* host, uint32_t ms) override {
        c.stop();
        if (!net::wifiConnected()) { err = "no-network"; return false; }
        if (const char* e = roomForTls()) { err = e; return false; }   // again before each of the three handshakes
        const uint32_t t0 = millis();
        IPAddress ip;
        if (!WiFi.hostByName(host, ip) || ip == IPAddress((uint32_t)0)) { err = "dns-failed"; return false; }
        const uint32_t used = millis() - t0;   // DNS has its own wait: what is left goes to TCP and TLS (Codex r4)
        if (used + 1000 > ms) { err = "timeout"; return false; }
        c.setHandshakeTimeout((ms - used) / 1000);
        c.setTimeout((ms - used) / 1000);
        if (!c.connect(ip, 443, host, kRoots, nullptr, nullptr) || millis() - t0 > ms) {   // TCP + TLS within the step too (Codex r5)
            if (c.connected()) { c.stop(); err = "timeout"; sampleHeap(); return false; }
            char b[8];
            err = c.lastError(b, sizeof(b)) < -1 ? "tls-failed" : "connect-failed";   // -1: TCP connect or handshake timeout
            sampleHeap();
            return false;
        }
        sampleHeap();   // the session's buffers are allocated now
        return true;
    }
    bool write(const char* d, size_t n) override {
        while (n) {
            const size_t k = c.write((const uint8_t*)d, n);
            if (!k || k > n) return false;
            d += k; n -= k;
        }
        return true;
    }
    uint32_t lastYield = 0;
    int read(uint8_t* b, size_t n, uint32_t ms) override {
        // A fast download never starves core 0's idle task (task watchdog: 5 s, panic). Flash erases yield too
        // (CONFIG_SPI_FLASH_YIELD_DURING_ERASE); this does not depend on that.
        if (millis() - lastYield >= 100) { vTaskDelay(1); lastYield = millis(); sampleHeap(); }
        const uint32_t t0 = millis();
        for (;;) {
            const int a = c.available();
            if (a > 0) { const int k = c.read(b, n < size_t(a) ? n : size_t(a)); return k > 0 ? k : -2; }
            if (a < 0 || !c.connected()) return 0;
            if (millis() - t0 >= ms) return -1;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    void stop() override { c.stop(); }
    const char* connectError() override { return err; }
};

static uint32_t clockMs() { return millis(); }
static void sampleHeap() {   // during the TLS session too (transport connect/read), not only after it (Codex r3)
    const uint32_t f = free8(), b = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    LOCKED(if (f < g_st.heapMin) g_st.heapMin = f; if (b < g_st.blockMin) g_st.blockMin = b);
}
static void progress(uint32_t done, uint32_t total) { sampleHeap(); LOCKED(g_st.done = done; g_st.total = total); }

struct Ctx { fwup::Url u; fwup::Head h; uint8_t io[4096]; fwup::Offer offer; Job job; bool beta; };   // beta: the owner's channel

#if ORCON_MATTER   // a Matter board is only offered Matter releases (and a board without Matter only those without)
static constexpr otap::Variant kOwnVariant = otap::Variant::ControlMatter;
static constexpr const char* kOwnEnv = "heltec_v2_matter";
#else
static constexpr otap::Variant kOwnVariant = otap::Variant::Control;
static constexpr const char* kOwnEnv = "heltec_v2_sx1276_control";
#endif
static void worker(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    {
        TlsTransport t;
        fwup::Limits lim;
        if (c->job == Job::Check) {
            const fwup::Own own = {ORCON_RELEASE_VERSION, kOwnVariant, kOwnEnv, ota::updateSlotSize(), ORCON_RELEASE_PRE, c->beta};
            fwup::Found f = fwup::Found::None;
            const char* e = fwup::check(t, ota::verifySignature, clockMs, lim, own, c->offer, f, (char*)c->io, sizeof(c->io), c->u, c->h);
            sampleHeap();
            LOCKED(g_st.found = e ? fwup::Found::None : f; if (!e && f != fwup::Found::None) g_offer = c->offer;
                   g_st.err = e ? e : ""; g_st.phase = e ? Phase::Failed : Phase::Idle; g_st.checked = true; g_st.checkedAt = millis());
        } else {
            const char* e = fwup::install(t, ota::releaseSink(), clockMs, lim, c->offer, progress, c->io, sizeof(c->io), c->u, c->h);
            ota::endDownload();
            LOCKED(g_st.err = e ? e : ""; g_st.phase = e ? Phase::Failed : Phase::Written);
            if (!e) { g_writtenAt = millis(); g_restart = true; }   // before g_running drops: never a gap for an RF command
        }
        t.stop();
    }
    const uint32_t stackFree = uxTaskGetStackHighWaterMark(nullptr);   // bytes on ESP-IDF
    LOCKED(g_st.stackFree = stackFree);
    delete c;
    g_finished = true;
    g_running = false;
    vTaskDelete(nullptr);
}

// Starts a job, or says why not. Loop task only (routes and tick).
static const char* startJob(Job j, bool automatic, uint32_t want = 0, const char* commit = "") {
    if (g_restart || net::restartQueued()) return "restart-pending";
    if (g_running) return "update-busy";
    if (ota::pendingVerify()) return "confirm-running-image-first";
    if (!net::wifiConnected()) return "no-network";
    if (const char* e = roomForTls()) return e;
    Ctx* c = new (std::nothrow) Ctx;
    if (!c) return "low-memory";
    c->job = j;
    c->beta = g_beta;   // the channel as the owner set it when the job starts (the worker never reads the setting)
    if (j == Job::Install) {
        bool same = false;
        LOCKED(c->offer = g_offer; same = g_st.found == fwup::Found::Newer && g_offer.m.version == want && !strcmp(g_offer.m.commit, commit));
        if (!same || !want) { delete c; return "no-update-offered"; }   // exactly the release the owner approved
        if (const char* e = ota::claimDownload()) { delete c; return e; }
    }
    const uint32_t total = j == Job::Install ? c->offer.m.size : 0;
    LOCKED(g_st.phase = j == Job::Check ? Phase::Checking : Phase::Downloading; g_st.job = j; g_st.err = ""; g_st.done = 0;
           g_st.total = total; g_st.heapMin = free8(); g_st.blockMin = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
           g_st.automatic = automatic);
    g_install = j == Job::Install; g_running = true; g_finished = false;
    g_lastStart = millis(); g_started = true;
    if (xTaskCreatePinnedToCore(worker, "fw_update", STACK, c, 1, nullptr, 0) != pdPASS) {
        g_running = false;
        if (j == Job::Install) ota::endDownload();
        delete c;
        LOCKED(g_st.phase = Phase::Failed; g_st.err = "low-memory");
        return "low-memory";
    }
    return nullptr;
}

// Either install path: the one-click download or its restart, and a manual upload that writes or waits for its
// restart (Codex r3: RF commands, Siri, pairing, reads and settings reboots wait for both).
bool busy() { return (g_running && g_install) || g_restart || ota::writerBusy(); }

// ---- routes ------------------------------------------------------------------------------------------------------------
static void reply(int code, const String& body) {
    g_s->sendHeader("Cache-Control", "no-store");
    g_s->send(code, "application/json", body);
}
static void fail(int code, const char* e) { char b[96]; snprintf(b, sizeof(b), "{\"error\":\"%s\"}", e); reply(code, b); }
static int codeOf(const char* e) {
    return !strcmp(e, "no-network") || !strcmp(e, "low-memory") ? 503 : 409;   // restart-pending, update-busy, ...: 409
}
static void quoted(String& o, const char* s) {   // notes are validated text (no control characters); escape JSON
    o += '"';
    for (; *s; ++s) { if (*s == '"' || *s == '\\') o += '\\'; o += *s; }
    o += '"';
}
static const char* phaseName(Phase p) {
    return p == Phase::Checking ? "checking" : p == Phase::Downloading ? "downloading" : p == Phase::Written ? "written" :
           p == Phase::Failed ? "failed" : "idle";
}
static void statusBody(int code) {
    Status s; fwup::Offer o;
    portENTER_CRITICAL(&g_mux); s = g_st; if (s.found != fwup::Found::None) o = g_offer; portEXIT_CRITICAL(&g_mux);
    char cur[24], b[384];   // two fields for the update restart and the channel (update_boot, channel); the longest line is ~370
    fwup::semverTextPre(ORCON_RELEASE_VERSION, ORCON_RELEASE_PRE, cur);
    String out; out.reserve(900);
    bool whole = true;   // a piece that does not fit makes the answer an error, never broken JSON
    const auto add = [&](int k) { if (k >= 0 && size_t(k) < sizeof(b)) out += b; else whole = false; };
    add(snprintf(b, sizeof(b), "{\"supported\":true,\"repo\":\"%s\",\"current\":{\"release\":%lu,\"name\":\"%s\",\"fw\":\"%s\",\"variant\":\"%s\"},"
             "\"phase\":\"%s\",\"job\":\"%s\",\"error\":\"%s\",\"automatic\":%s,\"checked\":%s,\"checked_age_s\":%ld,\"found\":\"%s\","
             "\"update_boot\":%s,\"channel\":\"%s\",",
             ORCON_RELEASE_REPO, (unsigned long)ORCON_RELEASE_VERSION, cur, FW_VERSION, otap::variantName(kOwnVariant), phaseName(s.phase),
             s.job == Job::Install ? "install" : "check", s.err, s.automatic ? "true" : "false", s.checked ? "true" : "false",
             s.checked ? long((millis() - s.checkedAt) / 1000) : -1L,
             s.found == fwup::Found::Newer ? "newer" : s.found == fwup::Found::UpToDate ? "up-to-date" : "none",
             updateBoot() ? "true" : "false", g_beta ? "beta" : "stable"));
    if (s.found != fwup::Found::None) {
        char name[24]; fwup::semverTextPre(o.m.version, o.m.pre, name);
        add(snprintf(b, sizeof(b), "\"latest\":{\"release\":%lu,\"pre\":%u,\"name\":\"%s\",\"commit\":\"%s\",\"size\":%lu,\"notes\":",
                     (unsigned long)o.m.version, unsigned(o.m.pre), name, o.m.commit, (unsigned long)o.m.size));
        quoted(out, o.notes); out += "},";
    } else out += "\"latest\":null,";
    add(snprintf(b, sizeof(b), "\"progress\":{\"done\":%lu,\"total\":%lu},\"mem\":{\"heap_free\":%lu,\"heap_min_job\":%lu,\"block_min_job\":%lu,"
             "\"stack_free_job\":%lu}}", (unsigned long)s.done, (unsigned long)s.total, (unsigned long)free8(), (unsigned long)s.heapMin,
             (unsigned long)s.blockMin, (unsigned long)s.stackFree));
    if (!whole) { fail(500, "answer-too-long"); return; }
    reply(code, out);
}
static void handleStatus() { if (!setupweb::authorize(false)) return; statusBody(200); }
static void handleCheck() {
    if (!setupweb::authorize(true)) return;   // owner session, CSRF token, same origin, home network
    if (g_s->arg("plain").length() > 2) { fail(400, "bad-json"); return; }
    if (g_running) { fail(409, "update-busy"); return; }
    if (g_started && millis() - g_lastStart < MANUAL_GAP_MS) {
        char b[80]; snprintf(b, sizeof(b), "{\"error\":\"too-soon\",\"retry_s\":%lu}", (unsigned long)((MANUAL_GAP_MS - (millis() - g_lastStart) + 999) / 1000));
        reply(429, b); return;
    }
    if (const char* e = startJob(Job::Check, false)) { fail(codeOf(e), e); return; }
    statusBody(202);
}
// {"version":N,"commit":"<40 hex>"}: exactly the build the owner saw in the last check (Codex r3: a later check could
// offer another signed build with the same number). Nothing else (no URL, no file) is accepted.
static void handleInstall() {
    if (!setupweb::authorize(true)) return;
    const String body = g_s->arg("plain");
    JsonDocument d;
    if (body.length() > 128 || deserializeJson(d, body) || !d.is<JsonObject>() || d.as<JsonObject>().size() != 2 ||
        !d["version"].is<uint32_t>() || !d["commit"].is<const char*>() || strlen(d["commit"].as<const char*>()) != 40) { fail(400, "bad-json"); return; }
    const uint32_t want = d["version"].as<uint32_t>();
    char commit[41]; strcpy(commit, d["commit"].as<const char*>());
    // No worker first: only a worker changes the offer, and only the loop task starts one, so from here on the offer
    // the owner approved cannot be replaced (Codex r2: a check finishing between these lines). startJob checks again.
    if (g_running) { fail(409, "update-busy"); return; }
    bool offered = false;
    LOCKED(offered = g_st.found == fwup::Found::Newer && g_offer.m.version == want && !strcmp(g_offer.m.commit, commit));
    if (!offered) { fail(409, "no-update-offered"); return; }
    if (setupweb::busy()) { fail(409, "session-busy"); return; }   // a read/pair session or an RF command in flight
    if (const char* e = startJob(Job::Install, false, want, commit)) { fail(codeOf(e), e); return; }
    setupweb::note(rfcon::INFO, "update: download van de officiële release gestart (op verzoek van de eigenaar)");
    statusBody(202);
}

// POST /api/update/restart {}: the owner's one restart without Matter, only when the check cannot run next to it.
static void handleUpdateRestart() {
    if (!setupweb::authorize(true)) return;
    if (g_s->arg("plain").length() > 2) { fail(400, "bad-json"); return; }
    const fwup::UpdateBootAsk a{orcon_fwup_defer != nullptr,
        !fwup::memoryOk(free8(), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        g_running.load(), g_restart.load() || net::restartQueued(), ota::pendingVerify(),
        orcon_fwup_defer && orcon_fwup_defer(), setupweb::busy()};
    if (const char* e = fwup::updateBootRefusal(a)) { fail(409, e); return; }
    g_ubFlag.magic = UB_MAGIC; g_ubFlag.check = ~UB_MAGIC;
    setupweb::note(rfcon::INFO, "update: herstart zonder Matter voor de updatecontrole (op verzoek van de eigenaar)");
    reply(202, "{\"restart\":\"update-check\"}");
    net::restartSoon();
}

// POST /api/update/channel {"beta":true|false}: the owner's "Bèta-updates ontvangen". Switching drops the last
// offer, so an install always follows a check on the channel now chosen. Back to stable never installs an older
// release (releaseRank): a beta stays until a stable release at least as high.
bool betaEnabled() { return g_beta; }
static void handleChannel() {
    if (!setupweb::authorize(true)) return;
    const String body = g_s->arg("plain");
    JsonDocument d;
    if (body.length() > 32 || deserializeJson(d, body) || !d.is<JsonObject>() || d.as<JsonObject>().size() != 1 ||
        !d["beta"].is<bool>()) { fail(400, "bad-json"); return; }
    if (g_running || busy() || net::restartQueued()) { fail(409, "update-busy"); return; }
    const bool beta = d["beta"].as<bool>();
    if (beta == g_beta) { statusBody(200); return; }
    Preferences p;
    if (!p.begin("orcon", false) || p.putBool("beta", beta) != 1) { p.end(); fail(503, "storage-unavailable"); return; }
    p.end();
    g_beta = beta;
    // The previous check was for another feed. Keep the check rate limit, but drop its result and timestamp.
    g_finished = false;
    LOCKED(g_st = Status{});
    setupweb::note(rfcon::INFO, beta ? "update: bèta-updates aan (op verzoek van de eigenaar)" : "update: bèta-updates uit");
    statusBody(200);
}

void begin(WebServer& s) {
    g_s = &s;
    { Preferences p; if (p.begin("orcon", true)) { g_beta = p.getBool("beta", false); p.end(); } }   // absent: stable
    s.on("/api/update/restart", HTTP_POST, handleUpdateRestart);
    s.on("/api/update/channel", HTTP_POST, handleChannel);
    s.on("/api/update",         HTTP_GET,  handleStatus);
    s.on("/api/update/check",   HTTP_POST, handleCheck);
    s.on("/api/update/install", HTTP_POST, handleInstall);
}

void tick(uint32_t now) {
    if (g_finished) {   // the worker ended: log here (the RF console is not thread-safe), restart after a written update
        g_finished = false;
        Status s; fwup::Offer o;
        portENTER_CRITICAL(&g_mux); s = g_st; o = g_offer; portEXIT_CRITICAL(&g_mux);
        char name[24]; fwup::semverTextPre(o.m.version, o.m.pre, name);
        char m[112];
        if (s.job == Job::Check) {
            if (s.phase == Phase::Failed) snprintf(m, sizeof(m), "updatecontrole mislukt: %s", s.err);
            else if (s.found == fwup::Found::Newer) snprintf(m, sizeof(m), "updatecontrole: nieuwe officiële versie %s beschikbaar", name);
            else snprintf(m, sizeof(m), "updatecontrole: geen nieuwere officiële versie");
            setupweb::note(s.phase == Phase::Failed ? rfcon::WARN : rfcon::INFO, m);
        } else if (s.phase == Phase::Written) {
            snprintf(m, sizeof(m), "update %s gedownload en gecontroleerd: herstart naar een proefstart met rollback", name);
            setupweb::note(rfcon::INFO, m);
        } else {
            snprintf(m, sizeof(m), "update mislukt: %s (opstartpartitie ongewijzigd)", s.err);
            setupweb::note(rfcon::WARN, m);
        }
    }
    if (g_restart) {   // stays busy until the restart: no RF command and no new job in between
        (void)now;   // sampled after g_restart was seen: g_writtenAt is never in the "future" (Codex r4)
        if (!g_restartSent && millis() - g_writtenAt.load() >= RESTART_AFTER_WRITE_MS) { g_restartSent = true; net::restartSoon(); }
        return;
    }
    if (updateBoot()) {   // Matter is off this boot only, for the owner's check; then back to the normal start
        if (!g_started && net::wifiConnected()) { g_started = true; g_lastStart = now; startJob(Job::Check, false); }
        bool checked = false, newer = false; uint32_t at = 0;
        LOCKED(checked = g_st.checked; newer = g_st.found == fwup::Found::Newer; at = g_st.checkedAt);
        if (!g_restartSent && fwup::updateBootDone(now, checked, newer, at, g_running.load() || net::restartQueued() || busy() || setupweb::busy())) {
            g_restartSent = true;
            setupweb::note(rfcon::INFO, "update: terug naar de normale start met Matter");
            net::restartSoon();
        }
        return;
    }
    if (g_running || !AUTO_CHECK) return;
    bool failedLast = false;
    LOCKED(failedLast = g_st.phase == Phase::Failed && g_st.job == Job::Check);
    const bool due = !g_started ? now >= FIRST_CHECK_MS : now - g_lastStart >= (failedLast ? RETRY_MS : EVERY_MS);
    if (!due || !net::wifiConnected() || ota::pendingVerify() || setupweb::busy()) return;
    if (startJob(Job::Check, true)) { g_lastStart = now; g_started = true; }   // could not start: try again later, never in a loop
}

}  // namespace updsvc
#elif ORCON_SIGNED_OTA
// Builds without the update check still have the manual signed upload: the same interlock (Codex r4).
#include "fw_update_esp.h"
#include "ota_signed.h"
namespace updsvc { bool busy() { return ota::writerBusy(); } }
#endif  // ORCON_UPDATE_CHECK
