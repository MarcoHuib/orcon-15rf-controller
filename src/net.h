// net.h — WiFi + web UI + MQTT + OLED glue.
#pragma once
#include <stdint.h>

namespace net {
void begin();
void loop();
void publishState();          // force an MQTT state publish + OLED refresh
void notifyStateChanged();     // mark state dirty (publish on next loop)

// accessors for the on-screen System page
const char* ipAddr();
const char* ssidName();
bool        apMode();
bool        wifiConnected();  // station mode and associated with the configured network
const char* mqttHost();
bool        mqttConnected();
bool        maskIds();        // privacy setting: mask device IDs in the UI
void        restartSoon();    // reboot after the HTTP reply has gone out
void        forgetWifi();     // drop the stored network; the next boot starts the setup AP

// Ownership model (ORCON_OWNER_MODEL builds; docs/PROVISIONING.md)
bool        fromSetupAp();    // this request arrived on the setup AP (socket addresses, not headers)
bool        staUp();          // the station is associated, or a WiFi test is running
bool        apOnly();         // setup AP without a station: installing, or a PRG recovery boot
bool        wifiTestStart(const char* ssid, const char* pass);   // the AP stays up; stored only on success
const char* wifiTestState();  // idle|testing|connected|wrong-password|not-found|failed
const char* wifiTestIp();
const char* storedSsid();
bool        ensureOwnIdentity();   // a random radio identity at the first pairing (control image)
}
