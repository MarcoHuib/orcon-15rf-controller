// Host tests for the official-release update check and the one-click download (src/fw_update.cpp), against a
// scripted mock of GitHub (redirects, TLS/DNS failures, stalls, truncation) and a mock OTA writer. Synthetic data only.
#include "fw_update.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
using namespace fwup;
static int checks = 0, fails = 0;
#define CHECK(x, m) do { ++checks; if (!(x)) { ++fails; printf("FAIL %s (line %d)\n", m, __LINE__); } } while (0)
static bool eq(const char* a, const char* b) { return a && b && !strcmp(a, b); }

// ---- a fake clock and a fake signature (the real one is ECDSA in ota_signed.cpp; here: a keyed checksum) -------------
static uint32_t g_now = 1000;
static uint32_t clockNow() { return g_now; }
static std::string fakeSig(const std::string& text) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : text) { h ^= c; h *= 1099511628211ull; }
    char b[40]; snprintf(b, sizeof(b), "3045%016llx%016llx", (unsigned long long)h, (unsigned long long)~h);
    return b;
}
static bool fakeVerify(const char* text, size_t n, const char* sig) { return fakeSig(std::string(text, n)) == sig; }

// ---- mock GitHub -------------------------------------------------------------------------------------------------
struct Reply { std::string bytes; int stallAt = -1; int closeAt = -1; uint32_t slowMs = 0; };
struct Mock : Transport {
    std::map<std::string, Reply> routes;           // "host path" -> reply
    std::map<std::string, const char*> refuse;     // host -> connect error
    std::vector<std::string> log;                  // every request "host path"
    const Reply* cur = nullptr; size_t pos = 0; std::string req, host; bool open = false; const char* cerr = nullptr;
    int connects = 0, stops = 0;
    bool connect(const char* h, uint32_t) override {
        ++connects; host = h; req.clear(); cur = nullptr; pos = 0;
        auto r = refuse.find(h);
        if (r != refuse.end()) { cerr = r->second; return false; }
        open = true; return true;
    }
    bool write(const char* d, size_t n) override { if (!open) return false; req.append(d, n); return true; }
    int read(uint8_t* b, size_t n, uint32_t timeoutMs) override {
        if (!open) return -2;
        if (!cur) {   // the request is complete: route it
            const size_t a = req.find("GET "), z = req.find(" HTTP/1.1\r\n");
            if (a != 0 || z == std::string::npos || req.find("Host: " + host + "\r\n") == std::string::npos ||
                req.find("Connection: close\r\n") == std::string::npos || req.substr(req.size() - 4) != "\r\n\r\n") return -2;
            const std::string key = host + " " + req.substr(4, z - 4);
            log.push_back(key);
            auto it = routes.find(key);
            static Reply notFound; notFound.bytes = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nNot Found";
            cur = it == routes.end() ? &notFound : &it->second;
        }
        if (cur->stallAt >= 0 && pos >= size_t(cur->stallAt)) { g_now += timeoutMs; return -1; }
        if (cur->closeAt >= 0 && pos >= size_t(cur->closeAt)) return 0;
        if (pos >= cur->bytes.size()) return 0;
        size_t k = std::min(n, cur->bytes.size() - pos);
        if (cur->stallAt >= 0) k = std::min(k, size_t(cur->stallAt) - pos);
        if (cur->closeAt >= 0) k = std::min(k, size_t(cur->closeAt) - pos);
        k = std::min<size_t>(k, 1 + (pos * 7919) % 1460);   // uneven segments
        memcpy(b, cur->bytes.data() + pos, k); pos += k; g_now += cur->slowMs;
        return int(k);
    }
    void stop() override { open = false; ++stops; }
    const char* connectError() override { return cerr; }
};
// GitHub's response fields around a release download (2026-11-22): its Content-Security-Policy alone is 4,203 bytes.
static std::string csp() {
    std::string v = "default-src 'none'; base-uri 'self'; child-src github.com/assets-cdn/worker/ gist.github.com/assets-cdn/worker/; ";
    while (v.size() < 4203) v += "connect-src 'self' uploads.github.com www.githubstatus.com collector.github.com raw.githubusercontent.com api.github.com; ";
    v.resize(4203);
    return "Content-Security-Policy: " + v + "\r\n";
}
static std::string gh() {
    return "Server: github.com\r\nDate: Sun, 22 Nov 2026 12:00:00 GMT\r\nContent-Type: text/html; charset=utf-8\r\nVary: X-PJAX, X-PJAX-Container, Turbo-Visit, Turbo-Frame, Accept-Encoding, Accept, X-Requested-With\r\n"
           "Cache-Control: no-cache\r\nStrict-Transport-Security: max-age=31536000; includeSubdomains; preload\r\nX-Frame-Options: deny\r\nX-Content-Type-Options: nosniff\r\n"
           "X-XSS-Protection: 0\r\nReferrer-Policy: no-referrer-when-downgrade\r\nX-GitHub-Request-Id: C0DE:1234:56789A:BCDEF0:6740A2B0\r\n";
}
static std::string ghRedirect(const std::string& to) { return "HTTP/1.1 302 Found\r\n" + gh() + csp() + "Location: " + to + "\r\nContent-Length: 0\r\n\r\n"; }
static std::string ok(const std::string& body, const std::string& extra = "") {
    return "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n" + extra + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}
static std::string redirect(const std::string& to, int code = 302) {
    return "HTTP/1.1 " + std::to_string(code) + " Found\r\nLocation: " + to + "\r\nContent-Length: 0\r\n\r\n";
}

