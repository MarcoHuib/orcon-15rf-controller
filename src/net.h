// net.h — WiFi + web UI + OLED glue.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace net {
void begin();
void loop();
void publishState();          // refresh the OLED now (also every 5 s and after a state change)
void notifyStateChanged();     // mark state dirty (OLED refresh on the next loop)

// accessors for the on-screen System page
const char* ipAddr();
const char* ssidName();
bool        apMode();
bool        wifiConnected();  // station mode and associated with the configured network
void        wifiDiag(char* out, size_t n);   // /api/diag: "wifi":{signal, address, outages, last reason},
bool        maskIds();        // privacy setting: mask device IDs in the UI
void        restartSoon();    // reboot after the HTTP reply has gone out
void        forgetWifi();     // drop the stored network; the next boot starts the setup AP

// Ownership model (ORCON_OWNER_MODEL builds; docs/PROVISIONING.md)
bool        fromSetupAp();    // this request arrived on the setup AP (socket addresses, not headers)
bool        fromHomeNet();    // this request came over the station from the home network's own subnet (Siri)
bool        staUp();          // the station is associated, or a WiFi test is running
bool        apOnly();         // setup AP without a station: installing, or a PRG recovery boot
bool        wifiTestStart(const char* ssid, const char* pass);   // the AP stays up; stored only on success
const char* wifiTestState();  // idle|testing|connected|wrong-password|not-found|failed
const char* wifiTestIp();
const char* storedSsid();
bool        ensureOwnIdentity();   // a random radio identity at the first pairing (control image)
}
