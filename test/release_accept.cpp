// release_accept — the device's own rules on a release package, before it is published (tools/release_ci.py).
//
//   release_accept semver <text>                         print the release number (fwup::parseSemver) or "invalid"
//   release_accept bundle <file.orconfw> <image-sha256> <own-version> [<own-pre> <beta-channel 0|1>]
//       the bundle head as fw_update.cpp's install() reads it (one line, exactly {"manifest","sig"}), the manifest as
//       ota_signed.cpp's acceptManifest(release only) judges it (parseManifest, v2, checkManifest with the slot size and
//       own version, beta and channel), the image streamed through the Scanner in uneven chunks and checkImage with the
//       image's real SHA-256 (computed by the caller), and the file name as fwup::bundleName() builds it.
//   release_accept index <update index> <file.orconfw> <own-version> [<own-variant> [<own-pre> <beta-channel 0|1>]]
//       the index as fw_update.cpp's check() reads it (parseIndex, parseManifest, judge for a board of <own-variant>,
//       control by default, running <own-version>), and the very same manifest and signature as the bundle head.
// Prints ACCEPTED (index: NEWER or UP-TO-DATE) or the device's error code; exit 0 only when accepted.
// The ECDSA signature itself is checked by release_ci.py with openssl against src/ota_pubkey.h.
#include "fw_update.h"
#include "ota_policy.h"
#include <ArduinoJson.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const uint32_t SLOT = 0x330000;   // app0/app1 (tools/check_parts.py)
static bool readFile(const char* p, std::vector<uint8_t>& out) {
    FILE* f = fopen(p, "rb"); if (!f) return false;
    uint8_t b[65536]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) out.insert(out.end(), b, b + n);
    fclose(f); return true;
}
static int done(const char* e, const char* ok = "ACCEPTED") { printf("%s\n", e ? e : ok); return e ? 1 : 0; }
static bool head(const std::vector<uint8_t>& b, std::string& manifest, std::string& sig, size_t& bodyAt) {
    size_t nl = 0;
    while (nl < b.size() && nl < 1024 && b[nl] != '\n') ++nl;
    if (nl >= b.size() || b[nl] != '\n') return false;
    JsonDocument d;   // the device's own strict grammar first (fw_update.cpp install(); Codex r4)
    if (!fwup::flatObject((const char*)b.data(), nl) || deserializeJson(d, (const char*)b.data(), nl) || !d.is<JsonObject>() || d.as<JsonObjectConst>().size() != 2 ||
        !d["manifest"].is<const char*>() || !d["sig"].is<const char*>()) return false;
    manifest = d["manifest"].as<const char*>(); sig = d["sig"].as<const char*>(); bodyAt = nl + 1;
    return true;
}
static const char* base(const char* p) { const char* s = strrchr(p, '/'); return s ? s + 1 : p; }
static bool ownVariant(const char* name, otap::Variant& v, const char*& env) {   // the boards a release is for
    if (!strcmp(name, "control")) { v = otap::Variant::Control; env = "heltec_v2_sx1276_control"; return true; }
    if (!strcmp(name, "control-matter")) { v = otap::Variant::ControlMatter; env = "heltec_v2_matter"; return true; }
    return false;
}

int main(int argc, char** argv) {
    if (argc == 3 && !strcmp(argv[1], "semver")) {
        uint32_t v = 0;
        if (!fwup::parseSemver(argv[2], v)) return done("invalid");
        printf("%lu\n", (unsigned long)v); return 0;
    }
    if ((argc == 5 || argc == 7) && !strcmp(argv[1], "bundle")) {
        const uint8_t ownPre = argc == 7 ? (uint8_t)strtoul(argv[5], nullptr, 10) : 0;
        const bool betaOn = argc == 7 && !strcmp(argv[6], "1");
        std::vector<uint8_t> b; if (!readFile(argv[2], b)) return done("unreadable");
        std::string man, sig; size_t at = 0;
        if (!head(b, man, sig, at)) return done("bad-bundle");
        otap::Manifest m;
        if (const char* e = otap::parseManifest(man.c_str(), m)) return done(e);
        if (m.v != 2) return done("manifest-version");
        if (const char* e = otap::checkManifest(m, "", "", SLOT, (uint32_t)strtoul(argv[4], nullptr, 10), ownPre, betaOn)) return done(e);
        char name[96];
        if (!fwup::bundleName(m, name, sizeof name) || strcmp(name, base(argv[2]))) return done("bundle-name");
        const char* hx = argv[3]; uint8_t sha[32];
        if (strlen(hx) != 64) return done("bad-hash");
        for (int i = 0; i < 32; ++i) { char t[3] = {hx[2 * i], hx[2 * i + 1], 0}; sha[i] = (uint8_t)strtoul(t, nullptr, 16); }
        otap::Scanner s(m.commit);
        const size_t n = b.size() - at;
        for (size_t i = 0, k = 0; i < n; i += k) { k = 1 + (i * 7919) % 1460; if (k > n - i) k = n - i; s.feed(b.data() + at + i, k); }
        return done(otap::checkImage(m, s, (uint32_t)n, sha));
    }
    if ((argc == 5 || argc == 6 || argc == 8) && !strcmp(argv[1], "index")) {
        otap::Variant variant = otap::Variant::Control;
        const char* env = "heltec_v2_sx1276_control";
        if (argc >= 6 && !ownVariant(argv[5], variant, env)) return done("bad-arguments");
        const uint8_t ownPre = argc == 8 ? (uint8_t)strtoul(argv[6], nullptr, 10) : 0;
        const bool betaOn = argc == 8 && !strcmp(argv[7], "1");
        std::vector<uint8_t> j, b; if (!readFile(argv[2], j) || !readFile(argv[3], b)) return done("unreadable");
        fwup::Offer o;
        if (const char* e = fwup::parseIndex((const char*)j.data(), j.size(), o)) return done(e);
        if (const char* e = otap::parseManifest(o.manifest, o.m)) return done(e);
        std::string man, sig; size_t at = 0;
        if (!head(b, man, sig, at)) return done("bad-bundle");
        if (man != o.manifest || sig != o.sig) return done("bundle-mismatch");
        const fwup::Own own = {(uint32_t)strtoul(argv[4], nullptr, 10), variant, env, SLOT, ownPre, betaOn};
        const char* e = fwup::judge(o, own);
        if (e && !strcmp(e, "up-to-date")) return done(nullptr, "UP-TO-DATE");
        return done(e, "NEWER");
    }
    fprintf(stderr, "usage: release_accept semver <text> | bundle <file> <sha256> <own> | index <json> <bundle> <own>\n");
    return 2;
}