// ---- a release as the pipeline publishes it --------------------------------------------------------------------------
static const char* COMMIT = "0123456789abcdef0123456789abcdef01234567";
static const char* REPO = "/" ORCON_RELEASE_REPO "/releases/";
static std::string manifestText(uint32_t version, const char* variant = "control", const char* env = "heltec_v2_sx1276_control",
                                bool tx = true, size_t size = 5000) {
    char b[600];
    snprintf(b, sizeof(b), "{\"v\":2,\"kind\":\"orcon-release\",\"env\":\"%s\",\"variant\":\"%s\",\"allow_tx\":%s,\"commit\":\"%s\","
             "\"version\":%u,\"size\":%zu,\"sha256\":\"%s\"}", env, variant, tx ? "true" : "false", COMMIT, version, size,
             "aa00000000000000000000000000000000000000000000000000000000000055");
    return b;
}
static std::string jsonStr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o + "\"";
}
static std::string indexJson(const std::string& man, const std::string& sig, const std::string& notes = "Kleine verbeteringen.") {
    return "{\"schema\":1,\"manifest\":" + jsonStr(man) + ",\"sig\":\"" + sig + "\",\"notes\":" + jsonStr(notes) + "}";
}
static std::vector<uint8_t> image(size_t n) { std::vector<uint8_t> v(n); for (size_t i = 0; i < n; ++i) v[i] = uint8_t(i * 31 + 7); return v; }
static std::string bundle(const std::string& man, const std::string& sig, const std::vector<uint8_t>& img) {
    return "{\"manifest\":" + jsonStr(man) + ",\"sig\":\"" + sig + "\"}\n" + std::string(img.begin(), img.end());
}
static const Own OWN = {1002003, otap::Variant::Control, "heltec_v2_sx1276_control", 0x330000};
// GitHub as it answers today: latest/download -> releases/download/<tag>/<asset> -> the signed asset-host URL.
static void publish(Mock& m, const std::string& man, const std::string& sig, const std::vector<uint8_t>& img, const char* tag,
                    const std::string& indexBody = "") {
    char name[96]; otap::Manifest mm; otap::parseManifest(man.c_str(), mm);
    bundleName(mm, name, sizeof(name));
    const std::string idx = indexBody.empty() ? indexJson(man, sig) : indexBody;
    const std::string signedQ = "?sp=r&sv=2018-11-09&sr=b&spr=https&se=2026-11-19T21%3A39%3A28Z&sig=AbC%2Bd%3D&jwt=x.y.z";
    m.routes["github.com " + std::string(REPO) + "latest/download/orcon-update.json"].bytes =
        redirect(std::string("https://github.com") + REPO + "download/" + tag + "/orcon-update.json");
    m.routes["github.com " + std::string(REPO) + "download/" + tag + "/orcon-update.json"].bytes =
        redirect("https://release-assets.githubusercontent.com/github-production-release-asset/1/idx" + signedQ);
    m.routes["release-assets.githubusercontent.com /github-production-release-asset/1/idx" + signedQ].bytes = ok(idx);
    m.routes["github.com " + std::string(REPO) + "download/" + tag + "/" + name].bytes =
        redirect("https://release-assets.githubusercontent.com/github-production-release-asset/1/fw" + signedQ);
    m.routes["release-assets.githubusercontent.com /github-production-release-asset/1/fw" + signedQ].bytes = ok(bundle(man, sig, img));
}
// The mock OTA writer: the device's checks in short (signature, manifest, version), bytes compared with the expected image.
struct MockSink : Sink {
    std::vector<uint8_t> want, got; bool begun = false, aborted = false, activated = false; const char* failWriteAt = nullptr; size_t failAfter = 0;
    const char* begin(const char* man, const char* sig) override {
        if (!fakeVerify(man, strlen(man), sig)) return "bad-signature";
        otap::Manifest m; if (const char* e = otap::parseManifest(man, m)) return e;
        if (const char* e = otap::checkManifest(m, "", "", 0x330000, OWN.version)) return e;
        begun = true; got.clear(); return nullptr;
    }
    const char* write(const uint8_t* d, size_t n) override {
        if (!begun) return "not-ready";
        if (failWriteAt && got.size() + n > failAfter) return failWriteAt;
        got.insert(got.end(), d, d + n); return nullptr;
    }
    const char* finish() override { if (got != want) return "hash-mismatch"; activated = true; return nullptr; }
    void abort(const char*) override { aborted = true; begun = false; }
};

static Url U; static Head H; static char IO[4096];
static const char* runCheck(Mock& m, Offer& o, Found& f, const Own& own = OWN) {
    Limits lim; return check(m, fakeVerify, clockNow, lim, own, o, f, IO, sizeof(IO), U, H);
}
static const char* runInstall(Mock& m, const Offer& o, MockSink& s) {
    Limits lim; return install(m, s, clockNow, lim, o, nullptr, (uint8_t*)IO, sizeof(IO), U, H);
}

