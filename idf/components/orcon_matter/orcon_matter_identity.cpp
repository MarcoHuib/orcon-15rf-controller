// This device's own Matter identity in NVS (orcon_matter_identity.h, docs/MATTER.md).
// generate(): an own chain made here, with ESP-IDF's mbedTLS for the X.509 certificates and CHIP for the rest:
//   PAA (self-signed) -> PAI -> DAC, each checked against CHIP's own attestation profile, the chain and the DAC key before
//   anything is stored; a certification declaration (CHIP's CMS_Sign) signed with the PAA key; a random valid passcode
//   and discriminator with salt and SPAKE2+ verifier (CHIP's Spake2pVerifier); a random serial number. Test vendor
//   0xFFF1 (this project has no vendor ID and is not certified: a controller says "uncertified"). The PAA and PAI keys
//   are used once and never stored: only the DAC key stays. Nothing here logs the passcode or a key.
#include "orcon_matter_identity.h"
#include "orcon_matter_out.h"
#include "orcon_matter_creds.h"   // a migration build's embedded identity (the placeholder holds zeros)

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <inttypes.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>

#include <credentials/CertificationDeclaration.h>
#include <crypto/CHIPCryptoPAL.h>
#include <platform/CHIPDeviceConfig.h>
#include <platform/ESP32/ESP32Config.h>
#include <setup_payload/ManualSetupPayloadGenerator.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>
#include <setup_payload/SetupPayload.h>

