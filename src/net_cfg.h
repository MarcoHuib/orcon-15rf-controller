// net_cfg.h — MQTT on/off rules (pure, host-tested in test/test_netcfg.cpp).
// NVS "mqtt": key absent -> MQTT_HOST compile default ("" on RX-only = off);
//             ""         -> MQTT explicitly off;  "<host>" -> MQTT on.
#pragma once
#include <string.h>

inline bool mqttHostValid(const char* h) {      // hostname or IPv4; no scheme, port or spaces
    size_t n = strlen(h);
    if (n == 0 || n > 63 || h[0] == '.' || h[0] == '-') return false;
    for (size_t i = 0; i < n; i++) {
        char c = h[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-'))
            return false;
    }
    return true;
}

enum class MqttSave { Keep, Off, Host, Invalid };

// /api/wifi. mqtt_on given: explicit (off, or on with a valid host). Not given
// (older page/client): a non-empty valid host is saved, blank keeps the current value.
inline MqttSave mqttSaveAction(bool hasOn, bool on, const char* host) {
    if (hasOn && !on) return MqttSave::Off;
    if (!host[0]) return hasOn ? MqttSave::Invalid : MqttSave::Keep;
    return mqttHostValid(host) ? MqttSave::Host : MqttSave::Invalid;
}
