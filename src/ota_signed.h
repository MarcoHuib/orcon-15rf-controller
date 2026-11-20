// ota_signed.h — signed OTA updates and remote diagnostics over WiFi (Heltec V2).
#pragma once
#include <stddef.h>
#include <stdint.h>
class WebServer;
namespace fwup { struct Sink; }
namespace ota {
void begin(WebServer& server);   // /api/ota, /api/ota/nonce, /api/ota/begin, /api/ota/upload, /api/diag
void tick(uint32_t now);         // health check of a freshly installed image; rollback when unhealthy
// Also used by the release update check (fw_update_esp.cpp). Never for a password: the web login does not trust this key.
void ownMac(char out[18]);       // lower-case "02:00:00:00:00:0a" (WiFi STA MAC)
bool verifySignature(const char* text, size_t n, const char* sigHex);   // ECDSA P-256 over SHA-256(text)
// The one-click update (fw_update_esp.cpp) uses the writer of /api/ota/begin + upload, for v2 releases only.
bool pendingVerify();            // the running image is still in its trial boot (not yet confirmed)
bool writerBusy();               // an image is being written (either path) or waits, selected, for the restart
uint32_t updateSlotSize();       // size of the slot the next image goes to
const char* claimDownload();     // loop task: nullptr = the download owns the writer (manual routes say update-busy)
void endDownload();              // the worker, when done: the writer is free again
void rollbackNow();              // mark the running (trial) image invalid and restart into the previous one
bool previousIsOwnerModel();     // the other slot holds a confirmed image of the ownership model
void heartbeat();                // the main loop is alive (ota::tick, upload chunks); see the loop watchdog
fwup::Sink& releaseSink();       // signature + manifest + slot checks, then hash/scan/write; finish selects the slot
}
