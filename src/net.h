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
}
