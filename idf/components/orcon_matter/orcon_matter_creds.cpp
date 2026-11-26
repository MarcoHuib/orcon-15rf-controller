// The Matter identity CHIP uses comes from the device itself (orcon_matter_identity.cpp: made on the device, or moved
// there once from a migration build), never the SDK's public test set: an own DAC chain and certification declaration,
// a random passcode and discriminator with its SPAKE2+ verifier. Each blob is read from NVS into the caller's buffer
// when CHIP asks (attestation and commissioning only), so it costs no RAM while Matter runs. Set before
// esp_matter::start() (CONFIG_CUSTOM_DAC_PROVIDER, CONFIG_CUSTOM_COMMISSIONABLE_DATA_PROVIDER).
#include "orcon_matter.h"
#include "orcon_matter_identity.h"

#include <crypto/CHIPCryptoPAL.h>
#include <credentials/DeviceAttestationCredsProvider.h>
#include <esp_matter_providers.h>
#include <lib/support/Span.h>
#include <mbedtls/platform_util.h>
#include <platform/CommissionableDataProvider.h>

namespace {
namespace id = orcon_matter::identity;
using chip::ByteSpan;
using chip::MutableByteSpan;

struct Dac : chip::Credentials::DeviceAttestationCredentialsProvider {
    CHIP_ERROR GetCertificationDeclaration(MutableByteSpan& out) override { return id::read(id::kCd, out); }
    CHIP_ERROR GetFirmwareInformation(MutableByteSpan& out) override { out.reduce_size(0); return CHIP_NO_ERROR; }
    CHIP_ERROR GetDeviceAttestationCert(MutableByteSpan& out) override { return id::read(id::kDacCert, out); }
    CHIP_ERROR GetProductAttestationIntermediateCert(MutableByteSpan& out) override { return id::read(id::kPaiCert, out); }
    CHIP_ERROR SignWithDeviceAttestationKey(const ByteSpan& msg, MutableByteSpan& out) override {   // as the SDK example
        chip::Crypto::P256ECDSASignature sig;
        chip::Crypto::P256Keypair key;
        VerifyOrReturnError(!out.empty() && !msg.empty(), CHIP_ERROR_INVALID_ARGUMENT);
        VerifyOrReturnError(out.size() >= sig.Capacity(), CHIP_ERROR_BUFFER_TOO_SMALL);
        uint8_t priv[32], pub[65];
        MutableByteSpan privSpan(priv), pubSpan(pub);
        CHIP_ERROR err = id::read(id::kDacKey, privSpan);
        if (err == CHIP_NO_ERROR) err = id::read(id::kDacPub, pubSpan);
        if (err == CHIP_NO_ERROR) err = key.HazardousOperationLoadKeypairFromRaw(privSpan, pubSpan);
        mbedtls_platform_zeroize(priv, sizeof priv);   // the key lives in RAM only for this signature
        ReturnErrorOnFailure(err);
        ReturnErrorOnFailure(key.ECDSA_sign_msg(msg.data(), msg.size(), sig));
        return chip::CopySpanToMutableSpan(ByteSpan{sig.ConstBytes(), sig.Length()}, out);
    }
};

struct Commissionable : chip::DeviceLayer::CommissionableDataProvider {
    CHIP_ERROR GetSetupDiscriminator(uint16_t& d) override { d = id::discriminator(); return valid(); }
    CHIP_ERROR SetSetupDiscriminator(uint16_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }   // fixed per device
    CHIP_ERROR GetSpake2pIterationCount(uint32_t& n) override { n = id::iterations(); return valid(); }
    CHIP_ERROR GetSpake2pSalt(MutableByteSpan& out) override { return id::read(id::kSalt, out); }
    CHIP_ERROR GetSpake2pVerifier(MutableByteSpan& out, size_t& len) override {
        const CHIP_ERROR err = id::read(id::kVerifier, out);
        len = err == CHIP_NO_ERROR ? out.size() : 0;
        return err;
    }
    CHIP_ERROR GetSetupPasscode(uint32_t& p) override { p = id::passcode(); return valid(); }
    CHIP_ERROR SetSetupPasscode(uint32_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
    static CHIP_ERROR valid() { return id::state() == id::State::Valid ? CHIP_NO_ERROR : CHIP_ERROR_INCORRECT_STATE; }
};

Dac g_dac;
Commissionable g_commissionable;
}  // namespace

namespace orcon_matter {
void setCredentialProviders() {
    esp_matter::set_custom_dac_provider(&g_dac);
    esp_matter::set_custom_commissionable_data_provider(&g_commissionable);
}
bool identityPresent() { return id::state() == id::State::Valid; }   // Matter runs only with a valid own identity
const char* manualCode() { return id::manualCode(); }
const char* qrCode() { return id::qrCode(); }
}  // namespace orcon_matter
