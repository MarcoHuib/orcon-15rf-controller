// fw_update.cpp — see fw_update.h. Pure C++ (ArduinoJson only), so the host tests run exactly this code.
#include "fw_update.h"
#include <ArduinoJson.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

namespace fwup {

// ---- versions -------------------------------------------------------------------------------------------------
static bool part(const char*& s, uint32_t& v) {
    if (!isdigit((unsigned char)*s)) return false;
    if (*s == '0' && isdigit((unsigned char)s[1])) return false;   // no leading zeros (SemVer)
    v = 0; int n = 0;
    while (isdigit((unsigned char)*s)) { if (++n > 3) return false; v = v * 10 + uint32_t(*s++ - '0'); }
    return true;
}
bool parseSemver(const char* s, uint32_t& out) {
    if (!s) return false;
    if (*s == 'v') ++s;
    uint32_t a, b, c;
    if (!part(s, a) || *s++ != '.' || !part(s, b) || *s++ != '.' || !part(s, c) || *s) return false;
    out = a * 1000000u + b * 1000u + c;
    return out != 0;
}
void semverText(uint32_t v, char out[16]) {
    if (!v) snprintf(out, 16, "dev");
    else if (v > 999999999u) snprintf(out, 16, "?");
    else snprintf(out, 16, "%lu.%lu.%lu", (unsigned long)(v / 1000000u), (unsigned long)(v / 1000u % 1000u), (unsigned long)(v % 1000u));
}

// ---- names and URLs ----------------------------------------------------------------------------------------------
static const char GH[] = "https://github.com/" ORCON_RELEASE_REPO "/releases/";
static const char REPO_PREFIX[] = "/" ORCON_RELEASE_REPO "/releases/";
bool bundleName(const otap::Manifest& m, char* out, size_t n) {
    char v[16]; semverText(m.version, v);
    if (!m.version || m.version > 999999999u || strlen(m.commit) != 40 || m.variant == otap::Variant::None) return false;
    const int k = snprintf(out, n, "orcon-%s-v%s-%.7s.orconfw", otap::variantName(m.variant), v, m.commit);
    return k > 0 && size_t(k) < n;
}
bool indexUrl(char* out, size_t n) {
    const int k = snprintf(out, n, "%slatest/download/%s", GH, INDEX_ASSET);
    return k > 0 && size_t(k) < n;
}
bool bundleUrl(const otap::Manifest& m, char* out, size_t n) {
    char name[80], v[16];
    if (!bundleName(m, name, sizeof(name))) return false;
    semverText(m.version, v);
    const int k = snprintf(out, n, "%sdownload/v%s/%s", GH, v, name);
    return k > 0 && size_t(k) < n;
}

bool parseUrl(const char* url, Url& u) {
    if (!url || strncmp(url, "https://", 8)) return false;
    const char* p = url + 8;
    size_t h = 0;
    while (*p && *p != '/' && *p != ':') {
        const char c = (char)tolower((unsigned char)*p++);
        if (!(isalnum((unsigned char)c) || c == '.' || c == '-') || h + 1 >= HOST_MAX) return false;   // also refuses '@', '?', '#'
        u.host[h++] = c;
    }
    u.host[h] = 0;
    if (!h || u.host[0] == '.' || u.host[0] == '-' || u.host[h - 1] == '.' || strstr(u.host, "..")) return false;
    if (*p == ':') { if (strncmp(p, ":443", 4) || (p[4] && p[4] != '/')) return false; p += 4; }
    if (!*p) { strcpy(u.path, "/"); return true; }
    const size_t n = strlen(p);
    if (n >= PATH_MAX_LEN) return false;
    for (size_t i = 0; i < n; ++i) if ((unsigned char)p[i] <= 0x20 || (unsigned char)p[i] >= 0x7f || p[i] == '#' || p[i] == '\\') return false;
    memcpy(u.path, p, n + 1);
    return true;
}
bool allowedUrl(const Url& u) {
    if (!strcmp(u.host, "github.com"))   // this repository's releases only; nothing encoded or relative there
        return !strncmp(u.path, REPO_PREFIX, sizeof(REPO_PREFIX) - 1) && !strstr(u.path, "..") && !strchr(u.path, '%');
    return !strcmp(u.host, "release-assets.githubusercontent.com") || !strcmp(u.host, "objects.githubusercontent.com");
}
bool resolveLocation(const char* loc, const Url& base, Url& out) {
    if (!loc) return false;
    if (!strncmp(loc, "https://", 8)) return parseUrl(loc, out);
    if (loc[0] != '/' || loc[1] == '/') return false;          // only an absolute path on the same host
    char probe[PATH_MAX_LEN + 16];
    const int k = snprintf(probe, sizeof(probe), "https://%s%s", base.host, loc);
    if (k <= 0 || size_t(k) >= sizeof(probe)) return false;
    return parseUrl(probe, out);
}

// ---- response head ------------------------------------------------------------------------------------------------
static const size_t HEAD_TOTAL_MAX = 16384, HEAD_LINES_MAX = 64;
static bool ieq(const char* a, const char* b) { return !strcasecmp(a, b); }
int Head::line() {
    buf_[n_] = 0;
    if (n_ && buf_[n_ - 1] == '\r') buf_[--n_] = 0;
    if (lines_++ == 0) {   // status line: HTTP/1.x NNN [reason]
        if ((strncmp(buf_, "HTTP/1.1 ", 9) && strncmp(buf_, "HTTP/1.0 ", 9)) || !isdigit((unsigned char)buf_[9]) ||
            !isdigit((unsigned char)buf_[10]) || !isdigit((unsigned char)buf_[11]) || (buf_[12] && buf_[12] != ' ')) {
            err_ = "bad-response"; return -1;
        }
        status = (buf_[9] - '0') * 100 + (buf_[10] - '0') * 10 + (buf_[11] - '0');
        n_ = 0; return 0;
    }
    if (!n_) { done_ = true; return 1; }
    if (buf_[0] == ' ' || buf_[0] == '\t') { err_ = "bad-response"; return -1; }   // obsolete line folding
    char* colon = strchr(buf_, ':');
    if (!colon || colon == buf_) { err_ = "bad-response"; return -1; }
    for (const char* p = buf_; p < colon; ++p)   // an RFC 9110 token: no space before the colon, no odd characters
        if (!isalnum((unsigned char)*p) && !strchr("!#$%&'*+-.^_`|~", *p)) { err_ = "bad-response"; return -1; }
    *colon = 0;
    char* v = colon + 1;
    while (*v == ' ' || *v == '\t') ++v;
    for (char* e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
    if (ieq(buf_, "Content-Length")) {
        const size_t k = strlen(v);
        if (!k || k > 10) { err_ = "bad-response"; return -1; }
        int64_t x = 0;
        for (size_t i = 0; i < k; ++i) { if (!isdigit((unsigned char)v[i])) { err_ = "bad-response"; return -1; } x = x * 10 + (v[i] - '0'); }
        if (x > 0x7fffffff || (length >= 0 && length != x)) { err_ = "bad-response"; return -1; }
        length = x;
    } else if (ieq(buf_, "Transfer-Encoding")) {
        if (!ieq(v, "identity")) chunked = true;
    } else if (ieq(buf_, "Content-Encoding")) {
        if (!ieq(v, "identity")) encoded = true;
    } else if (ieq(buf_, "Location")) {
        if (strlen(v) >= sizeof(location) || hasLocation_) { err_ = "bad-response"; return -1; }   // even an empty first one
        strcpy(location, v); hasLocation_ = true;
    }
    n_ = 0;
    return 0;
}
int Head::push(char c) {
    if (err_) return -1;
    if (done_) return 1;
    if (++total_ > HEAD_TOTAL_MAX || lines_ > HEAD_LINES_MAX) { err_ = "bad-response"; return -1; }
    if (c == '\n') return line();
    if (c == 0 || n_ + 1 >= sizeof(buf_)) { err_ = "bad-response"; return -1; }
    buf_[n_++] = c;
    return 0;
}

// ---- HTTP GET over the transport ----------------------------------------------------------------------------------
static bool expired(Clock now, uint32_t deadline) { return int32_t(deadline - now()) <= 0; }
// What is left of the overall deadline, capped by a per-step limit: a step that starts late never runs past it.
static uint32_t budget(Clock now, uint32_t deadline, uint32_t cap) {
    const int32_t left = int32_t(deadline - now());
    return left <= 0 ? 0 : uint32_t(left) < cap ? uint32_t(left) : cap;
}
// Exactly n bytes, or why not (the overall deadline counts too).
static const char* readExact(Transport& t, uint8_t* b, size_t n, Clock now, uint32_t deadline, uint32_t stallMs) {
    while (n) {
        if (expired(now, deadline)) return "timeout";
        const int k = t.read(b, n, budget(now, deadline, stallMs));
        if (k == -1) return "timeout";
        if (k <= 0 || size_t(k) > n) return "connection-lost";
        b += k; n -= size_t(k);
    }
    return nullptr;
}
static bool send(Transport& t, const char* s) { return t.write(s, strlen(s)); }

const char* httpGet(Transport& t, const char* url, Clock now, uint32_t deadline, const Limits& lim,
                    uint32_t maxBody, uint32_t& bodyLen, Url& u, Head& h) {
    bodyLen = 0;
    if (!parseUrl(url, u) || !allowedUrl(u)) return "redirect-not-allowed";
    for (int hop = 0;; ++hop) {
        if (expired(now, deadline)) return "timeout";
        if (!t.connect(u.host, budget(now, deadline, lim.connectMs))) { const char* e = t.connectError(); return expired(now, deadline) ? "timeout" : e ? e : "connect-failed"; }
        if (!(send(t, "GET ") && send(t, u.path) && send(t, " HTTP/1.1\r\nHost: ") && send(t, u.host) &&
              send(t, "\r\nUser-Agent: orcon-heltec-v2\r\nAccept: application/octet-stream\r\nAccept-Encoding: identity\r\n"
                      "Connection: close\r\n\r\n"))) { t.stop(); return "connection-lost"; }
        h.reset();
        for (int r = 0; r == 0;) {
            uint8_t c;
            if (const char* e = readExact(t, &c, 1, now, deadline, lim.stallMs)) { t.stop(); return e; }
            r = h.push((char)c);
            if (r < 0) { t.stop(); return h.error(); }
        }
        const int s = h.status;
        if (s == 301 || s == 302 || s == 303 || s == 307 || s == 308) {
            t.stop();
            if (hop >= 3) return "too-many-redirects";
            if (!resolveLocation(h.location, u, u) || !allowedUrl(u)) return "redirect-not-allowed";
            continue;
        }
        if (s == 200) {
            if (h.chunked || h.encoded) { t.stop(); return "unsupported-encoding"; }
            if (h.length < 0) { t.stop(); return "bad-response"; }
            if (h.length > int64_t(maxBody)) { t.stop(); return "response-too-large"; }
            bodyLen = uint32_t(h.length);
            return nullptr;
        }
        t.stop();
        if (s == 404) return "not-found";
        if (s == 403 || s == 429) return "rate-limited";
        if (s >= 500) return "server-error";
        return "http-error";
    }
}

// ---- index and offer ------------------------------------------------------------------------------------------------
static bool hexOk(const char* s) {
    const size_t n = strlen(s);
    if (n < 16 || n > SIG_MAX || n % 2) return false;
    for (size_t i = 0; i < n; ++i) if (!isxdigit((unsigned char)s[i])) return false;
    return true;
}
static bool textOk(const char* s, size_t n) {   // printable, well-formed UTF-8, no control characters
    for (size_t i = 0; i < n;) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f) return false;
        size_t k = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 && c >= 0xC2 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 && c <= 0xF4 ? 4 : 0;
        if (!k || i + k > n) return false;
        for (size_t j = 1; j < k; ++j) if (((unsigned char)s[i + j] & 0xC0) != 0x80) return false;
        const unsigned char c1 = k > 1 ? (unsigned char)s[i + 1] : 0;
        if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0) || (c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90)) return false;
        i += k;
    }
    return true;
}
bool flatObject(const char* s, size_t n) {
    size_t i = 0;
    auto ws = [&] { while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; };
    auto str = [&](bool key, size_t& from, size_t& len) {   // a "..." string; keys: no escapes
        if (i >= n || s[i] != '"') return false;
        from = ++i;
        while (i < n && s[i] != '"') {
            const unsigned char c = (unsigned char)s[i];
            if (c < 0x20) return false;
            if (c == '\\') {
                if (key || i + 1 >= n) return false;
                const char e = s[i + 1];
                if (e == 'u') { if (i + 5 >= n || !strncmp(s + i + 2, "0000", 4)) return false;
                                for (int k = 2; k < 6; ++k) if (!isxdigit((unsigned char)s[i + k])) return false; i += 6; continue; }
                if (!strchr("\"\\/bfnrt", e)) return false;
                i += 2; continue;
            }
            ++i;
        }
        if (i >= n) return false;
        len = i - from; ++i;
        return true;
    };
    const char* keys[8]; size_t lens[8], nk = 0;
    ws();
    if (i >= n || s[i++] != '{') return false;
    ws();
    if (i < n && s[i] == '}') { ++i; ws(); return i == n; }
    for (;;) {
        size_t f, l;
        if (!str(true, f, l) || nk == 8) return false;
        for (size_t j = 0; j < nk; ++j) if (lens[j] == l && !memcmp(keys[j], s + f, l)) return false;
        keys[nk] = s + f; lens[nk++] = l;
        ws();
        if (i >= n || s[i++] != ':') return false;
        ws();
        if (i < n && s[i] == '"') { if (!str(false, f, l)) return false; }
        else { const size_t d = i; while (i < n && isdigit((unsigned char)s[i])) ++i; if (i == d || i - d > 10) return false; }
        ws();
        if (i < n && s[i] == ',') { ++i; ws(); continue; }
        if (i < n && s[i] == '}') { ++i; break; }
        return false;
    }
    ws();
    return i == n;
}
static bool copyString(JsonVariantConst v, char* out, size_t cap) {
    if (!v.is<const char*>()) return false;
    const char* s = v.as<const char*>();
    const size_t n = v.as<JsonString>().size();
    if (n != strlen(s) || n >= cap) return false;   // no embedded NUL, bounded
    memcpy(out, s, n + 1);
    return true;
}
const char* parseIndex(const char* json, size_t n, Offer& o) {
    o = Offer();
    if (!json || !n || n > INDEX_MAX || !flatObject(json, n)) return "bad-index";
    JsonDocument d;
    if (deserializeJson(d, json, n) || !d.is<JsonObject>()) return "bad-index";
    JsonObjectConst obj = d.as<JsonObjectConst>();
    if (obj.size() != 4) return "bad-index";
    for (JsonPairConst kv : obj) {
        const char* k = kv.key().c_str();
        if (strcmp(k, "schema") && strcmp(k, "manifest") && strcmp(k, "sig") && strcmp(k, "notes")) return "bad-index";
    }
    if (!obj["schema"].is<int>() || obj["schema"].as<int>() != 1) return "bad-index";
    if (!copyString(obj["manifest"], o.manifest, sizeof(o.manifest)) || !o.manifest[0]) return "bad-index";
    if (!copyString(obj["sig"], o.sig, sizeof(o.sig)) || !hexOk(o.sig)) return "bad-index";
    if (!copyString(obj["notes"], o.notes, sizeof(o.notes)) || !textOk(o.notes, strlen(o.notes))) return "bad-index";
    return nullptr;
}
const char* judge(const Offer& o, const Own& own) {
    const otap::Manifest& m = o.m;
    if (m.v != 2 || !m.version || m.version > 999999999u) return "bad-index";
    if (m.variant != own.variant || strcmp(m.env, own.env)) return "wrong-variant";
    if (m.version < own.version) return "older-release";
    if (const char* e = otap::checkManifest(m, "", "", own.slotSize, own.version)) return e;   // v2: mac/nonce unused
    if (m.version == own.version) return "up-to-date";
    return nullptr;
}

