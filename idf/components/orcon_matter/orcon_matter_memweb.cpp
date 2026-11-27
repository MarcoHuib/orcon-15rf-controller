// Diagnostics: GET /api/mem serves the telemetry task's last "[mem]" line over WiFi, so the RAM
// measurement needs no serial cable. Read-only and behind Orcon's own web login: setupweb::authorize(false) checks
// this board's Host header and a logged-in session, exactly as for GET /api/netcfg, and sends the 403/401 itself.
// Orcon's sources stay byte-identical: the route is added by a link-time wrap of ota::begin(WebServer&), which
// net::begin() calls before server.begin() (CMakeLists.txt).
//
// Reply (text/plain, one line): the [mem] line exactly as printed on serial (internal 8-bit heap free/min/largest,
// the monitor's lows lo8/loblk/minblk, allocation failures, task stack high-water marks, NVS use), followed by
//   boot=<random per boot>  binds every reading to its boot session, also when a restart falls in a polling gap
//   web_req=<replies served this boot, this one included>
//   web_route=<internal 8-bit heap the route registration took, B>
//   web_low=<lowest internal 8-bit free seen by this route, at entry or after sending, B>
//   web_drop=<largest free drop between entry and after sending, B> (earlier replies: measured after sending)
// The route's static cost (code, buffer) is measured from the ELF, not here.
// 503 until the first line exists (one telemetry period after boot).
#include "orcon_matter.h"

#include <WebServer.h>
#include <algorithm>
#include <atomic>
#include <stdio.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace setupweb { bool authorize(bool mutation, bool allowAp); }   // Orcon's src/setup_web.h

namespace {
char g_line[768];                  // = telemetryTask's line buffer: the last [mem] line, NUL-terminated
size_t g_len = 0;                  // only under the claim below
std::atomic<uint8_t> g_owner{0};   // try-lock: 0 free, 1 telemetry task writing, 2 web reply reading
WebServer* g_srv = nullptr;
uint32_t g_boot = 0, g_req = 0, g_low = UINT32_MAX, g_drop = 0;   // loop task only (registration, replies)
int32_t g_route = -1;

uint32_t free8() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
bool claim(uint8_t who) { uint8_t z = 0; return g_owner.compare_exchange_strong(z, who); }

void handleMem() {
    const uint32_t in = free8();
    if (!setupweb::authorize(false, false)) return;
    bool got = false;
    for (int i = 0; i < 5 && !(got = claim(2)); ++i) vTaskDelay(1);   // the writer holds it for a memcpy only
    if (!got || !g_len) {
        if (got) g_owner.store(0);
        g_srv->send(503, "text/plain", "no [mem] line yet\n");
        return;
    }
    g_low = std::min(g_low, in);
    char tail[128];
    orcon_matter::Out t{tail, sizeof tail};   // its length is checked: never more than tail holds
    t.fmt(" boot=%lu web_req=%lu web_route=%ld web_low=%lu web_drop=%lu\n", (unsigned long)g_boot,
          (unsigned long)(g_req + 1), (long)g_route, (unsigned long)g_low, (unsigned long)g_drop);
    g_srv->sendHeader("Cache-Control", "no-store");
    g_srv->setContentLength(g_len + t.n);
    g_srv->send_P(200, "text/plain", g_line, g_len);   // straight from the buffer: no copy on the heap
    g_srv->sendContent(tail, t.n);
    g_owner.store(0);
    const uint32_t out = free8();
    ++g_req;
    g_low = std::min(g_low, out);
    if (in > out) g_drop = std::max(g_drop, in - out);
}
}  // namespace

namespace orcon_matter {
void memWebPublish(const char* line) {   // telemetry task, right after printing the line
    if (!claim(1)) return;                // a reply is reading the previous line: this period stays serial-only
    g_len = strnlen(line, sizeof g_line - 1);
    memcpy(g_line, line, g_len);
    g_line[g_len] = 0;
    g_owner.store(0);
}
}  // namespace orcon_matter

extern "C" void __real__ZN3ota5beginER9WebServer(WebServer& s);
extern "C" void __wrap__ZN3ota5beginER9WebServer(WebServer& s) {
    __real__ZN3ota5beginER9WebServer(s);
    g_srv = &s;
    g_boot = esp_random() | 1u;   // Wi-Fi already runs here (net::begin), so a true random number; never 0
    const uint32_t before = free8();
    s.on("/api/mem", HTTP_GET, handleMem);
    g_route = int32_t(before) - int32_t(free8());
    orcon_matter::pairingWebBegin(s);   // the owner's Matter page and routes (/connect)
}
