// Host tests for the signed-OTA rules (manifest, variants, streamed image checks). Synthetic data only.
#include "ota_policy.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace otap;
static int checks = 0, fails = 0;
#define CHECK(x, m) do { ++checks; if (!(x)) { ++fails; printf("FAIL %s (line %d)\n", m, __LINE__); } } while (0)
static const char* MAC = "02:00:00:00:00:08";
static const char* NONCE = "00112233445566778899aabbccddeeff";
static const char* COMMIT = "0123456789abcdef0123456789abcdef01234567";
static const char* SHA = "aa00000000000000000000000000000000000000000000000000000000000055";
static std::string manifest(const char* variant = "rx-only", const char* env = "heltec_v2_sx1276", bool tx = false,
                            unsigned size = 4096, const char* mac = MAC, const char* extra = "") {
    char b[600];
    snprintf(b, sizeof(b), "{\"v\":1,\"mac\":\"%s\",\"env\":\"%s\",\"variant\":\"%s\",\"allow_tx\":%s,\"commit\":\"%s\",\"size\":%u,"
             "\"sha256\":\"%s\",\"nonce\":\"%s\"%s}", mac, env, variant, tx ? "true" : "false", COMMIT, size, SHA, NONCE, extra);
    return b;
}
static std::vector<uint8_t> image(std::initializer_list<const char*> parts, size_t pad = 3000) {
    std::vector<uint8_t> v(pad, 0xA5);
    for (const char* p : parts) { v.insert(v.end(), p, p + strlen(p)); v.insert(v.end(), 97, 0x00); }
    return v;
}
static Scanner scan(const std::vector<uint8_t>& img, size_t chunk) {
    Scanner s(COMMIT);
    for (size_t i = 0; i < img.size(); i += chunk) s.feed(img.data() + i, std::min(chunk, img.size() - i));
    return s;
}
int main() {
    Manifest m;
    // ---- parsing ----
    CHECK(!parseManifest(manifest().c_str(), m) && m.variant == Variant::RxOnly && !m.allowTx && m.size == 4096 &&
          m.sha256[0] == 0xaa && m.sha256[31] == 0x55 && !strcmp(m.mac, MAC), "valid RX-only manifest parsed");
    CHECK(!strcmp(parseManifest(manifest("rx-only", "heltec_v2_sx1276", false, 4096, MAC, ",\"x\":1").c_str(), m), "unknown-field"), "unknown field refused");
    CHECK(!strcmp(parseManifest("{\"v\":1}", m), "missing-field"), "missing fields refused");
    CHECK(!strcmp(parseManifest("not json", m), "bad-manifest"), "garbage refused");
    CHECK(!strcmp(parseManifest(manifest("rf-unlimited").c_str(), m), "variant-not-approved"), "unapproved variant refused");
    { std::string s = manifest(); s.replace(s.find(COMMIT), 3, "xyz"); CHECK(!strcmp(parseManifest(s.c_str(), m), "bad-manifest"), "non-hex commit refused"); }
    // ---- manifest against the device ----
    parseManifest(manifest().c_str(), m);
    CHECK(!checkManifest(m, MAC, NONCE, 0x330000), "RX-only manifest for this board accepted");
    CHECK(!checkManifest(m, "02:00:00:00:00:08", NONCE, 0x330000), "MAC compare");
    { Manifest u; parseManifest(manifest("rx-only", "heltec_v2_sx1276", false, 4096, "02:00:00:00:00:0A").c_str(), u);
      CHECK(!checkManifest(u, "02:00:00:00:00:0a", NONCE, 0x330000), "MAC case-insensitive"); }
    CHECK(!strcmp(checkManifest(m, "02:00:00:00:00:09", NONCE, 0x330000), "wrong-board"), "other board refused");
    CHECK(!strcmp(checkManifest(m, MAC, "ffffffffffffffffffffffffffffffff", 0x330000), "nonce-mismatch"), "replayed/old nonce refused");
    CHECK(!strcmp(checkManifest(m, MAC, "", 0x330000), "nonce-required"), "expired nonce refused");
    CHECK(!strcmp(checkManifest(m, MAC, NONCE, 4095), "image-too-large"), "image larger than the slot refused");
    { Manifest z; parseManifest(manifest("rx-only", "heltec_v2_sx1276", false, 0).c_str(), z); CHECK(!strcmp(checkManifest(z, MAC, NONCE, 0x330000), "image-too-large"), "empty image refused"); }
    { Manifest x; parseManifest(manifest("rx-only", "heltec_v2_sx1276", true).c_str(), x); CHECK(!strcmp(checkManifest(x, MAC, NONCE, 0x330000), "variant-mismatch"), "RX-only claiming TX refused"); }
    { Manifest x; parseManifest(manifest("rf-test", "heltec_v2_sx1276_rftest", false).c_str(), x); CHECK(!strcmp(checkManifest(x, MAC, NONCE, 0x330000), "variant-mismatch"), "RF test without allow_tx refused"); }
    { Manifest x; parseManifest(manifest("rf-test", "heltec_v2_sx1276_webctl", true).c_str(), x); CHECK(!strcmp(checkManifest(x, MAC, NONCE, 0x330000), "variant-mismatch"), "uncapped RF env refused"); }
    { Manifest x; parseManifest(manifest("rx-only", "heltec_v2_sx1276_rftest", false).c_str(), x); CHECK(!strcmp(checkManifest(x, MAC, NONCE, 0x330000), "variant-mismatch"), "RX label on the RF env refused"); }
    // ---- streamed image ----
    const auto rx = image({COMMIT, "SX1276 direct RX (TX disabled)", "RX-ONLY TEST FIRMWARE", SIGNED_OTA_MARKER});
    Manifest mr; parseManifest(manifest("rx-only", "heltec_v2_sx1276", false, (unsigned)rx.size()).c_str(), mr);
    for (size_t chunk : {1u, 7u, 63u, 64u, 65u, 255u, 256u, 257u, 1436u, 100000u}) {
        const Scanner s = scan(rx, chunk);
        CHECK(s.rxRadio && s.rxBanner && s.signedOta && s.commitStamp && !s.txCode && !s.rfTest && !s.legacyOta && !s.dryRun,
              "markers found for every chunk size (patterns straddle chunk boundaries)");
        CHECK(!checkImage(mr, s, (uint32_t)rx.size(), mr.sha256), "RX-only image accepted");
    }
    { const Scanner s = scan(rx, 512);
      CHECK(!strcmp(checkImage(mr, s, (uint32_t)rx.size() - 1, mr.sha256), "size-mismatch"), "truncated upload refused");
      uint8_t bad[32]; memcpy(bad, mr.sha256, 32); bad[5] ^= 1;
      CHECK(!strcmp(checkImage(mr, s, (uint32_t)rx.size(), bad), "hash-mismatch"), "corrupted image refused"); }
    { const auto noOta = image({COMMIT, "SX1276 direct RX (TX disabled)", "RX-ONLY TEST FIRMWARE"});
      Manifest x; parseManifest(manifest("rx-only", "heltec_v2_sx1276", false, (unsigned)noOta.size()).c_str(), x);
      CHECK(!strcmp(checkImage(x, scan(noOta, 300), (uint32_t)noOta.size(), x.sha256), "image-without-signed-ota"),
            "image that would remove the WiFi update path refused"); }
    { const auto legacy = image({COMMIT, "SX1276 direct RX (TX disabled)", "RX-ONLY TEST FIRMWARE", SIGNED_OTA_MARKER, "ArduinoOTA"});
      Manifest x; parseManifest(manifest("rx-only", "heltec_v2_sx1276", false, (unsigned)legacy.size()).c_str(), x);
      CHECK(!strcmp(checkImage(x, scan(legacy, 300), (uint32_t)legacy.size(), x.sha256), "image-has-unauthenticated-ota"), "unauthenticated OTA refused"); }
    { const auto other = image({"fedcba9876543210fedcba9876543210fedcba98", "SX1276 direct RX (TX disabled)", "RX-ONLY TEST FIRMWARE", SIGNED_OTA_MARKER});
      Manifest x; parseManifest(manifest("rx-only", "heltec_v2_sx1276", false, (unsigned)other.size()).c_str(), x);
      CHECK(!strcmp(checkImage(x, scan(other, 300), (uint32_t)other.size(), x.sha256), "commit-not-in-image"), "image of another commit refused"); }
    const auto rf = image({COMMIT, "SX1276 direct RX + txpoc packet TX (RF TEST: max 50 packets per boot)", SIGNED_OTA_MARKER});
    { Manifest x; parseManifest(manifest("rx-only", "heltec_v2_sx1276", false, (unsigned)rf.size()).c_str(), x);
      CHECK(!strcmp(checkImage(x, scan(rf, 300), (uint32_t)rf.size(), x.sha256), "image-not-rx-only"), "RF image labelled RX-only refused (no silent TX)"); }
    { Manifest x; parseManifest(manifest("rf-test", "heltec_v2_sx1276_rftest", true, (unsigned)rf.size()).c_str(), x);
      CHECK(!checkManifest(x, MAC, NONCE, 0x330000) && !checkImage(x, scan(rf, 999), (uint32_t)rf.size(), x.sha256), "explicit RF test image accepted");
      Manifest y; parseManifest(manifest("rf-test", "heltec_v2_sx1276_rftest", true, (unsigned)rx.size()).c_str(), y);
      CHECK(!strcmp(checkImage(y, scan(rx, 300), (uint32_t)rx.size(), y.sha256), "image-not-rf-test"), "RX image labelled RF refused"); }
    { const auto uncapped = image({COMMIT, "SX1276 direct RX + txpoc packet TX", SIGNED_OTA_MARKER});
      Manifest x; parseManifest(manifest("rf-test", "heltec_v2_sx1276_rftest", true, (unsigned)uncapped.size()).c_str(), x);
      CHECK(!strcmp(checkImage(x, scan(uncapped, 300), (uint32_t)uncapped.size(), x.sha256), "image-not-rf-test"), "uncapped RF image refused"); }
    { const auto dry = image({COMMIT, "SX1276 direct RX (TX disabled)", "RX-ONLY TEST FIRMWARE", "tx-poc-dryrun", SIGNED_OTA_MARKER});
      Manifest x; parseManifest(manifest("rx-only", "heltec_v2_sx1276", false, (unsigned)dry.size()).c_str(), x);
      CHECK(!strcmp(checkImage(x, scan(dry, 300), (uint32_t)dry.size(), x.sha256), "image-not-rx-only"), "dry-run image not accepted as RX-only"); }
    printf("ota policy: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
