#pragma once
#include <stdint.h>
#include <esp_err.h>
#include <sdkconfig.h>   // CONFIG_ORCON_PROBE_CONTROL below
namespace chip { namespace DeviceLayer { struct ChipDeviceEvent; } }
namespace orcon_matter {
void earlyBegin();   // once, at the start of Orcon's setup(): the Matter mailbox and the telemetry task (+ heap monitor)
esp_err_t begin();   // called once, right after Orcon's net::begin(); never blocks the Orcon loop
// The pieces begin() uses, also for a QEMU probe that runs Matter over Ethernet without Orcon:
esp_err_t createModel();   // the bridged fan "Orcon ventilator" (CONFIG_ORCON_MATTER_BRIDGE), or five switches
void onChipEvent(const chip::DeviceLayer::ChipDeviceEvent* e, intptr_t);   // the esp_matter::start() event callback
bool serverReadySeen();   // CHIP posted kServerReady (server and DNS-SD up) this boot
void memWebPublish(const char* line);   // B1-web: the telemetry task's last [mem] line for GET /api/mem
// Running Matter, its state and its identity
bool matterReady();                     // Matter runs: CHIP's server is up and Wi-Fi is Orcon's
const char* matterStateName();          // waiting | starting | ready | failed
int stateJson(char* out, size_t n);     // public GET /api/matter/state: state, step, err, confirm refusals
void setCredentialProviders();          // this controller's own DAC and setup data (orcon_matter_creds.cpp)
bool identityPresent();                 // false: built with the placeholder identity, Matter stays off
const char* manualCode();               // its pairing codes, for the owner's /matter page only
const char* qrCode();
// Orcon's loop task only: Matter requests to Orcon's admission, the unit's own state back to Matter
void serviceControl(uint32_t nowMs);
int controlJson(char* out, size_t n);   // the last request and its result, for the owner's GET /api/matter
int fanCheckJson(char* out, size_t n);  // the fan endpoint's first look this boot (new endpoint, stale On/Off)
// The root NodeLabel default, once and only while no label is stored (CHIP task, stack locked; at kServerReady)
const char* applyDefaultNodeLabel();
#if CONFIG_ORCON_PROBE_CONTROL
void probeStarting();                   // QEMU probe only: begin()'s Matter state steps (orcon_matter.cpp)
void probeHandedOver();
#endif
}
class WebServer;
namespace orcon_matter {
void pairingWebBegin(WebServer& s);     // /connect and /api/connect* (orcon_matter_pairing.cpp)
void activationService();               // Orcon's loop task: restart once the owner's activation made an identity
}
