// PLACEHOLDER Matter identity: no keys, no codes. A build without a device's own identity (tools/matter_identity.sh,
// ORCON_MATTER_IDENTITY in tools/build_idf.sh) compiles with this file and runs Orcon, but Matter refuses to start
// (state "failed", step "no-identity"): such an image never confirms a trial boot and rolls back by itself.
#pragma once
#include <stdint.h>
#define ORCON_MATTER_PLACEHOLDER_IDENTITY 1
namespace orcon_matter_creds {
constexpr uint16_t kVendorId = 0xFFF1, kProductId = 0x8000, kDiscriminator = 0;
constexpr uint32_t kPasscode = 0, kIterations = 1000;
static const uint8_t kSalt[16] = {0}, kVerifier[97] = {0}, kDacCert[1] = {0}, kPaiCert[1] = {0}, kCd[1] = {0};
static const uint8_t kDacPublic[65] = {0}, kDacPrivate[32] = {0};
static const char kManualCode[] = "", kQrCode[] = "";
}  // namespace orcon_matter_creds
