// fw_update_esp.h — the release update check and one-click install on the Heltec (control image only).
// Logic: fw_update.h (host-tested). This is the device glue: a short-lived worker task, TLS with pinned roots
// (fw_update_roots.h), the routes and the automatic check schedule. docs/UPDATECONTROLE.md.
#pragma once
#include <stdint.h>
#include "config.h"
class WebServer;
namespace updsvc {
#if ORCON_UPDATE_CHECK
void begin(WebServer& server);   // GET /api/update, POST /api/update/check, POST /api/update/install
void tick(uint32_t now);         // main loop: automatic checks, console notes, restart after a written update
bool busy();                     // a check or a download runs: no RF commands, no restarts
#else
inline bool busy() { return false; }
#endif
}