namespace orcon_matter {
namespace identity {
const char kDacKey[] = "dac-key", kDacPub[] = "dac-pub", kDacCert[] = "dac-cert", kPaiCert[] = "pai-cert",
           kPaaCert[] = "paa-cert", kCd[] = "cd", kSalt[] = "salt", kVerifier[] = "verifier";
namespace {
const char* TAG = "orcon_id";
constexpr char kPart[] = "chipkvs";   // Matter's own NVS (orcon_matter_nvs.cpp)
constexpr char kNs[] = "orcon-id";
constexpr uint8_t kFormat = 1;
constexpr uint32_t kIterations = 10000;   // PBKDF2 for the SPAKE2+ verifier (the specification allows 1000..100000)
constexpr size_t kCertMax = 600;
constexpr uint16_t kVid = CHIP_DEVICE_CONFIG_DEVICE_VENDOR_ID, kPid = CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_ID;
constexpr uint32_t kDeviceType = 0x002B;   // Fan
// The record covered by the digest: the format, the origin, these blobs (length + bytes, in this order) and the values.
// The PAA certificate is public and stored apart (generated identities only): what a verifier needs as trust anchor.
const char* const kBlobs[] = {kDacKey, kDacPub, kDacCert, kPaiCert, kCd, kSalt, kVerifier};
struct Values {
    uint8_t origin = 0;
    uint32_t pin = 0;
    uint16_t disc = 0;
    uint32_t iter = 0;
    char serial[24] = "";
};
State g_state = State::Unknown;
Values g_v;
char g_qr[32] = "", g_manual[16] = "";
GenStats g_stats{};

uint32_t freeInternal() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

bool plausible(const Values& v) {
    return (v.origin == uint8_t(Origin::Generated) || v.origin == uint8_t(Origin::Migrated)) &&
           chip::PayloadContents::IsValidSetupPIN(v.pin) && v.disc <= 0xFFF && v.iter >= 1000 && v.iter <= 100000 &&
           v.serial[0] != '\0';
}

// SHA-256 over the record as stored (so writing it also proves what NVS holds).
esp_err_t digestOf(nvs_handle_t h, const Values& v, uint8_t out[32]) {
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    const uint8_t head[2] = {kFormat, v.origin};
    mbedtls_sha256_update(&c, head, sizeof head);
    uint8_t buf[kCertMax];
    esp_err_t err = ESP_OK;
    for (const char* k : kBlobs) {
        size_t n = sizeof buf;
        if ((err = nvs_get_blob(h, k, buf, &n)) != ESP_OK) break;
        const uint8_t len[2] = {uint8_t(n), uint8_t(n >> 8)};
        mbedtls_sha256_update(&c, len, sizeof len);
        mbedtls_sha256_update(&c, buf, n);
    }
    mbedtls_platform_zeroize(buf, sizeof buf);   // the DAC key passed through it
    if (err == ESP_OK) {
        const uint8_t vals[10] = {uint8_t(v.pin), uint8_t(v.pin >> 8), uint8_t(v.pin >> 16), uint8_t(v.pin >> 24),
                                  uint8_t(v.disc), uint8_t(v.disc >> 8), uint8_t(v.iter), uint8_t(v.iter >> 8),
                                  uint8_t(v.iter >> 16), uint8_t(v.iter >> 24)};
        mbedtls_sha256_update(&c, vals, sizeof vals);
        mbedtls_sha256_update(&c, reinterpret_cast<const uint8_t*>(v.serial), strlen(v.serial));
        mbedtls_sha256_finish(&c, out);
    }
    mbedtls_sha256_free(&c);
    return err;
}

void computeCodes() {
    chip::PayloadContents p;
    p.version = 0;
    p.vendorID = kVid;
    p.productID = kPid;
    p.commissioningFlow = chip::CommissioningFlow::kStandard;
    p.rendezvousInformation.SetValue(chip::RendezvousInformationFlag::kOnNetwork);
    p.discriminator.SetLongValue(g_v.disc);
    p.setUpPINCode = g_v.pin;
    chip::MutableCharSpan qr(g_qr);
    if (chip::QRCodeBasicSetupPayloadGenerator(p).payloadBase38Representation(qr) != CHIP_NO_ERROR) g_qr[0] = '\0';
    chip::MutableCharSpan manual(g_manual);
    if (chip::ManualSetupPayloadGenerator(p).payloadDecimalStringRepresentation(manual) != CHIP_NO_ERROR) g_manual[0] = '\0';
}

// CHIP's Basic Information serial number (chip-factory "serial-num"; CHIP reads it before the build's own fallback).
esp_err_t setChipSerial(const char* s) {
    using chip::DeviceLayer::Internal::ESP32Config;
    char cur[24] = "";
    size_t n = 0;
    if (ESP32Config::ReadConfigValueStr(ESP32Config::kConfigKey_SerialNum, cur, sizeof cur, n) == CHIP_NO_ERROR && !strcmp(cur, s))
        return ESP_OK;
    return ESP32Config::WriteConfigValueStr(ESP32Config::kConfigKey_SerialNum, s) == CHIP_NO_ERROR ? ESP_OK : ESP_FAIL;
}

// Write a whole record: erase the namespace, every field, CHIP's serial, then the digest last (it makes the record valid).
esp_err_t store(const Values& v, const chip::ByteSpan blobs[], const chip::ByteSpan& paa) {
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(kPart, kNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    if (err == ESP_OK) err = nvs_set_u8(h, "v", kFormat);
    if (err == ESP_OK) err = nvs_set_u8(h, "origin", v.origin);
    for (size_t i = 0; err == ESP_OK && i < sizeof kBlobs / sizeof kBlobs[0]; ++i) err = nvs_set_blob(h, kBlobs[i], blobs[i].data(), blobs[i].size());
    if (err == ESP_OK && !paa.empty()) err = nvs_set_blob(h, kPaaCert, paa.data(), paa.size());
    if (err == ESP_OK) err = nvs_set_u32(h, "pin", v.pin);
    if (err == ESP_OK) err = nvs_set_u16(h, "disc", v.disc);
    if (err == ESP_OK) err = nvs_set_u32(h, "iter", v.iter);
    if (err == ESP_OK) err = nvs_set_str(h, "serial", v.serial);
    if (err == ESP_OK) err = nvs_commit(h);
    if (err == ESP_OK) err = setChipSerial(v.serial);
    uint8_t dg[32];
    if (err == ESP_OK) err = digestOf(h, v, dg);   // over what NVS now holds
    if (err == ESP_OK) err = nvs_set_blob(h, "digest", dg, sizeof dg);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---- generation -------------------------------------------------------------------------------------------------
struct Rng {
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    Rng() { mbedtls_entropy_init(&ent); mbedtls_ctr_drbg_init(&drbg); }
    ~Rng() { mbedtls_ctr_drbg_free(&drbg); mbedtls_entropy_free(&ent); }
    int seed() {
        static const char pers[] = "orcon-matter-identity";
        return mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, reinterpret_cast<const uint8_t*>(pers), sizeof pers - 1);
    }
    int bytes(void* out, size_t n) { return mbedtls_ctr_drbg_random(&drbg, static_cast<uint8_t*>(out), n); }
};
struct Key {
    mbedtls_pk_context pk;
    Key() { mbedtls_pk_init(&pk); }
    ~Key() { mbedtls_pk_free(&pk); }   // frees (and clears) the private value
    int make(Rng& r) {
        const int rc = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
        return rc ? rc : mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), mbedtls_ctr_drbg_random, &r.drbg);
    }
    int raw(uint8_t priv[32], uint8_t pub[65]) const {
        const mbedtls_ecp_keypair* e = mbedtls_pk_ec(pk);
        size_t n = 0;
        const int rc = mbedtls_mpi_write_binary(&e->MBEDTLS_PRIVATE(d), priv, 32);
        return rc ? rc : mbedtls_ecp_point_write_binary(&e->MBEDTLS_PRIVATE(grp), &e->MBEDTLS_PRIVATE(Q),
                                                        MBEDTLS_ECP_PF_UNCOMPRESSED, &n, pub, 65);
    }
};
enum class Kind { Paa, Pai, Dac };
// One attestation certificate in CHIP's profile: v3, ECDSA-SHA256, basicConstraints and keyUsage both critical
// (mbedTLS's own basicConstraints helper marks it critical only for a CA, so the extension is written here), SKID, AKID.
int makeCert(Key& subject, Key& issuer, const char* subjectName, const char* issuerName, Kind kind, Rng& r, uint8_t* out,
             size_t* len) {
    static const uint8_t kBcDac[] = {0x30, 0x00}, kBcPai[] = {0x30, 0x06, 0x01, 0x01, 0xFF, 0x02, 0x01, 0x00},
                         kBcPaa[] = {0x30, 0x03, 0x01, 0x01, 0xFF};
    const uint8_t* bc = kind == Kind::Dac ? kBcDac : kind == Kind::Pai ? kBcPai : kBcPaa;
    const size_t bcLen = kind == Kind::Dac ? sizeof kBcDac : kind == Kind::Pai ? sizeof kBcPai : sizeof kBcPaa;
    mbedtls_x509write_cert c;
    mbedtls_x509write_crt_init(&c);
    mbedtls_x509write_crt_set_version(&c, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&c, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&c, &subject.pk);
    mbedtls_x509write_crt_set_issuer_key(&c, &issuer.pk);
    uint8_t sn[8];
    int rc = r.bytes(sn, sizeof sn);
    sn[0] = uint8_t((sn[0] & 0x3F) | 0x40);   // positive, eight bytes
    if (!rc) rc = mbedtls_x509write_crt_set_serial_raw(&c, sn, sizeof sn);
    if (!rc) rc = mbedtls_x509write_crt_set_subject_name(&c, subjectName);
    if (!rc) rc = mbedtls_x509write_crt_set_issuer_name(&c, issuerName);
    if (!rc) rc = mbedtls_x509write_crt_set_validity(&c, "20260101000000", "99991231235959");   // no fixed end (RFC 5280)
    if (!rc) rc = mbedtls_x509write_crt_set_extension(&c, MBEDTLS_OID_BASIC_CONSTRAINTS, MBEDTLS_OID_SIZE(MBEDTLS_OID_BASIC_CONSTRAINTS),
                                                     1, bc, bcLen);
    if (!rc) rc = mbedtls_x509write_crt_set_key_usage(&c, kind == Kind::Dac ? MBEDTLS_X509_KU_DIGITAL_SIGNATURE
                                                                           : MBEDTLS_X509_KU_KEY_CERT_SIGN | MBEDTLS_X509_KU_CRL_SIGN);
    if (!rc) rc = mbedtls_x509write_crt_set_subject_key_identifier(&c);
    if (!rc) rc = mbedtls_x509write_crt_set_authority_key_identifier(&c);
    if (!rc) {
        const int n = mbedtls_x509write_crt_der(&c, out, kCertMax, mbedtls_ctr_drbg_random, &r.drbg);   // written at the end
        if (n > 0) { memmove(out, out + kCertMax - n, size_t(n)); *len = size_t(n); } else rc = n ? n : -1;
    }
    mbedtls_x509write_crt_free(&c);
    return rc;
}
// A distinguished name with Matter's vendor (and product) attribute: UTF8String of four upper-case hex digits.
// False when it does not fit: a cut name would certify another subject.
bool dn(char* out, size_t n, const char* cn, bool withPid) {
    char vid[5], pid[5];
    snprintf(vid, sizeof vid, "%04X", unsigned(kVid));
    snprintf(pid, sizeof pid, "%04X", unsigned(kPid));
    Out o{out, n};
    o.fmt("CN=%s,1.3.6.1.4.1.37244.2.1=#0C04%02X%02X%02X%02X", cn, vid[0], vid[1], vid[2], vid[3]);
    if (withPid) o.fmt(",1.3.6.1.4.1.37244.2.2=#0C04%02X%02X%02X%02X", pid[0], pid[1], pid[2], pid[3]);
    return !o.full;
}
}  // namespace

State state() { return g_state; }
Origin origin() { return g_state == State::Valid ? Origin(g_v.origin) : Origin::None; }
const char* stateName() {
    switch (g_state) {
        case State::Unknown: return "unknown";
        case State::Absent: return "absent";
        case State::Valid: return "valid";
        case State::Damaged: return "damaged";
    }
    return "?";
}
uint32_t passcode() { return g_state == State::Valid ? g_v.pin : 0; }
uint16_t discriminator() { return g_state == State::Valid ? g_v.disc : 0; }
uint32_t iterations() { return g_state == State::Valid ? g_v.iter : 0; }
const char* serial() { return g_state == State::Valid ? g_v.serial : ""; }
const char* qrCode() { return g_state == State::Valid ? g_qr : ""; }
const char* manualCode() { return g_state == State::Valid ? g_manual : ""; }
GenStats lastGenStats() { return g_stats; }

bool embeddedPresent() {
#ifdef ORCON_MATTER_PLACEHOLDER_IDENTITY
    return false;
#else
    return true;
#endif
}
#ifndef ORCON_MATTER_PLACEHOLDER_IDENTITY
// A migration build: this image itself holds a device's identity. Orcon's factory reset looks for this marker in the
// rollback slot and replaces such an image (otap::keepAfterReset): booted after the reset, it would move the previous
// owner's identity onto the device again. Kept by -u (CMakeLists.txt): nothing references it.
extern "C" __attribute__((used)) const char orcon_matter_identity_marker[] = "ORCON-MATTER-EMBEDDED-IDENTITY v1";
#endif
bool embeddedCodesMatch() {
    return embeddedPresent() && g_state == State::Valid && !strcmp(g_manual, orcon_matter_creds::kManualCode) &&
           !strcmp(g_qr, orcon_matter_creds::kQrCode);
}

State load() {
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(kPart, kNs, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return g_state = State::Absent;   // the namespace was never written
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "identity unreadable: %s", esp_err_to_name(err));
        return g_state = State::Unknown;
    }
    uint8_t stored[32];
    size_t n = sizeof stored;
    err = nvs_get_blob(h, "digest", stored, &n);
    if (err == ESP_ERR_NVS_NOT_FOUND) {   // a write that never completed: as if none (generate() starts it over)
        nvs_close(h);
        return g_state = State::Absent;
    }
    Values v;
    uint8_t fmt = 0;
    size_t sl = sizeof v.serial;
    bool ok = err == ESP_OK && n == sizeof stored && nvs_get_u8(h, "v", &fmt) == ESP_OK && fmt == kFormat &&
              nvs_get_u8(h, "origin", &v.origin) == ESP_OK && nvs_get_u32(h, "pin", &v.pin) == ESP_OK &&
              nvs_get_u16(h, "disc", &v.disc) == ESP_OK && nvs_get_u32(h, "iter", &v.iter) == ESP_OK &&
              nvs_get_str(h, "serial", v.serial, &sl) == ESP_OK;
    uint8_t now[32];
    ok = ok && digestOf(h, v, now) == ESP_OK && !memcmp(now, stored, sizeof now) && plausible(v);
    nvs_close(h);
    if (!ok) {
        ESP_LOGE(TAG, "identity damaged: Matter stays off (the owner decides about a new identity)");
        return g_state = State::Damaged;
    }
    g_v = v;
    g_state = State::Valid;
    computeCodes();
    if (setChipSerial(v.serial) != ESP_OK) ESP_LOGW(TAG, "serial number not stored for CHIP");
    return g_state;
}