const char* memoryOk(uint32_t freeHeap, uint32_t largestBlock) {
    return freeHeap < MIN_FREE_HEAP || largestBlock < MIN_FREE_BLOCK ? "low-memory" : nullptr;
}

const char* check(Transport& t, Verify verify, Clock now, const Limits& lim, const Own& own, Offer& offer, Found& found,
                  char* io, size_t ioLen, Url& u, Head& h) {
    found = Found::None;
    char url[160];
    if (!indexUrl(url, sizeof(url)) || ioLen < INDEX_MAX + 1) return "internal";
    const uint32_t deadline = now() + lim.checkMs;
    uint32_t len = 0;
    const char* e = httpGet(t, url, now, deadline, lim, INDEX_MAX, len, u, h);
    if (e) return !strcmp(e, "not-found") ? "no-release" : e;   // nothing published yet, or no index in the newest release
    e = readExact(t, (uint8_t*)io, len, now, deadline, lim.stallMs);
    t.stop();
    if (!e && expired(now, deadline)) e = "timeout";   // late but complete: still nothing is offered (Codex r4)
    if (e) return e;
    io[len] = 0;
    if ((e = parseIndex(io, len, offer))) return e;
    if (!verify(offer.manifest, strlen(offer.manifest), offer.sig)) return "bad-signature";
    if ((e = otap::parseManifest(offer.manifest, offer.m))) return e;
    e = judge(offer, own);
    if (!e) { found = Found::Newer; return nullptr; }
    if (!strcmp(e, "up-to-date")) { found = Found::UpToDate; return nullptr; }
    return e;
}

