// Host test of the Stable/Beta channels: versions with "-beta.N", the release order,
// the beta manifest form, "beta-disabled", the channel index and bundle names, and judge(). The stable behaviour itself
// is covered by Orcon's own test/test_ota_policy.cpp and test/test_fw_update.cpp (run against the same sources).
//   test/run_host_tests.sh (matter_channels)
#include "fw_update.h"
#include "ota_policy.h"
#include <cstdio>
#include <cstring>
#include <string>

using namespace fwup;
static int fails = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
static bool is(const char* a, const char* b) { return a && b && !std::strcmp(a, b); }

static const char* kC = "0123456789abcdef0123456789abcdef01234567";
static const char* kS = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
static std::string manifest(const char* kind, uint32_t version, const char* pre /* raw JSON value or nullptr */) {
    std::string s = std::string("{\"v\":2,\"kind\":\"") + kind + "\",\"env\":\"heltec_v2_sx1276_control\",\"variant\":\"control\","
                    "\"allow_tx\":true,\"commit\":\"" + kC + "\",\"version\":" + std::to_string(version) +
                    ",\"size\":1000000,\"sha256\":\"" + kS + "\"";
    if (pre) s += std::string(",\"pre\":") + pre;
    return s + "}";
}
static const char* parse(const std::string& j, otap::Manifest& m) { return otap::parseManifest(j.c_str(), m); }

