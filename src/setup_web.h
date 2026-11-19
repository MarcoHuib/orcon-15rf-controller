// setup_web.h — setup wizard page ("/") and its API (/api/setup/*, /api/remote/*).
// In TX-PoC web-control builds (ORCON_WEB_CONTROL) the pair/remote endpoints drive
// the txpoc control layer instead of the legacy orcon command path.
#pragma once
#include <stdint.h>
#include "config.h"
#include "ramses_codec.h"

class WebServer;

namespace setupweb {
void restoreProfileSelection(); // after NVS load; routing only, never TX or pairing
void begin(WebServer& server);                                    // routes + per-boot token
void onFrame(const ramses::Frame& f, uint32_t seq, int16_t rssi);  // every decoded RX frame (main loop)
void note(uint8_t kind, const char* msg);                        // console line from another module (rfcon kind)
void tick(uint32_t now);                                           // timeouts (main loop, never in GET)
// For routes registered elsewhere (net.cpp settings): host and, for a mutation, same origin and
// JSON; then a logged-in session (with its CSRF token for a mutation). Sends the refusal itself.
// Builds without the web login: always true (unchanged legacy behaviour).
bool authorize(bool mutation, bool allowAp = false);
bool uploadAuthorized();   // a multipart upload: own host, a logged-in session and its CSRF token (no reply)
#if ORCON_WEB_CONTROL
void consumeTxResults(uint32_t now); // before draining RX, so prompt bind replies are not lost
// Web-control pairing just completed with this unit: consume once so net.cpp can
// persist it. Returns false when there is nothing new.
bool takeLearnedFan(ramses::DeviceId& out);
#endif
}
