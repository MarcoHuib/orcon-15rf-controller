// setup_web.h — setup wizard page ("/") and its API (/api/setup/*, /api/remote/*).
// Not compiled into the TX-PoC route table (net.cpp registers it outside ORCON_TX_POC).
#pragma once
#include <stdint.h>
#include "ramses_codec.h"

class WebServer;

namespace setupweb {
void begin(WebServer& server);                                    // routes + per-boot token
void onFrame(const ramses::Frame& f, uint32_t seq, int16_t rssi);  // every decoded RX frame (main loop)
void tick(uint32_t now);                                           // timeouts (main loop, never in GET)
}
