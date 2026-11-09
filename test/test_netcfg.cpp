// test_netcfg.cpp — MQTT on/off decisions for /api/wifi (net_cfg.h).
#include "net_cfg.h"
#include <cstdio>
static int fails = 0;
#define CHECK(c,m) do{ if(!(c)){printf("  [FAIL] %s\n",m);fails++;} else printf("  [ ok ] %s\n",m);}while(0)
int main() {
    printf("MQTT config tests\n");
    CHECK(mqttSaveAction(true, false, "") == MqttSave::Off, "mqtt_on=false -> off (blank host)");
    CHECK(mqttSaveAction(true, false, "192.168.1.10") == MqttSave::Off, "mqtt_on=false -> off even with a host typed");
    CHECK(mqttSaveAction(true, true, "") == MqttSave::Invalid, "mqtt_on=true with blank host -> refused (no silent default)");
    CHECK(mqttSaveAction(true, true, "192.0.2.2") == MqttSave::Host, "mqtt_on=true with IPv4 -> saved");
    CHECK(mqttSaveAction(true, true, "broker.lan") == MqttSave::Host, "mqtt_on=true with hostname -> saved");
    CHECK(mqttSaveAction(false, false, "") == MqttSave::Keep, "old client, blank -> keep current (unchanged behaviour)");
    CHECK(mqttSaveAction(false, false, "10.0.0.5") == MqttSave::Host, "old client, host -> saved (unchanged behaviour)");
    const char* bad[] = { "mqtt://broker", "broker:1883", "a b", "-x", ".x", "x\"y",
                          "0123456789012345678901234567890123456789012345678901234567890123" };
    bool allBad = true;
    for (const char* b : bad) if (mqttSaveAction(true, true, b) != MqttSave::Invalid) { printf("    accepted: %s\n", b); allBad = false; }
    CHECK(allBad, "scheme, port, spaces, leading ./-, quotes, >63 chars -> refused");
    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails ? 1 : 0;
}
