#include "config.h"
#if ORCON_OWNER_MODEL
#include "owner_store.h"
#include "auth_store.h"
#include "ota_policy.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_core_dump.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <mbedtls/aes.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>
#include <nvs.h>
#include <string.h>

namespace ownerstore {

static const char NS[] = "orconown";
static provision::State g_state = provision::State::Done;   // fail closed until begin()

// ---- primitives: mbedTLS 2.28 + the hardware RNG (true random while WiFi runs: always here) -----
static int rng(void*, unsigned char* out, size_t n) { esp_fill_random(out, n); return 0; }
static void rnd(uint8_t* out, size_t n) { esp_fill_random(out, n); }
static bool pbkdf2(const uint8_t* pw, size_t n, const uint8_t* salt, size_t sn, uint32_t it, uint8_t out[32]) {
    return authstore::kdf((const char*)pw, n, salt, sn, it, out);       // PBKDF2-HMAC-SHA256, its own KAT first
}
static bool aes(bool enc, const uint8_t key[32], const uint8_t iv[16], const uint8_t* in, size_t n, uint8_t* out) {
    if (n % 16) return false;
    mbedtls_aes_context c;
    mbedtls_aes_init(&c);
    uint8_t v[16];
    memcpy(v, iv, sizeof(v));                                            // crypt_cbc updates the IV in place
    const bool ok = (enc ? mbedtls_aes_setkey_enc(&c, key, 256) : mbedtls_aes_setkey_dec(&c, key, 256)) == 0 &&
                    mbedtls_aes_crypt_cbc(&c, enc ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT, n, v, in, out) == 0;
    mbedtls_aes_free(&c);
    ownerkey::wipe(v, sizeof(v));
    return ok;
}
static bool pub(const uint8_t d[32], uint8_t q[65]) {
    mbedtls_ecp_group g; mbedtls_mpi k; mbedtls_ecp_point p;
    mbedtls_ecp_group_init(&g); mbedtls_mpi_init(&k); mbedtls_ecp_point_init(&p);
    size_t n = 0;
    const bool ok = mbedtls_ecp_group_load(&g, MBEDTLS_ECP_DP_SECP256R1) == 0 && mbedtls_mpi_read_binary(&k, d, 32) == 0 &&
                    mbedtls_ecp_check_privkey(&g, &k) == 0 && mbedtls_ecp_mul(&g, &p, &k, &g.G, rng, nullptr) == 0 &&
                    mbedtls_ecp_point_write_binary(&g, &p, MBEDTLS_ECP_PF_UNCOMPRESSED, &n, q, 65) == 0 && n == 65;
    mbedtls_ecp_point_free(&p); mbedtls_mpi_free(&k); mbedtls_ecp_group_free(&g);   // mpi_free zeroises
    return ok;
}
static void sha(const uint8_t* in, size_t n, uint8_t out[32]) { mbedtls_sha256_ret(in, n, out, 0); }
static const ownerkey::Crypto CRYPTO{pbkdf2, aes, rnd, pub, sha};
const ownerkey::Crypto& crypto() { return CRYPTO; }

static void unhex(const char* h, uint8_t* out) {
    for (size_t i = 0; h[2 * i]; ++i) { const char t[3] = {h[2 * i], h[2 * i + 1], 0}; out[i] = (uint8_t)strtoul(t, nullptr, 16); }
}
bool cryptoReady() {
    // The vectors test/test_owner_key.cpp checks against OpenSSL: NIST SP 800-38A F.2.5 (CBC-AES256),
    // P-256 1*G = the generator, SHA-256("abc"). PBKDF2 runs its own KAT (authstore::kdfReady).
    static int state = 0;                                                // 0 unknown, 1 ok, -1 failed
    if (!state) {
        uint8_t k[32], iv[16], p[16], c[16], got[16], d[32] = {0}, g[65], q[65], h[32], want[32];
        unhex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4", k);
        unhex("000102030405060708090a0b0c0d0e0f", iv); unhex("6bc1bee22e409f96e93d7e117393172a", p);
        unhex("f58c4c04d6e5f1ba779eabfb5f7bfbd6", c);
        unhex("046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
              "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5", g);
        unhex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want);
        d[31] = 1;
        sha((const uint8_t*)"abc", 3, h);
        const bool ok = authstore::kdfReady() && aes(true, k, iv, p, 16, got) && !memcmp(got, c, 16) && aes(false, k, iv, c, 16, got) &&
                        !memcmp(got, p, 16) && pub(d, q) && !memcmp(q, g, 65) && !memcmp(h, want, 32);
        state = ok ? 1 : -1;
        Serial.printf("[owner] crypto self-test %s\n", ok ? "ok" : "FAILED (key routes stay closed)");
    }
    return state == 1;
}
bool generate(uint8_t d[32], uint8_t q[65]) {
    if (!cryptoReady()) return false;
    mbedtls_ecp_group g; mbedtls_mpi k; mbedtls_ecp_point p;
    mbedtls_ecp_group_init(&g); mbedtls_mpi_init(&k); mbedtls_ecp_point_init(&p);
    size_t n = 0;
    const bool ok = mbedtls_ecp_group_load(&g, MBEDTLS_ECP_DP_SECP256R1) == 0 && mbedtls_ecp_gen_keypair(&g, &k, &p, rng, nullptr) == 0 &&
                    mbedtls_mpi_write_binary(&k, d, 32) == 0 &&
                    mbedtls_ecp_point_write_binary(&g, &p, MBEDTLS_ECP_PF_UNCOMPRESSED, &n, q, 65) == 0 && n == 65;
    mbedtls_ecp_point_free(&p); mbedtls_mpi_free(&k); mbedtls_ecp_group_free(&g);
    if (!ok) ownerkey::wipe(d, 32);
    return ok;
}

