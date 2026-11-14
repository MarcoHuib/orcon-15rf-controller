// owner_store.h — the device side of ownership (ESP32 only; the rules live in provision.h and the
// key file format in owner_key.h, both host-tested):
//   - the installation state and the owner's public key in NVS ("orconown");
//   - the key-file primitives on mbedTLS and the hardware RNG, behind a known-answer self-test;
//   - the button-started recovery mode (RTC memory: this boot only, a power cut cancels it);
//   - the factory reset (marker in NVS "orconreset", then provision::resume at the next boot).
#pragma once
#include <stdint.h>
#include "owner_key.h"
#include "provision.h"

namespace ownerstore {
void begin();                                    // at boot, after resumeFactoryReset(): state (+ migration), boot mode
provision::State state();
bool markSetup();                                // wizard step 1, written BEFORE the first password
bool ownerKey(uint8_t q[65]);                    // the registered public key
bool registerKey(const uint8_t q[65]);           // write + read back, then state Done
const ownerkey::Crypto& crypto();
bool cryptoReady();                              // known-answer self-test passed; otherwise fail closed
bool generate(uint8_t d[32], uint8_t q[65]);     // a fresh P-256 key from the hardware RNG

enum class Mode : uint8_t { None, Recovery, WifiReset };
void armMode(Mode);                              // for the next boot only
Mode bootMode();                                 // what the PRG menu started this boot for
bool physicalActive();                           // a PRG-menu boot, within provision::PHYSICAL_MS
uint32_t physicalLeftMs();

bool trialBoot();                                // the running image still awaits its OTA confirmation
bool requestFactoryReset();                      // the confirmed PRG gesture: sets the marker only
bool resumeFactoryReset(bool& ran);              // at boot, before anything else reads NVS
}  // namespace ownerstore