int main() {
    // ---- versions
    uint32_t v = 0; uint8_t p = 7;
    CHECK(parseSemverPre("1.1.0", v, p) && v == 1001000 && p == 0);
    CHECK(parseSemverPre("v1.1.0-beta.1", v, p) && v == 1001000 && p == 1);
    CHECK(parseSemverPre("1.1.0-beta.99", v, p) && p == 99);
    for (const char* bad : {"1.1.0-beta.0", "1.1.0-beta.01", "1.1.0-beta.100", "1.1.0-rc.1", "1.1.0-beta.", "1.1.0-beta.1a",
                            "1.1.0-beta", "1.1.0-", "1.1.0-beta.1+meta", "1.1-beta.1", "01.1.0-beta.1", "-beta.1", ""})
        CHECK(!parseSemverPre(bad, v, p));
    CHECK(!parseSemverPre(nullptr, v, p));
    CHECK(!parseSemver("1.1.0-beta.1", v));   // the stable parser keeps refusing every suffix
    char t[24];
    semverTextPre(1001000, 0, t); CHECK(is(t, "1.1.0"));
    semverTextPre(1001000, 4, t); CHECK(is(t, "1.1.0-beta.4"));
    semverTextPre(999999999, 99, t); CHECK(is(t, "999.999.999-beta.99"));
    // ---- the maxima: 999.999.999 and beta.99 are the largest valid values; one more is refused; no text is ever cut short
    CHECK(parseSemverPre("999.999.999-beta.99", v, p) && v == 999999999u && p == 99);
    CHECK(parseSemverPre("999.999.999", v, p) && v == 999999999u && p == 0);
    for (const char* bad : {"1000.0.0", "1.1000.0", "1.0.1000", "1000.0.0-beta.1", "999.999.999-beta.100", "999.999.999-beta.999"})
        CHECK(!parseSemverPre(bad, v, p));
    for (unsigned pre = 0; pre <= 99; ++pre) {   // every valid beta number of the largest version: to text and back
        semverTextPre(999999999, uint8_t(pre), t);
        CHECK(parseSemverPre(t, v, p) && v == 999999999u && p == pre);
    }
    semverTextPre(999999999, 255, t); CHECK(is(t, "999.999.999-beta.255"));   // the longest text of any input (20 chars), whole

    // ---- the order: 1.0.1 < 1.1.0-beta.1 < 1.1.0-beta.2 < 1.1.0 < 1.1.1-beta.1
    using otap::releaseRank;
    CHECK(releaseRank(1000001, 0) < releaseRank(1001000, 1));
    CHECK(releaseRank(1001000, 1) < releaseRank(1001000, 2));
    CHECK(releaseRank(1001000, 2) < releaseRank(1001000, 0));
    CHECK(releaseRank(1001000, 99) < releaseRank(1001000, 0));
    CHECK(releaseRank(1001000, 0) < releaseRank(1001001, 1));
    CHECK(releaseRank(999999999, 0) > releaseRank(999999999, 99));

    // ---- the manifest: stable unchanged, a beta is exactly one field more under its own kind
    otap::Manifest m;
    CHECK(parse(manifest("orcon-release", 1001000, nullptr), m) == nullptr && m.v == 2 && m.pre == 0 && m.version == 1001000);
    CHECK(parse(manifest("orcon-prerelease", 1001000, "1"), m) == nullptr && m.pre == 1);
    CHECK(parse(manifest("orcon-prerelease", 1001000, "99"), m) == nullptr && m.pre == 99);
    CHECK(is(parse(manifest("orcon-release", 1001000, "1"), m), "unknown-field"));        // a stable never carries pre
    CHECK(is(parse(manifest("orcon-prerelease", 1001000, nullptr), m), "missing-field"));
    for (const char* bad : {"0", "100", "255", "-1", "\"1\"", "1.5", "true", "null"})
        CHECK(is(parse(manifest("orcon-prerelease", 1001000, bad), m), "bad-manifest"));
    CHECK(is(parse(manifest("orcon-beta", 1001000, nullptr), m), "bad-manifest"));         // an unknown kind
    CHECK(is(parse(manifest("orcon-beta", 1001000, "1"), m), "unknown-field"));
    {   // v1 (board-bound) never has a kind, so a "prerelease" v1 is refused as before
        std::string j = manifest("orcon-prerelease", 1001000, "1"); j.replace(j.find("\"v\":2"), 5, "\"v\":1");
        CHECK(parse(j, m) != nullptr);
    }

    // ---- against this device (slot 3,342,336 B; own release and beta number; the owner's setting)
    const uint32_t slot = 3342336;
    otap::Manifest beta1, stable110, stable101;
    parse(manifest("orcon-prerelease", 1001000, "1"), beta1);
    parse(manifest("orcon-release", 1001000, nullptr), stable110);
    parse(manifest("orcon-release", 1000001, nullptr), stable101);
    CHECK(is(otap::checkManifest(beta1, "", "", slot, 1000001, 0, false), "beta-disabled"));
    CHECK(otap::checkManifest(beta1, "", "", slot, 1000001, 0, true) == nullptr);
    CHECK(is(otap::checkManifest(beta1, "", "", slot, 1001000, 0, true), "older-release"));    // never stable -> its beta
    CHECK(is(otap::checkManifest(beta1, "", "", slot, 1001000, 2, true), "older-release"));    // never beta.2 -> beta.1
    CHECK(otap::checkManifest(stable110, "", "", slot, 1001000, 2, false) == nullptr);          // beta.2 -> its stable
    CHECK(otap::checkManifest(stable110, "", "", slot, 1001000, 2, true) == nullptr);
    CHECK(is(otap::checkManifest(stable101, "", "", slot, 1001000, 2, false), "older-release"));  // channel off: no downgrade
    CHECK(otap::checkManifest(stable101, "", "", slot, 1000001) == nullptr);                    // the old call, unchanged

    // ---- URLs: stable "latest", beta the fixed channel-beta index; bundle names carry the suffix
    char u[200];
    CHECK(indexUrl(u, sizeof u) && std::strstr(u, "/releases/latest/download/orcon-update.json"));
    CHECK(indexUrl(u, sizeof u, true) && std::strstr(u, "/releases/download/channel-beta/orcon-update.json"));
    CHECK(indexUrl(u, sizeof u, false, otap::Variant::ControlMatter) && std::strstr(u, "/releases/latest/download/orcon-update-matter.json"));
    CHECK(indexUrl(u, sizeof u, true, otap::Variant::ControlMatter) && std::strstr(u, "/releases/download/channel-beta/orcon-update-matter.json"));
    CHECK(is(indexAsset(otap::Variant::Control), "orcon-update.json") && is(indexAsset(otap::Variant::RxOnly), "orcon-update.json"));
    Url pu;
    CHECK(parseUrl(u, pu) && allowedUrl(pu));
    char n[80];
    CHECK(bundleName(beta1, n, sizeof n) && is(n, "orcon-control-v1.1.0-beta.1-0123456.orconfw"));
    CHECK(bundleName(stable110, n, sizeof n) && is(n, "orcon-control-v1.1.0-0123456.orconfw"));
    CHECK(bundleUrl(beta1, u, sizeof u) && std::strstr(u, "/releases/download/v1.1.0-beta.1/orcon-control-v1.1.0-beta.1-0123456.orconfw"));
    CHECK(parseUrl(u, pu) && allowedUrl(pu));
    // the longest valid name and URL fit the firmware's own buffers (bundleUrl: name[80]; check(): url[200], as n and u here)
    otap::Manifest top = beta1; top.version = 999999999; top.pre = 99;
    CHECK(bundleName(top, n, sizeof n) && is(n, "orcon-control-v999.999.999-beta.99-0123456.orconfw"));
    CHECK(bundleUrl(top, u, sizeof u) && std::strstr(u, "/releases/download/v999.999.999-beta.99/orcon-control-v999.999.999-beta.99-0123456.orconfw"));
    CHECK(parseUrl(u, pu) && allowedUrl(pu));
    top.pre = 100; CHECK(!bundleName(top, n, sizeof n) && !bundleUrl(top, u, sizeof u));   // never a name for an invalid beta
    top.pre = 99; top.version = 1000000000u; CHECK(!bundleName(top, n, sizeof n) && !bundleUrl(top, u, sizeof u));

    // ---- judge(): what the check offers
    const Own stableOwn{1000001, otap::Variant::Control, "heltec_v2_sx1276_control", slot, 0, false};   // stable, beta off
    Own betaOwn = stableOwn; betaOwn.beta = true;
    Offer o; o.m = beta1;
    CHECK(is(judge(o, stableOwn), "beta-disabled"));
    CHECK(judge(o, betaOwn) == nullptr);                                  // newer: offered
    Own onBeta1 = betaOwn; onBeta1.version = 1001000; onBeta1.pre = 1;
    CHECK(is(judge(o, onBeta1), "up-to-date"));
    o.m = stable110;
    CHECK(judge(o, onBeta1) == nullptr);                                  // the stable after the beta: offered
    Own onStable110 = stableOwn; onStable110.version = 1001000;
    CHECK(is(judge(o, onStable110), "up-to-date"));
    o.m = beta1;
    CHECK(is(judge(o, Own{1001000, otap::Variant::Control, "heltec_v2_sx1276_control", slot, 0, true}), "older-release"));

    std::printf(fails ? "channels: %d of %d checks FAILED\n" : "channels: %d checks, 0 failures\n", fails ? fails : checks, checks);
    return fails ? 1 : 0;
}