// ---- install --------------------------------------------------------------------------------------------------------
static const size_t BUNDLE_HEAD_MAX = 1024;
const char* install(Transport& t, Sink& sink, Clock now, const Limits& lim, const Offer& offer, Progress progress,
                    uint8_t* io, size_t ioLen, Url& u, Head& h) {
    char url[200];
    if (!bundleUrl(offer.m, url, sizeof(url)) || ioLen < BUNDLE_HEAD_MAX + 1 || !offer.m.size) return "internal";
    const uint32_t deadline = now() + lim.installMs;
    uint32_t len = 0;
    const char* e = httpGet(t, url, now, deadline, lim, uint32_t(BUNDLE_HEAD_MAX + 1) + offer.m.size, len, u, h);
    if (e) return !strcmp(e, "not-found") ? "asset-missing" : e;
    // The first line: {"manifest":...,"sig":...}, exactly the signed pair the check verified.
    size_t n = 0;
    for (;;) {
        if (n >= BUNDLE_HEAD_MAX || n >= len) { t.stop(); return "bad-bundle"; }
        if ((e = readExact(t, io + n, 1, now, deadline, lim.stallMs))) { t.stop(); return e; }
        if (io[n] == '\n') break;
        ++n;
    }
    {
        JsonDocument d;
        if (!flatObject((const char*)io, n) || deserializeJson(d, (const char*)io, n) || !d.is<JsonObject>() || d.as<JsonObjectConst>().size() != 2 ||
            !d["manifest"].is<const char*>() || !d["sig"].is<const char*>()) { t.stop(); return "bad-bundle"; }
        if (d["manifest"].as<JsonString>().size() != strlen(offer.manifest) || d["sig"].as<JsonString>().size() != strlen(offer.sig) ||
            strcmp(d["manifest"].as<const char*>(), offer.manifest) || strcmp(d["sig"].as<const char*>(), offer.sig)) {   // lengths: no NUL suffix
            t.stop(); return "bundle-mismatch";
        }
    }
    if (uint64_t(len) != uint64_t(n) + 1 + offer.m.size) { t.stop(); return "size-mismatch"; }
    if ((e = sink.begin(offer.manifest, offer.sig))) { t.stop(); sink.abort(e); return e; }
    uint32_t done = 0;
    if (progress) progress(0, offer.m.size);
    while (done < offer.m.size) {
        const size_t want = offer.m.size - done < ioLen ? offer.m.size - done : ioLen;
        if (expired(now, deadline)) { e = "timeout"; break; }
        const int k = t.read(io, want, budget(now, deadline, lim.stallMs));
        if (k == -1) { e = "timeout"; break; }
        if (k <= 0 || size_t(k) > want) { e = "connection-lost"; break; }
        if ((e = sink.write(io, size_t(k)))) break;
        done += uint32_t(k);
        if (progress) progress(done, offer.m.size);
    }
    t.stop();
    if (!e && expired(now, deadline)) e = "timeout";   // the last bytes came after the deadline: nothing is selected
    if (e) { sink.abort(e); return e; }
    if ((e = sink.finish())) { sink.abort(e); return e; }
    return nullptr;
}

}  // namespace fwup