int main() {
    // ---- SemVer <-> release number ----
    uint32_t v = 0;
    CHECK(parseSemver("1.0.0", v) && v == 1000000, "1.0.0");
    CHECK(parseSemver("v1.2.3", v) && v == 1002003, "v1.2.3");
    CHECK(parseSemver("0.0.1", v) && v == 1, "0.0.1");
    CHECK(parseSemver("999.999.999", v) && v == 999999999u, "largest");
    for (const char* bad : {"", "v", "1", "1.2", "192.0.2.4", "01.2.3", "1.02.3", "1.2.03", "1000.0.0", "1.1000.0", "1.0.1000",
                            "0.0.0", "1.2.3-rc1", "1.2.3+b", " 1.2.3", "1.2.3 ", "V1.2.3", "vv1.2.3", "-1.2.3", "1..3", "1.2.x"}) {
        CHECK(!parseSemver(bad, v), bad);
    }
    char t[16];
    semverText(1002003, t); CHECK(eq(t, "1.2.3"), "text 1.2.3");
    semverText(0, t); CHECK(eq(t, "dev"), "text dev");
    semverText(1000000000u, t); CHECK(eq(t, "?"), "text overflow");
    CHECK(parseSemver("2.0.0", v) && v > 1999999 && parseSemver("1.999.999", v) && v < 2000000, "numeric order = SemVer order");

    // ---- memory gate ----
    CHECK(!memoryOk(MIN_FREE_HEAP, MIN_FREE_BLOCK), "enough memory");
    CHECK(eq(memoryOk(MIN_FREE_HEAP - 1, 100000), "low-memory"), "too little free heap");
    CHECK(eq(memoryOk(200000, MIN_FREE_BLOCK - 1), "low-memory"), "fragmented heap (no 16 KB TLS buffer)");

    // ---- URLs ----
    CHECK(parseUrl("https://github.com/a/b", U) && eq(U.host, "github.com") && eq(U.path, "/a/b"), "plain URL");
    CHECK(parseUrl("https://GitHub.com:443/x", U) && eq(U.host, "github.com") && eq(U.path, "/x"), "case and :443");
    CHECK(parseUrl("https://github.com", U) && eq(U.path, "/"), "no path");
    for (const char* bad : {"http://github.com/x", "https://user@github.com/x", "https://github.com:8443/x", "https://github.com/a b",
                            "https://github.com/a\\b", "https://github.com/a#b", "https:///x", "https://.github.com/x", "ftp://x/y",
                            "https://git_hub.com/x", "https://github..com/x", "HTTPS://github.com/x"}) {
        CHECK(!parseUrl(bad, U), bad);
    }
    { std::string longPath = "https://release-assets.githubusercontent.com/" + std::string(PATH_MAX_LEN, 'a'); CHECK(!parseUrl(longPath.c_str(), U), "path too long"); }
    char url[200];
    CHECK(indexUrl(url, sizeof(url)) && parseUrl(url, U) && allowedUrl(U), "own index URL allowed");
    CHECK(parseUrl("https://github.com/other/repo/releases/download/v1/x", U) && !allowedUrl(U), "another repository refused");
    { std::string p = std::string("https://github.com") + REPO + "../../../other/repo/releases/x";
      CHECK(parseUrl(p.c_str(), U) && !allowedUrl(U), "dot-dot out of the repository refused"); }
    { std::string p = std::string("https://github.com") + REPO + "download/%2e%2e/x"; CHECK(parseUrl(p.c_str(), U) && !allowedUrl(U), "encoded path on github.com refused"); }
    CHECK(parseUrl("https://release-assets.githubusercontent.com/a?b=c%2B", U) && allowedUrl(U), "asset host allowed");
    CHECK(parseUrl("https://objects.githubusercontent.com/a", U) && allowedUrl(U), "legacy asset host allowed");
    for (const char* bad : {"https://evil.example/x", "https://githubusercontent.com/x", "https://raw.githubusercontent.com/x",
                            "https://api.github.com/x", "https://github.com.evil.example/x", "https://release-assets.githubusercontent.com.evil.example/x"}) {
        CHECK(parseUrl(bad, U) && !allowedUrl(U), bad);
    }
    { Url base; parseUrl("https://github.com/x", base);
      CHECK(resolveLocation("/y/z", base, U) && eq(U.host, "github.com") && eq(U.path, "/y/z"), "relative redirect: same host");
      CHECK(!resolveLocation("//evil.example/x", base, U), "scheme-relative redirect refused");
      CHECK(!resolveLocation("http://github.com/x", base, U), "plain-HTTP redirect refused");
      CHECK(!resolveLocation("y/z", base, U), "path-relative redirect refused");
      CHECK(resolveLocation("/q", base, base) && eq(base.path, "/q") && eq(base.host, "github.com"), "in-place resolve"); }

    // ---- response head limits ----
    { Head h; const char* s = "HTTP/1.1 200 OK\r\nContent-Length: 12\r\nX: y\r\n\r\n"; int r = 0;
      for (const char* p = s; *p && r == 0; ++p) r = h.push(*p);
      CHECK(r == 1 && h.status == 200 && h.length == 12 && !h.chunked, "head parsed"); }
    auto headOf = [](const std::string& s) { static Head h; h.reset(); int r = 0; for (char c : s) { r = h.push(c); if (r) break; } return r < 0 ? h.error() : r == 1 ? "done" : "more"; };
    CHECK(eq(headOf("HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n"), "bad-response"), "conflicting lengths refused");
    CHECK(eq(headOf("HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n"), "done"), "repeated equal length ok");
    CHECK(eq(headOf("HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n"), "bad-response"), "negative length refused");
    CHECK(eq(headOf("HTTP/1.1 200 OK\r\nContent-Length: 99999999999\r\n\r\n"), "bad-response"), "huge length refused");
    CHECK(eq(headOf("HTTP/1.1 200 OK\r\n folded\r\n\r\n"), "bad-response"), "line folding refused");
    CHECK(eq(headOf("HTTP/1.1 200 OK\r\nnocolon\r\n\r\n"), "bad-response"), "header without colon refused");
    CHECK(eq(headOf("HTTP/2 200\r\n\r\n"), "bad-response"), "unknown protocol refused");
    CHECK(eq(headOf("HTTP/1.1 2000 OK\r\n\r\n"), "bad-response"), "four-digit status refused");
    CHECK(eq(headOf("HTTP/1.1 302 Found\r\nLocation: /a\r\nLocation: /b\r\n\r\n"), "bad-response"), "two locations refused");
    CHECK(eq(headOf("HTTP/1.1 200 OK\r\nContent-Type: a\rX-Hidden: b\r\n\r\n"), "bad-response"), "a bare CR inside a header line refused (Codex r6)");
    CHECK(eq(headOf("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"), "done"), "a normal CRLF head still parses");
    CHECK(eq(headOf("HTTP/1.1 200 OK\r\nX: a\x01b\r\n\r\n"), "bad-response") && eq(headOf("HTTP/1.1 200 OK\r\nX: a\x7f\r\n\r\n"), "bad-response"),
          "control octets and DEL in a header refused (Codex r7)");
    CHECK(eq(headOf("HTTP/1.1 200 OK\r\nX: a\tb\r\n\r\n"), "done"), "a tab in a header value is fine");
    CHECK(eq(headOf("HTTP/1.1 200 OK\r\nX: " + std::string(PATH_MAX_LEN + 100, 'a') + "\r\n\r\n"), "done"), "an overlong field nobody reads is skipped, not buffered");
    for (const char* f : {"Content-Encoding", "Transfer-Encoding", "Content-Length"})
        CHECK(eq(headOf(std::string("HTTP/1.1 200 OK\r\n") + f + ": " + std::string(PATH_MAX_LEN + 100, 'a') + "\r\n\r\n"), "bad-response"), f);   // a read field is never cut
    { std::string many = "HTTP/1.1 200 OK\r\n"; for (int i = 0; i < 70; ++i) many += "X-" + std::to_string(i) + ": 1\r\n";
      CHECK(eq(headOf(many + "\r\n"), "bad-response"), "too many headers refused"); }
    { std::string s = "HTTP/1.1 200 OK\r\n"; s += std::string("X: a"); s.push_back('\0'); s += "\r\n\r\n"; CHECK(eq(headOf(s), "bad-response"), "NUL in head refused"); }
    CHECK(eq(headOf("HTTP/1.1 200 OK\r\nTransfer-Encoding : chunked\r\n\r\n"), "bad-response"), "space before the colon refused (Codex r3)");
    CHECK(eq(headOf("HTTP/1.1 200 OK\r\nContent Length: 5\r\n\r\n"), "bad-response"), "field name with a space refused");
    CHECK(eq(headOf("HTTP/1.1 200 OK\r\nX-Ok_1: a\r\n\r\n"), "done"), "token field name accepted");
    CHECK(eq(headOf("HTTP/1.1 302 Found\r\nLocation:\r\nLocation: /b\r\n\r\n"), "bad-response"), "an empty first Location still counts (Codex r4)");
    // ---- GitHub's redirect heads (2026-11-22): a 4,203-byte Content-Security-Policy on both 302s, then 200 ----
    { const std::string C = csp(), R = "HTTP/1.1 302 Found\r\n" + gh() + C + "Location: https://github.com/x/y\r\nContent-Length: 0\r\n\r\n";
      static Head h; h.reset(); int r = 0; for (char c : R) { r = h.push(c); if (r) break; }
      CHECK(C.size() == strlen("Content-Security-Policy: ") + 4203 + 2 && r == 1 && h.status == 302 && eq(h.location, "https://github.com/x/y") && h.length == 0,
            "a 302 with GitHub's 4,203-byte CSP parses; Location and Content-Length are read in full");
      CHECK(eq(headOf("HTTP/1.1 200 OK\r\n" + C + C + C + "Content-Length: 1\r\n\r\n"), "done"), "three such headers fit the 16 KB head limit");
      CHECK(eq(headOf("HTTP/1.1 200 OK\r\n" + C + C + C + C + "Content-Length: 1\r\n\r\n"), "bad-response"), "four do not: the total head limit still holds");
      std::string many = "HTTP/1.1 200 OK\r\n"; for (int i = 0; i < 70; ++i) many += "X-Long-" + std::to_string(i) + ": " + std::string(100, 'v') + "\r\n";
      CHECK(eq(headOf(many + "\r\n"), "bad-response"), "the line limit counts skipped fields too");
      // Malformed bytes inside a skipped field are refused like anywhere else; skipping never hides a second header.
      const std::string half = "Content-Security-Policy: " + std::string(3000, 'a');
      for (const std::string& bad : {half + "\rX-Hidden: 1\r\n", half + "\r \r\n", half + std::string(1, '\0') + "\r\n", half + "\x7f\r\n", half + "\x01\r\n"})
        CHECK(eq(headOf("HTTP/1.1 200 OK\r\n" + bad + "\r\n"), "bad-response"), "bare CR, NUL, DEL or a control octet inside a skipped field refused");
      CHECK(eq(headOf("HTTP/1.1 200 OK\r\n" + C + " folded: x\r\n\r\n"), "bad-response"), "line folding after a long field refused");
      CHECK(eq(headOf("HTTP/1.1 200 OK\r\nContent-Security-Policy : " + std::string(3000, 'a') + "\r\n\r\n"), "bad-response"), "space before the colon of a skipped field refused");
      CHECK(eq(headOf("HTTP/1.1 200 OK\r\nCont\x01ent-Security-Policy: x\r\n\r\n"), "bad-response"), "a control octet in a skipped field name refused");
      CHECK(eq(headOf("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n" + C + "Content-Length: 2\r\n\r\n"), "bad-response"), "conflicting lengths around a long field refused");
      CHECK(eq(headOf("HTTP/1.1 302 Found\r\nLocation: /a\r\n" + C + "Location: /b\r\n\r\n"), "bad-response"), "two locations around a long field refused");
      { static Head s; s.reset(); int q = 0; for (char c : "HTTP/1.1 200 OK\r\n" + C + "Transfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n") { q = s.push(c); if (q) break; }
        CHECK(q == 1 && s.chunked, "a framing field after a long one is still read (a chunked body stays refused)"); }
      { static Head s; s.reset(); int q = 0; for (char c : "HTTP/1.1 200 OK\r\nX-Note: Transfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n") { q = s.push(c); if (q) break; }
        CHECK(q == 1 && !s.chunked && s.length == 5, "a framing field inside another field's value is only a value"); }
      CHECK(eq(headOf("HTTP/1.1 200 OK\r\nContent-Type: " + std::string(PATH_MAX_LEN + 10, 'a') + "\r\nLocation: " + std::string(PATH_MAX_LEN + 20, 'b') + "\r\n\r\n"),
               "bad-response"), "a read field is still refused when too long, after a skipped long one"); }
    // The strict flat-object grammar the index and the bundle head must pass before ArduinoJson (Codex r4).
    for (const char* good : {"{}", "{\"a\":1}", " {\"a\":\"x\\\"y\\u00e9\",\"b\":0}\n"}) CHECK(flatObject(good, strlen(good)), good);
    for (const char* bad : {"{'a':1}", "{a:1}", "{\"a\":'x'}", "{\"a\":1,\"a\":2}", "{\"a\":1}x", "{\"a\":\"x\\u0000\"}", "{\"a\":-1}",
                            "{\"a\":1,}", "{\"a\":01}", "{\"a\":00}", "{\"\\u0061\":1}", "{\"a\":{\"b\":1}}", "{\"a\":[1]}", "{\"a\":true}", "{\"a\":\"x\ty\"}", "[1]", "{\"a\":1"})
        CHECK(!flatObject(bad, strlen(bad)), bad);
    CHECK(eq(headOf("HTTP/1.1 302 Found\r\nLocation: " + std::string(PATH_MAX_LEN + 20, 'a') + "\r\n\r\n"), "bad-response"), "overlong location refused");

    const std::vector<uint8_t> img = image(5000);
    const std::string man = manifestText(1003000), sig = fakeSig(man);
    // ---- the real GitHub shape (2026-11-22): both 302s carry a 4,203-byte CSP, then 200 with the 608-byte index ----
    { std::string idx = indexJson(man, sig, "");
      if (idx.size() < 608) idx = indexJson(man, sig, std::string(608 - idx.size(), 'x'));
      Mock m; publish(m, man, sig, img, "v1.3.0", idx);
      int hops = 0;
      for (auto& kv : m.routes)
          if (kv.second.bytes.rfind("HTTP/1.1 302", 0) == 0) {
              const size_t l = kv.second.bytes.find("Location: ") + 10, e = kv.second.bytes.find("\r\n", l);
              kv.second.bytes = ghRedirect(kv.second.bytes.substr(l, e - l)); ++hops;
          }
      Offer o; Found f;
      CHECK(idx.size() == 608 && hops == 3 && !runCheck(m, o, f) && f == Found::Newer && o.m.version == 1003000 && m.log.size() == 3 &&
            m.log[2].rfind("release-assets.githubusercontent.com ", 0) == 0,
            "GitHub's two redirects with a 4,203-byte CSP each, then the 608-byte orcon-update.json: the newer release is found");
      MockSink s; s.want = img;
      CHECK(!runInstall(m, o, s) && s.activated, "the same heads on the bundle download: installed");
      Offer up; Found same; const Own on13 = {1003000, otap::Variant::Control, "heltec_v2_sx1276_control", 0x330000};
      CHECK(!runCheck(m, up, same, on13) && same == Found::UpToDate, "a board on that release sees it as up to date");
      const std::string latest = "github.com " + std::string(REPO) + "latest/download/orcon-update.json";
      for (const char* to : {"https://evil.example/x", "http://github.com/x", "https://github.com/other/repo/releases/download/v1/x"}) {
          Mock b; publish(b, man, sig, img, "v1.3.0"); b.routes[latest].bytes = ghRedirect(to);
          Offer o2; Found f2; CHECK(eq(runCheck(b, o2, f2), "redirect-not-allowed") && b.connects == 1, to);   // the redirect rules are unchanged
      }
      Mock loop; publish(loop, man, sig, img, "v1.3.0");
      loop.routes[latest].bytes = ghRedirect("https://github.com" + std::string(REPO) + "latest/download/orcon-update.json");
      Offer o3; Found f3; CHECK(eq(runCheck(loop, o3, f3), "too-many-redirects"), "a redirect loop with GitHub's heads still stops after 3"); }
    // ---- check: a newer official release ----
    { Mock m; publish(m, man, sig, img, "v1.3.0"); Offer o; Found f;
      CHECK(!runCheck(m, o, f) && f == Found::Newer && o.m.version == 1003000 && eq(o.notes, "Kleine verbeteringen."), "newer release found");
      CHECK(m.log.size() == 3 && m.log[2].rfind("release-assets.githubusercontent.com ", 0) == 0, "two redirects to the asset host");
      CHECK(m.stops >= 3, "every connection closed");
      // ---- install of exactly that release ----
      MockSink s; s.want = img;
      CHECK(!runInstall(m, o, s) && s.activated && !s.aborted, "install streams, verifies and activates");
      char name[96]; bundleName(o.m, name, sizeof(name));
      CHECK(eq(name, "orcon-control-v1.3.0-0123456.orconfw"), "bundle name from the signed manifest");
      CHECK(m.log[3] == "github.com " + std::string(REPO) + "download/v1.3.0/orcon-control-v1.3.0-0123456.orconfw", "install asks the checked tag, not latest"); }
    // ---- check: same release / older / wrong variant ----
    { Mock m; const std::string same = manifestText(1002003); publish(m, same, fakeSig(same), img, "v1.2.3"); Offer o; Found f;
      CHECK(!runCheck(m, o, f) && f == Found::UpToDate, "same release: up to date"); }
    { Mock m; const std::string old = manifestText(1001000); publish(m, old, fakeSig(old), img, "v1.1.0"); Offer o; Found f;
      CHECK(eq(runCheck(m, o, f), "older-release") && f == Found::None, "older release (downgrade) not offered"); }
    { Mock m; const std::string rx = manifestText(1003000, "rx-only", "heltec_v2_sx1276", false); publish(m, rx, fakeSig(rx), img, "v1.3.0"); Offer o; Found f;
      CHECK(eq(runCheck(m, o, f), "wrong-variant"), "RX-only release not offered to the control image"); }
    { Mock m; const std::string rf = manifestText(1003000, "rf-test", "heltec_v2_sx1276_rftest"); publish(m, rf, fakeSig(rf), img, "v1.3.0"); Offer o; Found f;
      CHECK(eq(runCheck(m, o, f), "wrong-variant"), "RF test image not offered"); }
    { Mock m; const std::string bad = manifestText(1003000, "control", "heltec_v2_sx1276_webctl"); publish(m, bad, fakeSig(bad), img, "v1.3.0"); Offer o; Found f;
      CHECK(eq(runCheck(m, o, f), "wrong-variant"), "other env not offered"); }
    { Mock m; const std::string big = manifestText(1003000, "control", "heltec_v2_sx1276_control", true, 0x330001); publish(m, big, fakeSig(big), img, "v1.3.0"); Offer o; Found f;
      CHECK(eq(runCheck(m, o, f), "image-too-large"), "image larger than the slot not offered"); }
    { Mock m; Own dev = OWN; dev.version = 0; publish(m, man, sig, img, "v1.3.0"); Offer o; Found f;
      CHECK(!runCheck(m, o, f, dev) && f == Found::Newer, "a development build is older than any release"); }
    // ---- check: signature and index ----
    { Mock m; publish(m, man, fakeSig(man + " "), img, "v1.3.0"); Offer o; Found f; CHECK(eq(runCheck(m, o, f), "bad-signature"), "wrong signature"); }
    { Mock m; std::string t2 = man; t2.replace(t2.find("1003000"), 7, "1004000"); publish(m, man, sig, img, "v1.3.0", indexJson(t2, sig));
      Offer o; Found f; CHECK(eq(runCheck(m, o, f), "bad-signature"), "tampered manifest in the index"); }
    for (const std::string& badIdx : {std::string("not json"), std::string("{\"schema\":1,\"manifest\":\"x\",\"sig\":\"00\"}"),
                                      indexJson(man, sig).replace(10, 1, "2"), "{\"schema\":1,\"manifest\":" + jsonStr(man) + ",\"sig\":\"" + sig + "\",\"notes\":\"\",\"x\":1}",
                                      indexJson(man, "zz" + sig), indexJson(man, sig, "regel\neen"), indexJson(man, sig, std::string(NOTES_MAX + 1, 'n')),
                                      indexJson(man, sig, "\xc3\x28")}) {
        Mock m; publish(m, man, sig, img, "v1.3.0", badIdx); Offer o; Found f;
        CHECK(eq(runCheck(m, o, f), "bad-index"), "malformed index refused");
    }
    { const std::string good = indexJson(man, sig);
      for (const std::string& bad : {good.substr(0, good.size() - 1) + ",\"notes\":\"twee\"}", good + "x", good + "{}",
                                     "{\"schema\":1,\"manifest\":" + jsonStr(man) + ",\"sig\":\"" + sig + "\",\"\\u006eotes\":\"\"}",
                                     indexJson(man, sig, "\xed\xa0\x80"), indexJson(man, sig, "\xe0\x80\x80"), indexJson(man, sig, "\xf4\x90\x80\x80")}) {
          Mock m; publish(m, man, sig, img, "v1.3.0", bad); Offer o; Found f;
          CHECK(eq(runCheck(m, o, f), "bad-index"), "duplicate key, trailing data, escaped key or invalid UTF-8 refused (Codex r3)");
      }
      Mock m; publish(m, man, sig, img, "v1.3.0", good + "\n"); Offer o; Found f; CHECK(!runCheck(m, o, f), "trailing whitespace is fine"); }
    { Mock m; std::string q = indexJson(man, sig); q.replace(q.find("\"notes\""), 7, "notes");   // a bare key that ArduinoJson would take
      publish(m, man, sig, img, "v1.3.0", q); Offer o; Found f; CHECK(eq(runCheck(m, o, f), "bad-index"), "bare key in the index refused"); }
    { Mock m; publish(m, man, sig, img, "v1.3.0");   // complete but after the check's deadline: nothing offered (Codex r4)
      for (auto& r : m.routes) if (r.first.find("/idx") != std::string::npos) r.second.slowMs = 3000;
      Offer o; Found f; Limits lim; lim.checkMs = 8000;
      CHECK(eq(check(m, fakeVerify, clockNow, lim, OWN, o, f, IO, sizeof(IO), U, H), "timeout") && f == Found::None, "late index read publishes no offer"); }
    { Mock m; publish(m, man, sig, img, "v1.3.0", indexJson(man, sig, "Ventilatie €")); Offer o; Found f;
      CHECK(!runCheck(m, o, f) && eq(o.notes, "Ventilatie €"), "UTF-8 notes kept"); }
    { Mock m; publish(m, man, sig, img, "v1.3.0", indexJson(man, sig) + std::string(INDEX_MAX, ' ')); Offer o; Found f;
      CHECK(eq(runCheck(m, o, f), "response-too-large"), "oversized index refused before reading it"); }
    // ---- check: network ----
    { Mock m; Offer o; Found f; CHECK(eq(runCheck(m, o, f), "no-release"), "no release published (404)"); }
    { Mock m; m.refuse["github.com"] = "dns-failed"; Offer o; Found f; CHECK(eq(runCheck(m, o, f), "dns-failed"), "DNS failure"); }
    { Mock m; m.refuse["github.com"] = "tls-failed"; Offer o; Found f; CHECK(eq(runCheck(m, o, f), "tls-failed"), "TLS failure (untrusted chain or name)"); }
    { Mock m; publish(m, man, sig, img, "v1.3.0"); m.refuse["release-assets.githubusercontent.com"] = "tls-failed"; Offer o; Found f;
      CHECK(eq(runCheck(m, o, f), "tls-failed"), "TLS failure on the asset host"); }
    { Mock m; m.routes["github.com " + std::string(REPO) + "latest/download/orcon-update.json"].bytes = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
      Offer o; Found f; CHECK(eq(runCheck(m, o, f), "rate-limited"), "rate limit 403"); }
    { Mock m; m.routes["github.com " + std::string(REPO) + "latest/download/orcon-update.json"].bytes = "HTTP/1.1 429 Too Many\r\nContent-Length: 0\r\n\r\n";
      Offer o; Found f; CHECK(eq(runCheck(m, o, f), "rate-limited"), "rate limit 429"); }
    { Mock m; m.routes["github.com " + std::string(REPO) + "latest/download/orcon-update.json"].bytes = "HTTP/1.1 503 Busy\r\nContent-Length: 0\r\n\r\n";
      Offer o; Found f; CHECK(eq(runCheck(m, o, f), "server-error"), "server error"); }
    { Mock m; m.routes["github.com " + std::string(REPO) + "latest/download/orcon-update.json"].bytes = redirect("https://evil.example/x");
      Offer o; Found f; CHECK(eq(runCheck(m, o, f), "redirect-not-allowed") && m.connects == 1, "redirect to a foreign host refused before connecting"); }
    { Mock m; m.routes["github.com " + std::string(REPO) + "latest/download/orcon-update.json"].bytes = redirect("http://github.com" + std::string(REPO) + "x");
      Offer o; Found f; CHECK(eq(runCheck(m, o, f), "redirect-not-allowed"), "redirect to plain HTTP refused"); }
    { Mock m; m.routes["github.com " + std::string(REPO) + "latest/download/orcon-update.json"].bytes = redirect("https://github.com/other/repo/releases/x");
      Offer o; Found f; CHECK(eq(runCheck(m, o, f), "redirect-not-allowed"), "redirect to another repository refused"); }
    { Mock m; const std::string a = std::string(REPO) + "latest/download/orcon-update.json";
      m.routes["github.com " + a].bytes = redirect(a, 307);
      Offer o; Found f; CHECK(eq(runCheck(m, o, f), "too-many-redirects") && m.connects == 4, "redirect loop stops after 3"); }
    { Mock m; m.routes["github.com " + std::string(REPO) + "latest/download/orcon-update.json"].bytes = "HTTP/1.1 302 Found\r\nContent-Length: 0\r\n\r\n";
      Offer o; Found f; CHECK(eq(runCheck(m, o, f), "redirect-not-allowed"), "redirect without Location refused"); }
    { Mock m; publish(m, man, sig, img, "v1.3.0");
      for (auto& r : m.routes) if (r.first.find("/idx") != std::string::npos) r.second.bytes = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n10\r\n";
      Offer o; Found f; CHECK(eq(runCheck(m, o, f), "unsupported-encoding"), "chunked body refused"); }
    { Mock m; publish(m, man, sig, img, "v1.3.0");
      for (auto& r : m.routes) if (r.first.find("/idx") != std::string::npos) r.second.bytes = "HTTP/1.1 200 OK\r\n\r\n{}";
      Offer o; Found f; CHECK(eq(runCheck(m, o, f), "bad-response"), "body without Content-Length refused"); }
    { Mock m; publish(m, man, sig, img, "v1.3.0");
      for (auto& r : m.routes) if (r.first.find("/idx") != std::string::npos) r.second.closeAt = int(r.second.bytes.size()) - 5;
      Offer o; Found f; CHECK(eq(runCheck(m, o, f), "connection-lost"), "connection lost mid-index"); }
    { Mock m; publish(m, man, sig, img, "v1.3.0");
      for (auto& r : m.routes) if (r.first.find("/idx") != std::string::npos) r.second.stallAt = 10;
      Offer o; Found f; CHECK(eq(runCheck(m, o, f), "timeout"), "stalled server times out"); }
    { Mock m; publish(m, man, sig, img, "v1.3.0");
      for (auto& r : m.routes) r.second.slowMs = 2000;   // a byte trickle that never stalls
      Offer o; Found f; CHECK(eq(runCheck(m, o, f), "timeout"), "overall check deadline"); }

    // ---- install: failures never activate anything ----
    Offer good; { Mock m; publish(m, man, sig, img, "v1.3.0"); Found f; runCheck(m, good, f); }
    { Mock m; MockSink s; s.want = img; CHECK(eq(runInstall(m, good, s), "asset-missing") && !s.begun && !s.activated, "missing asset"); }
    { Mock m; publish(m, man, sig, img, "v1.3.0");
      for (auto& r : m.routes) if (r.first.find("/fw") != std::string::npos) r.second.closeAt = int(r.second.bytes.size()) - 1000;
      MockSink s; s.want = img; CHECK(eq(runInstall(m, good, s), "connection-lost") && s.aborted && !s.activated, "interrupted download aborted"); }
    { Mock m; publish(m, man, sig, img, "v1.3.0");
      for (auto& r : m.routes) if (r.first.find("/fw") != std::string::npos) r.second.stallAt = int(r.second.bytes.size()) - 1000;
      MockSink s; s.want = img; CHECK(eq(runInstall(m, good, s), "timeout") && s.aborted && !s.activated, "stalled download aborted"); }
    { Mock m; publish(m, man, sig, img, "v1.3.0"); MockSink s; s.want = img; s.failWriteAt = "flash-write-failed"; s.failAfter = 2000;
      CHECK(eq(runInstall(m, good, s), "flash-write-failed") && s.aborted && !s.activated, "interrupted OTA write aborted"); }
    { Mock m; std::vector<uint8_t> evil = img; evil[4000] ^= 1; publish(m, man, sig, evil, "v1.3.0"); MockSink s; s.want = img;
      CHECK(eq(runInstall(m, good, s), "hash-mismatch") && s.aborted && !s.activated, "tampered image refused by the writer"); }
    { Mock m; const std::string other = manifestText(1003000, "control", "heltec_v2_sx1276_control", true, 4999);
      publish(m, man, sig, img, "v1.3.0");
      for (auto& r : m.routes) if (r.first.find("/fw") != std::string::npos) r.second.bytes = ok(bundle(other, fakeSig(other), image(4999)));
      MockSink s; s.want = img; CHECK(eq(runInstall(m, good, s), "bundle-mismatch") && !s.begun && !s.activated, "bundle of another manifest refused"); }
    { Mock m; publish(m, man, sig, img, "v1.3.0");   // a NUL suffix on the signature: never equal (Codex r4)
      for (auto& r : m.routes) if (r.first.find("/fw") != std::string::npos)
          r.second.bytes = ok("{\"manifest\":" + jsonStr(man) + ",\"sig\":\"" + sig + "\\u0000x\"}\n" + std::string(img.begin(), img.end()));
      MockSink s; s.want = img; const char* e = runInstall(m, good, s); CHECK((eq(e, "bad-bundle") || eq(e, "bundle-mismatch")) && !s.begun, "NUL suffix in the bundle head refused"); }
    { Mock m; publish(m, man, sig, img, "v1.3.0");
      for (auto& r : m.routes) if (r.first.find("/fw") != std::string::npos) r.second.bytes = ok(bundle(man, sig, img) + "x");
      MockSink s; s.want = img; CHECK(eq(runInstall(m, good, s), "size-mismatch") && !s.begun, "trailing bytes refused"); }
    { Mock m; publish(m, man, sig, img, "v1.3.0");
      for (auto& r : m.routes) if (r.first.find("/fw") != std::string::npos) r.second.bytes = ok(std::string(2000, 'x'));
      MockSink s; s.want = img; CHECK(eq(runInstall(m, good, s), "bad-bundle") && !s.begun, "bundle without a head line refused"); }
    { Mock m; publish(m, man, sig, img, "v1.3.0");
      for (auto& r : m.routes) if (r.first.find("/fw") != std::string::npos) r.second.bytes = ok("{\"manifest\":" + jsonStr(man) + ",\"sig\":\"" + sig + "\",\"x\":1}\n" + std::string(img.begin(), img.end()));
      MockSink s; s.want = img; CHECK(eq(runInstall(m, good, s), "bad-bundle") && !s.begun, "bundle head with an extra field refused"); }
    { Mock m; publish(m, man, sig, img, "v1.3.0"); m.refuse["release-assets.githubusercontent.com"] = "dns-failed";
      MockSink s; s.want = img; CHECK(eq(runInstall(m, good, s), "dns-failed") && !s.begun, "network loss before the download"); }
    { Mock m; publish(m, man, sig, img, "v1.3.0");
      for (auto& r : m.routes) if (r.first.find("/fw") != std::string::npos) r.second.bytes = redirect("https://evil.example/fw");
      MockSink s; s.want = img; CHECK(eq(runInstall(m, good, s), "redirect-not-allowed") && !s.begun, "download redirect to a foreign host refused"); }
    // The writer re-checks the release: an offer for an older version than the running one never reaches the flash.
    { Mock m; const std::string old = manifestText(1001000); publish(m, old, fakeSig(old), img, "v1.1.0");
      Offer o; Found f; runCheck(m, o, f);   // refused at the check, but force an install of it anyway
      parseIndex(indexJson(old, fakeSig(old)).c_str(), indexJson(old, fakeSig(old)).size(), o); otap::parseManifest(o.manifest, o.m);
      MockSink s; s.want = img; CHECK(eq(runInstall(m, o, s), "older-release") && !s.activated, "writer refuses a downgrade too"); }
    // The overall deadline bounds every step: a late last chunk or a late redirect never selects an image (Codex r3).
    { Mock m; publish(m, man, sig, img, "v1.3.0"); for (auto& r : m.routes) r.second.slowMs = 400;
      MockSink s; s.want = img; Limits lim; lim.installMs = 4000;
      CHECK(eq(install(m, s, clockNow, lim, good, nullptr, (uint8_t*)IO, sizeof(IO), U, H), "timeout") && !s.activated, "download past its deadline: nothing selected"); }
    { Mock m; publish(m, man, sig, img, "v1.3.0");
      for (auto& r : m.routes) if (r.first.find("/fw") != std::string::npos) r.second.stallAt = int(r.second.bytes.size()) - 1;
      MockSink s; s.want = img; Limits lim; lim.installMs = 20000; lim.stallMs = 15000; const uint32_t t0 = g_now;
      CHECK(eq(install(m, s, clockNow, lim, good, nullptr, (uint8_t*)IO, sizeof(IO), U, H), "timeout") && !s.activated && g_now - t0 <= 20000 + 1500,
            "a stall near the deadline waits only for what is left of it"); }
    { Mock m; publish(m, man, sig, img, "v1.3.0"); for (auto& r : m.routes) r.second.slowMs = 300;
      MockSink s; s.want = img; CHECK(!runInstall(m, good, s) && s.activated, "a slow but steady download completes"); }

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
