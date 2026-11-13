// ota_signed.h — signed OTA updates and remote diagnostics over WiFi (Heltec V2).
#pragma once
#include <stdint.h>
class WebServer;
namespace ota {
void begin(WebServer& server);   // /api/ota, /api/ota/nonce, /api/ota/begin, /api/ota/upload, /api/diag
void tick(uint32_t now);         // health check of a freshly installed image; rollback when unhealthy
}
