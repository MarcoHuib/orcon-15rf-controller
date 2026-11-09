// rx_only.h — what an RX-only build still accepts on /api/cmd and MQTT.
// Pure header (host-tested in test_orcon.cpp).
#pragma once
#include <string.h>

// Keys that only change local UI/telemetry settings. Everything else would
// transmit, pair, or persist TX "keeper" state, so the whole request is refused.
inline bool rxOnlyAllowedCmd(const char* key) {
    static const char* const OK[] = { "maskids", "showfilter", "roomidx" };
    for (const char* k : OK) if (!strcmp(key, k)) return true;
    return false;
}
