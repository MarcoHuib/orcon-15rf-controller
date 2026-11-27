#pragma once
// This device's own Matter identity, kept ON THE DEVICE instead of in the firmware image (docs/MATTER.md), so one
// firmware fits every device and can be published:
//   - attestation: the DAC key, the DAC and PAI certificates (and the PAA's), the certification declaration
//   - commissioning: passcode, discriminator, SPAKE2+ iteration count, salt and verifier; the serial number
// in the NVS namespace "orcon-id" of Matter's own NVS (orcon_matter_nvs.cpp; Orcon's NVS is untouched, and CHIP's own
// factory reset clears only its chip-config, chip-counters and KVS). Made once on the device (generate(), the owner's
// "Matter activeren"), or copied once from an image built with an identity (migrateEmbedded(), the move of an existing
// device). A SHA-256 over every field, written last, makes the record whole or absent: an interrupted write is never
// used, a changed byte is "damaged" (and never silently replaced: that would be a new device for every controller).
// No hardware protects these bytes: this classic ESP32 runs without flash encryption (the bootloader stays v1.0.1's),
// so whoever reads the flash over USB reads the key, exactly as with an image that carries one.
#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/core/CHIPError.h>
#include <lib/support/Span.h>

namespace orcon_matter {
namespace identity {
enum class State : uint8_t { Unknown, Absent, Valid, Damaged };
enum class Origin : uint8_t { None = 0, Generated = 1, Migrated = 2 };
State load();                    // after matterNvsBegin(): read and verify the stored record
State state();
Origin origin();
const char* stateName();
bool embeddedPresent();          // this image was built with an identity (a private migration build)
bool embeddedCodesMatch();       // after a migration: the codes made from the stored values equal the embedded ones
esp_err_t generate();            // a new identity, made on this device (only while none is valid)
esp_err_t migrateEmbedded();     // the image's own identity, stored once (only while none is valid)
esp_err_t erase();               // the whole record
// What the providers and the owner's page read (only while Valid)
uint32_t passcode();
uint16_t discriminator();
uint32_t iterations();
const char* serial();
CHIP_ERROR read(const char* key, chip::MutableByteSpan& out);   // a stored blob, into the caller's buffer
const char* qrCode();            // from the stored values ("" before)
const char* manualCode();
extern const char kDacKey[], kDacPub[], kDacCert[], kPaiCert[], kPaaCert[], kCd[], kSalt[], kVerifier[];
struct GenStats { uint32_t ms, heapBefore, heapAfter, heapMinAfter; };   // the last generate(): time and RAM
GenStats lastGenStats();
}  // namespace identity
}  // namespace orcon_matter
