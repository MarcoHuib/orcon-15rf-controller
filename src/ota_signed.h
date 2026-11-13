// ota_signed.h — signed OTA updates and remote diagnostics over WiFi (Heltec V2).
#pragma once
#include <stddef.h>
#include <stdint.h>
class WebServer;
namespace ota {
void begin(WebServer& server);   // /api/ota, /api/ota/nonce, /api/ota/begin, /api/ota/upload, /api/diag
void tick(uint32_t now);         // health check of a freshly installed image; rollback when unhealthy
// Shared with the web login's recovery request (auth_store.cpp): the same OTA public key.
void ownMac(char out[18]);       // lower-case "02:00:00:00:00:0a" (WiFi STA MAC)
bool verifySignature(const char* text, size_t n, const char* sigHex);   // ECDSA P-256 over SHA-256(text)
}