// ---- state and key in NVS ---------------------------------------------------------------------
// The state is read with the raw NVS API: "namespace absent" (a new or reset device) must be told
// apart from "unreadable", which reads as installed (fail closed: no wizard, no vendor recovery).
static uint8_t readState() {
    nvs_handle_t h;
    const esp_err_t e = nvs_open(NS, NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return 0;
    if (e != ESP_OK) return 0xFF;
    uint8_t v = 0;
    const esp_err_t g = nvs_get_u8(h, "state", &v);
    nvs_close(h);
    return g == ESP_OK ? v : g == ESP_ERR_NVS_NOT_FOUND ? 0 : 0xFF;
}
static bool writeState(uint8_t v) {
    Preferences p;
    if (!p.begin(NS, false)) return false;
    const bool ok = p.putUChar("state", v) == 1;
    p.end();
    return ok && readState() == v;
}
provision::State state() { return g_state; }
// ---- the ownership sentinel (provision.h; Codex OWNER-11): the last two 4 KB sectors of the spiffs
// partition, which this firmware does not use and the Arduino NVS auto-format never touches.
struct SentinelSectors : provision::SentinelFlash {
    static const esp_partition_t* part() {
        const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
        return p && p->size >= 3 * 4096u ? p : nullptr;   // not the pinned layout: every access fails (fail closed)
    }
    static uint32_t off(const esp_partition_t* p, uint8_t i) { return p->size - (2u - i) * 4096u; }
    bool read(uint8_t i, uint8_t out[provision::SENTINEL_LEN]) override {
        const esp_partition_t* p = part();
        return p && esp_partition_read(p, off(p, i), out, provision::SENTINEL_LEN) == ESP_OK;
    }
    bool erase(uint8_t i) override { const esp_partition_t* p = part(); return p && esp_partition_erase_range(p, off(p, i), 4096) == ESP_OK; }
    bool write(uint8_t i, const uint8_t in[provision::SENTINEL_LEN]) override {
        const esp_partition_t* p = part();
        return p && esp_partition_write(p, off(p, i), in, provision::SENTINEL_LEN) == ESP_OK;
    }
};
static SentinelSectors g_sentinel;

// The PRG factory reset when the reset/ownership storage itself cannot be read: it is rewritten with a
// pending reset. Physical presence authorises a reset in every state, so this opens nothing new; the next
// boot then wipes everything (provision::resume) and the device is New.
bool forceResetMarker() { return provision::sentinelForce(g_sentinel, true, true); }

bool markSetup() {
    if (!provision::installing(g_state)) return false;
    if (!provision::sentinelOwn(g_sentinel)) return false;   // ownership outside NVS first (OWNER-11); never during a reset (OWNER-13)
    if (!writeState(provision::STORED_SETUP)) return false;
    g_state = provision::State::Setup;
    return true;
}
bool ownerKey(uint8_t q[65]) {
    Preferences p;
    if (!p.begin(NS, true)) return false;
    const bool ok = p.getBytesLength("pub") == ownerkey::PUB_LEN && p.getBytes("pub", q, ownerkey::PUB_LEN) == ownerkey::PUB_LEN && q[0] == 0x04;
    p.end();
    return ok;
}
// Tri-state like the state: a key that cannot be read still counts as present (fail closed).
static bool pubPresent() {
    nvs_handle_t h;
    const esp_err_t e = nvs_open(NS, NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return false;
    if (e != ESP_OK) return true;
    size_t n = 0;
    const esp_err_t g = nvs_get_blob(h, "pub", nullptr, &n);
    nvs_close(h);
    return g != ESP_ERR_NVS_NOT_FOUND;
}
bool registerKey(const uint8_t q[65]) {
    // From the first write on, the release-key path is closed in RAM as well: a write that persists
    // but fails its read-back must never leave a Legacy device recoverable with the release key
    // (Codex OWNER-07). If nothing persisted, the next boot reads Legacy again; the owner retries.
    if (g_state == provision::State::Legacy) g_state = provision::State::Done;
    Preferences p;
    if (!p.begin(NS, false)) return false;
    bool ok = p.putBytes("pub", q, ownerkey::PUB_LEN) == ownerkey::PUB_LEN;
    p.end();
    uint8_t back[65];
    ok = ok && ownerKey(back) && ownerkey::samePub(back, q) && writeState(provision::STORED_DONE);   // key first, then the state
    if (ok) g_state = provision::State::Done;
    return ok;
}

// ---- the button-started recovery mode: RTC memory survives esp_restart(), not a power cut -------
struct Arm { uint32_t magic; uint8_t mode, inv, pad[2]; };
static RTC_NOINIT_ATTR Arm g_arm;
static const uint32_t ARM_MAGIC = 0x4f524d31u;   // "ORM1"
static Mode g_boot = Mode::None;
static provision::PhysicalTimer g_physical;      // latched: a millis() wrap never revives it
void armMode(Mode m) { g_arm.magic = ARM_MAGIC; g_arm.mode = uint8_t(m); g_arm.inv = uint8_t(~uint8_t(m)); }
Mode bootMode() { return g_boot; }
uint32_t physicalLeftMs() { return g_physical.left(millis()); }
bool physicalActive() { return physicalLeftMs() > 0; }

void begin() {
    webauth::Cred c;
    const authstore::Status cs = authstore::status(c);
    uint8_t stored = readState();
    // A password that cannot be read is never "no password" (Codex OWNER-02): with no state it
    // would otherwise read as a new device (wizard open). Fail closed: installed, no wizard.
    if (cs == authstore::Status::Unreadable && stored == 0) stored = 0xFF;
    bool migrate = false;
    g_state = provision::fromStored(stored, cs == authstore::Status::Ok, pubPresent(), migrate);
    if (migrate && !writeState(provision::STORED_LEGACY))
        Serial.println("[owner] migration not stored; will repeat at the next boot");
    const provision::SentinelView sv = provision::sentinelLoad(g_sentinel);
    g_state = provision::withSentinel(g_state, sv.read, sv.cur);
    // Every owned device has its ownership in both sentinel sectors (a migrated board, a repair after damage).
    if (g_state != provision::State::New && !provision::sentinelOwn(g_sentinel))
        Serial.println("[owner] ownership sentinel not written; will repeat at the next boot");
    const bool armed = g_arm.magic == ARM_MAGIC && g_arm.inv == uint8_t(~g_arm.mode) && g_arm.mode <= uint8_t(Mode::WifiReset);
    g_boot = armed ? Mode(g_arm.mode) : Mode::None;
    g_arm.magic = 0;                                                     // one boot only
    g_physical.arm(g_boot != Mode::None);
    Serial.printf("[owner] state %s%s%s\n", provision::name(g_state), migrate ? " (migrated)" : g_state == provision::State::Locked ? " (NVS lost its owner: PRG factory reset required)" : "",
                  g_boot == Mode::Recovery ? ", recovery mode" : g_boot == Mode::WifiReset ? ", WiFi reset mode" : "");
}

// ---- factory reset --------------------------------------------------------------------------
// Fail closed (Codex OWNER-10): an allowlist. Only a running image whose state is known to be boot-stable
// (VALID, or UNDEFINED: a USB-flashed image selected by boot_app0) is settled. Everything else (unreadable,
// NEW, PENDING_VERIFY, INVALID, ABORTED, any unknown value) is unsettled: a restart could make the
// bootloader select the other slot first.
bool unsettledBoot() {
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) != ESP_OK) return true;
    return !(st == ESP_OTA_IMG_VALID || st == ESP_OTA_IMG_UNDEFINED);
}
struct Ops : provision::ResetOps {
    // The marker lives in the ownership sentinel, outside NVS: an NVS auto-format cannot swallow a pending
    // reset (Codex OWNER-11). Tri-state (Codex OWNER-01): only a confirmed absence means "no reset pending".
    provision::Marker marker() override {
        const provision::SentinelView v = provision::sentinelLoad(g_sentinel);
        if (v.read == provision::SentinelRead::Unreadable) return provision::Marker::Unreadable;
        return v.read == provision::SentinelRead::Ok && v.cur.resetPending ? provision::Marker::Set : provision::Marker::Absent;
    }
    bool mark() override { return provision::sentinelWrite(g_sentinel, true, true); }
    bool eraseNamespace(const char* ns) override {
        nvs_handle_t h;
        const esp_err_t e = nvs_open(ns, NVS_READWRITE, &h);
        if (e != ESP_OK) return false;
        const bool ok = nvs_erase_all(h) == ESP_OK && nvs_commit(h) == ESP_OK;
        nvs_close(h);
        Serial.printf("[reset] wiped %s: %s\n", ns, ok ? "ok" : "FAILED");
        return ok;
    }
    bool eraseCrashDump() override {
        const esp_err_t e = esp_core_dump_image_erase();
        return e == ESP_OK || e == ESP_ERR_NOT_FOUND || e == ESP_ERR_INVALID_SIZE;
    }
    // The other slot holds the rollback image. One from before the ownership model would trust the
    // release key for password recovery again. It is not erased but overwritten with a copy of the
    // running, confirmed image: no pre-owner firmware remains, and the device keeps two bootable
    // owner-model images (bootloader fallback, and a rollback target for the next OTA, both intact).
    bool retirePreOwnerSlot() override {
        const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
        const esp_partition_t* run = esp_ota_get_running_partition();
        if (!other || !run || other == run) return true;
        esp_image_metadata_t meta;
        const esp_partition_pos_t pos{other->address, other->size};
        const esp_err_t v = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta);
        if (v == ESP_ERR_IMAGE_INVALID) return true;    // nothing bootable there
        if (v != ESP_OK) return false;                  // a read error is not "absent": keep the marker, retry (Codex OWNER-09)
        otap::Scanner s("");
        static uint8_t buf[4096];
        for (uint32_t off = 0; off < meta.image_len; off += sizeof(buf)) {
            const uint32_t n = meta.image_len - off < sizeof(buf) ? meta.image_len - off : sizeof(buf);
            if (esp_partition_read(other, off, buf, n) != ESP_OK) return false;
            s.feed(buf, n);
        }
        if (s.ownerModel) return true;                  // a current image: it stays as the rollback target
        esp_image_metadata_t rm;
        const esp_partition_pos_t rpos{run->address, run->size};
        if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &rpos, &rm) != ESP_OK) return false;   // never copy a bad image
        Serial.printf("[reset] %s holds firmware from before the ownership model: replacing it with a copy of %s\n",
                      other->label, run->label);
        esp_ota_handle_t h;
        if (esp_ota_begin(other, rm.image_len, &h) != ESP_OK) return false;   // erases what the copy needs
        for (uint32_t off = 0; off < rm.image_len; off += sizeof(buf)) {
            const uint32_t n = rm.image_len - off < sizeof(buf) ? rm.image_len - off : sizeof(buf);
            if (esp_partition_read(run, off, buf, n) != ESP_OK || esp_ota_write(h, buf, n) != ESP_OK) { esp_ota_abort(h); return false; }
        }
        return esp_ota_end(h) == ESP_OK;                // ESP-IDF verifies the copy; the boot selection is unchanged
    }
    bool clear() override { return provision::sentinelWrite(g_sentinel, false, false); }   // last: the device is unowned again
};
bool requestFactoryReset() {
    if (unsettledBoot()) return false;   // a restart now could roll the update back first; the user retries later
    Ops o;
    return provision::request(o);
}
bool resetPending() { Ops o; return o.marker() == provision::Marker::Set; }
bool resumeFactoryReset(bool& ran) {
    Ops o;
    ran = o.marker() == provision::Marker::Set;
    return provision::resume(o);
}

}  // namespace ownerstore
#endif  // ORCON_OWNER_MODEL