esp_err_t erase() {
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(kPart, kNs, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_all(h);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    using chip::DeviceLayer::Internal::ESP32Config;   // the next identity stores its own serial over a stale one
    if (ESP32Config::ClearConfigValue(ESP32Config::kConfigKey_SerialNum) != CHIP_NO_ERROR) ESP_LOGW(TAG, "CHIP serial number not cleared");
    g_v = Values{};
    g_qr[0] = g_manual[0] = '\0';
    if (err == ESP_OK) g_state = State::Absent;
    return err;
}

esp_err_t migrateEmbedded() {
    if (!embeddedPresent() || g_state == State::Valid || g_state == State::Unknown) return ESP_ERR_INVALID_STATE;
    namespace c = orcon_matter_creds;
    Values v;
    v.origin = uint8_t(Origin::Migrated);
    v.pin = c::kPasscode;
    v.disc = c::kDiscriminator;
    v.iter = c::kIterations;
    strlcpy(v.serial, CONFIG_USE_TEST_SERIAL_NUMBER, sizeof v.serial);   // the serial the migration build reported
    if (!plausible(v)) return ESP_ERR_INVALID_ARG;
    const chip::ByteSpan blobs[] = {chip::ByteSpan(c::kDacPrivate), chip::ByteSpan(c::kDacPublic), chip::ByteSpan(c::kDacCert),
                                    chip::ByteSpan(c::kPaiCert), chip::ByteSpan(c::kCd), chip::ByteSpan(c::kSalt),
                                    chip::ByteSpan(c::kVerifier)};
    esp_err_t err = store(v, blobs, chip::ByteSpan());
    if (err == ESP_OK && load() != State::Valid) err = ESP_ERR_INVALID_CRC;
    if (err == ESP_OK && !embeddedCodesMatch()) ESP_LOGW(TAG, "the codes made from the stored identity differ from the embedded ones");
    ESP_LOGI(TAG, "embedded identity stored on the device: %s", esp_err_to_name(err));
    return err;
}

esp_err_t generate() {
    using namespace chip::Crypto;
    if (g_state == State::Valid || g_state == State::Unknown) return ESP_ERR_INVALID_STATE;   // never over one in use
    const int64_t t0 = esp_timer_get_time();
    g_stats = GenStats{0, freeInternal(), 0, 0};
    Rng r;
    Key paa, pai, dac;
    uint8_t* certs = static_cast<uint8_t*>(heap_caps_malloc(3 * kCertMax, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!certs) return ESP_ERR_NO_MEM;
    uint8_t *paaC = certs, *paiC = certs + kCertMax, *dacC = certs + 2 * kCertMax;
    size_t paaL = 0, paiL = 0, dacL = 0;
    char paaN[96], paiN[128], dacN[128];
    const bool named = dn(paaN, sizeof paaN, "Orcon 15RF zelfbouw PAA", false) &&
                       dn(paiN, sizeof paiN, "Orcon 15RF zelfbouw PAI", true) &&
                       dn(dacN, sizeof dacN, "Orcon 15RF controller", true);
    uint8_t paaPriv[32], paaPub[65], dacPriv[32], dacPub[65];
    int rc = named ? r.seed() : MBEDTLS_ERR_X509_BUFFER_TOO_SMALL;
    if (!rc) rc = paa.make(r);
    if (!rc) rc = pai.make(r);
    if (!rc) rc = dac.make(r);
    if (!rc) rc = makeCert(paa, paa, paaN, paaN, Kind::Paa, r, paaC, &paaL);
    if (!rc) rc = makeCert(pai, paa, paiN, paaN, Kind::Pai, r, paiC, &paiL);
    if (!rc) rc = makeCert(dac, pai, dacN, paiN, Kind::Dac, r, dacC, &dacL);
    if (!rc) rc = paa.raw(paaPriv, paaPub);
    if (!rc) rc = dac.raw(dacPriv, dacPub);
    esp_err_t err = rc ? ESP_FAIL : ESP_OK;
    // What a commissioner checks, by CHIP itself, before anything is stored: each certificate's attestation profile,
    // the chain, and that the DAC carries this key.
    if (err == ESP_OK) {
        CertificateChainValidationResult res;
        P256PublicKey pk;
        const bool good =
            VerifyAttestationCertificateFormat(chip::ByteSpan(paaC, paaL), AttestationCertType::kPAA) == CHIP_NO_ERROR &&
            VerifyAttestationCertificateFormat(chip::ByteSpan(paiC, paiL), AttestationCertType::kPAI) == CHIP_NO_ERROR &&
            VerifyAttestationCertificateFormat(chip::ByteSpan(dacC, dacL), AttestationCertType::kDAC) == CHIP_NO_ERROR &&
            ValidateCertificateChain(paaC, paaL, paiC, paiL, dacC, dacL, res) == CHIP_NO_ERROR &&
            ExtractPubkeyFromX509Cert(chip::ByteSpan(dacC, dacL), pk) == CHIP_NO_ERROR && !memcmp(pk.ConstBytes(), dacPub, sizeof dacPub);
        if (!good) { ESP_LOGE(TAG, "generated certificates failed CHIP's own checks"); err = ESP_ERR_INVALID_RESPONSE; }
    }
    // The certification declaration, signed with the PAA key (its SKID names the signer)
    uint8_t cd[400];
    chip::MutableByteSpan cdSpan(cd);
    if (err == ESP_OK) {
        chip::Credentials::CertificationElements e;
        e.FormatVersion = 1;
        e.VendorId = kVid;
        e.ProductIds[0] = kPid;
        e.ProductIdsCount = 1;
        e.DeviceTypeId = kDeviceType;
        uint8_t id[5];
        rc = r.bytes(id, sizeof id);
        if (!rc && snprintf(e.CertificateId, sizeof e.CertificateId, "ZELFBOUW-%02X%02X%02X%02X%02X", id[0], id[1], id[2],
                            id[3], id[4]) != int(chip::Credentials::kCertificateIdLength))
            rc = -1;   // a certification declaration's id is exactly 19 characters
        e.SecurityLevel = 0;
        e.SecurityInformation = 0;
        e.VersionNumber = 1;
        e.CertificationType = 0;   // development and test
        uint8_t enc[128], skid[20];
        chip::MutableByteSpan encSpan(enc), kid(skid);
        P256Keypair signer;
        const bool good = !rc && chip::Credentials::EncodeCertificationElements(e, encSpan) == CHIP_NO_ERROR &&
                          ExtractSKIDFromX509Cert(chip::ByteSpan(paaC, paaL), kid) == CHIP_NO_ERROR &&
                          signer.HazardousOperationLoadKeypairFromRaw(chip::ByteSpan(paaPriv), chip::ByteSpan(paaPub)) == CHIP_NO_ERROR &&
                          chip::Credentials::CMS_Sign(encSpan, kid, signer, cdSpan) == CHIP_NO_ERROR;
        chip::ByteSpan content;   // and as a verifier reads it: the PAA certificate checks its signature, the content is ours
        const bool readsBack = good &&
                               chip::Credentials::CMS_Verify(chip::ByteSpan(cdSpan.data(), cdSpan.size()), chip::ByteSpan(paaC, paaL), content) ==
                                   CHIP_NO_ERROR &&
                               content.data_equal(chip::ByteSpan(encSpan.data(), encSpan.size()));
        if (!readsBack) { ESP_LOGE(TAG, "certification declaration not made"); err = ESP_FAIL; }
    }
    mbedtls_platform_zeroize(paaPriv, sizeof paaPriv);   // the PAA key signed its last
    // Commissioning: a valid random passcode and discriminator (never the SDK's 20202021 / 3840), salt and verifier
    Values v;
    v.origin = uint8_t(Origin::Generated);
    v.iter = kIterations;
    uint8_t salt[32], ver[kSpake2p_VerifierSerialized_Length];
    chip::MutableByteSpan verSpan(ver);
    if (err == ESP_OK) {
        do { rc = r.bytes(&v.pin, sizeof v.pin); v.pin = v.pin % 99999998u + 1; } while (!rc && !chip::PayloadContents::IsValidSetupPIN(v.pin));
        do { if (!rc) rc = r.bytes(&v.disc, sizeof v.disc); v.disc &= 0xFFF; } while (!rc && v.disc == 0xF00);
        uint8_t sn[3];
        if (!rc) rc = r.bytes(salt, sizeof salt);
        if (!rc) rc = r.bytes(sn, sizeof sn);
        snprintf(v.serial, sizeof v.serial, "ORC-%02X%02X%02X", sn[0], sn[1], sn[2]);
        Spake2pVerifier verifier;
        if (rc || verifier.Generate(v.iter, chip::ByteSpan(salt), v.pin) != CHIP_NO_ERROR || verifier.Serialize(verSpan) != CHIP_NO_ERROR)
            err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        const chip::ByteSpan blobs[] = {chip::ByteSpan(dacPriv), chip::ByteSpan(dacPub), chip::ByteSpan(dacC, dacL),
                                        chip::ByteSpan(paiC, paiL), chip::ByteSpan(cdSpan.data(), cdSpan.size()),
                                        chip::ByteSpan(salt), chip::ByteSpan(verSpan.data(), verSpan.size())};
        err = store(v, blobs, chip::ByteSpan(paaC, paaL));
    }
    mbedtls_platform_zeroize(dacPriv, sizeof dacPriv);
    mbedtls_platform_zeroize(&v.pin, sizeof v.pin);
    heap_caps_free(certs);
    if (err == ESP_OK && load() != State::Valid) err = ESP_ERR_INVALID_CRC;
    g_stats.ms = uint32_t((esp_timer_get_time() - t0) / 1000);
    g_stats.heapAfter = freeInternal();
    g_stats.heapMinAfter = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "own identity %s: %s in %" PRIu32 " ms", err == ESP_OK ? "made on this device" : "not made",
             esp_err_to_name(err), g_stats.ms);
    return err;
}

CHIP_ERROR read(const char* key, chip::MutableByteSpan& out) {
    if (g_state != State::Valid) return CHIP_ERROR_INCORRECT_STATE;
    nvs_handle_t h;
    if (nvs_open_from_partition(kPart, kNs, NVS_READONLY, &h) != ESP_OK) return CHIP_ERROR_PERSISTED_STORAGE_FAILED;
    size_t n = out.size();
    const esp_err_t err = nvs_get_blob(h, key, out.data(), &n);
    nvs_close(h);
    if (err == ESP_ERR_NVS_INVALID_LENGTH) return CHIP_ERROR_BUFFER_TOO_SMALL;
    if (err != ESP_OK) return CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND;
    out.reduce_size(n);
    return CHIP_NO_ERROR;
}
}  // namespace identity
}  // namespace orcon_matter
