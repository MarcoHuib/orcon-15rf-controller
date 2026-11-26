// This controller's own Matter identity (tools/matter_identity.sh, orcon_matter_creds.h outside the repository) instead of the SDK's
// public test set: own DAC chain and certification declaration, a random passcode/discriminator with its SPAKE2+
// verifier. Set before esp_matter::start() (CONFIG_CUSTOM_DAC_PROVIDER, CONFIG_CUSTOM_COMMISSIONABLE_DATA_PROVIDER).
#include "orcon_matter.h"
#include "orcon_matter_creds.h"

#include <crypto/CHIPCryptoPAL.h>
#include <credentials/DeviceAttestationCredsProvider.h>
#include <esp_matter_providers.h>
#include <lib/support/Span.h>
#include <platform/CommissionableDataProvider.h>

namespace {
using namespace orcon_matter_creds;
using chip::ByteSpan;
using chip::MutableByteSpan;

struct Dac : chip::Credentials::DeviceAttestationCredentialsProvider {
    CHIP_ERROR GetCertificationDeclaration(MutableByteSpan& out) override { return chip::CopySpanToMutableSpan(ByteSpan(kCd), out); }
    CHIP_ERROR GetFirmwareInformation(MutableByteSpan& out) override { out.reduce_size(0); return CHIP_NO_ERROR; }
    CHIP_ERROR GetDeviceAttestationCert(MutableByteSpan& out) override { return chip::CopySpanToMutableSpan(ByteSpan(kDacCert), out); }
    CHIP_ERROR GetProductAttestationIntermediateCert(MutableByteSpan& out) override {
        return chip::CopySpanToMutableSpan(ByteSpan(kPaiCert), out);
    }
    CHIP_ERROR SignWithDeviceAttestationKey(const ByteSpan& msg, MutableByteSpan& out) override {   // as the SDK example
        chip::Crypto::P256ECDSASignature sig;
        chip::Crypto::P256Keypair key;
        VerifyOrReturnError(!out.empty() && !msg.empty(), CHIP_ERROR_INVALID_ARGUMENT);
        VerifyOrReturnError(out.size() >= sig.Capacity(), CHIP_ERROR_BUFFER_TOO_SMALL);
        ReturnErrorOnFailure(key.HazardousOperationLoadKeypairFromRaw(ByteSpan(kDacPrivate), ByteSpan(kDacPublic)));
        ReturnErrorOnFailure(key.ECDSA_sign_msg(msg.data(), msg.size(), sig));
        return chip::CopySpanToMutableSpan(ByteSpan{sig.ConstBytes(), sig.Length()}, out);
    }
};

struct Commissionable : chip::DeviceLayer::CommissionableDataProvider {
    CHIP_ERROR GetSetupDiscriminator(uint16_t& d) override { d = kDiscriminator; return CHIP_NO_ERROR; }
    CHIP_ERROR SetSetupDiscriminator(uint16_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }   // fixed per device
    CHIP_ERROR GetSpake2pIterationCount(uint32_t& n) override { n = kIterations; return CHIP_NO_ERROR; }
    CHIP_ERROR GetSpake2pSalt(MutableByteSpan& out) override { return chip::CopySpanToMutableSpan(ByteSpan(kSalt), out); }
    CHIP_ERROR GetSpake2pVerifier(MutableByteSpan& out, size_t& len) override {
        len = sizeof(kVerifier);
        return chip::CopySpanToMutableSpan(ByteSpan(kVerifier), out);
    }
    CHIP_ERROR GetSetupPasscode(uint32_t& p) override { p = kPasscode; return CHIP_NO_ERROR; }
    CHIP_ERROR SetSetupPasscode(uint32_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
};

Dac g_dac;
Commissionable g_commissionable;
}  // namespace

namespace orcon_matter {
void setCredentialProviders() {
    esp_matter::set_custom_dac_provider(&g_dac);
    esp_matter::set_custom_commissionable_data_provider(&g_commissionable);
}
bool identityPresent() {   // the placeholder (no own identity) never runs Matter
#ifdef ORCON_MATTER_PLACEHOLDER_IDENTITY
    return false;
#else
    return true;
#endif
}
const char* manualCode() { return kManualCode; }
const char* qrCode() { return kQrCode; }
}  // namespace orcon_matter
