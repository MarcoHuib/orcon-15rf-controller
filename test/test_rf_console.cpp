// test_rf_console.cpp — the live RF console ring: sequence numbers, delta reads, overflow
// accounting, size bounds, JSON escaping, ID masking and frame text. Pure C++.
#include "rf_console.h"
#include <cstdio>
#include <cstring>
#include <string>
using namespace rfcon;
static int checks = 0, fails = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static char out[4096];
static std::string js(const Ring& r, uint32_t after, uint32_t max = 16, size_t cap = sizeof(out)) {
    size_t n = r.json(after, max, 5000, out, cap);
    return n ? std::string(out, n) : std::string();
}
static bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

int main() {
    Ring r;
    CHECK(r.next() == 1 && r.oldest() == 1);
    CHECK(js(r, 0) == "{\"now\":5000,\"next\":1,\"oldest\":1,\"lost\":0,\"more\":false,\"ev\":[]}");
    r.add(RAW, 10, "sync #%d %d dBm", 7, -81);
    r.add(RX, 11, "frame");
    CHECK(r.next() == 3 && r.oldest() == 1);
    std::string s = js(r, 0);
    CHECK(has(s, "\"next\":3") && has(s, "{\"s\":1,\"t\":10,\"k\":\"RAW\",\"m\":\"sync #7 -81 dBm\"}") && has(s, "\"k\":\"RX\""));
    s = js(r, 1);
    CHECK(has(s, "\"s\":2") && !has(s, "\"s\":1,") && has(s, "\"lost\":0"));
    CHECK(has(js(r, 2), "\"ev\":[]") && has(js(r, 2), "\"next\":3"));          // up to date: nothing new

    // max per reply, "more" and the next cursor
    for (int i = 0; i < 20; i++) r.add(INFO, 100 + i, "e%d", i);
    s = js(r, 0, 5);
    CHECK(has(s, "\"next\":6") && has(s, "\"more\":true") && has(s, "\"s\":5,") && !has(s, "\"s\":6,"));

    // overflow: the oldest are overwritten, the client learns exactly how many it missed
    Ring o;
    for (uint32_t i = 0; i < Ring::N + 10; i++) o.add(WARN, i, "x%lu", (unsigned long)i);
    CHECK(o.oldest() == 11 && o.next() == Ring::N + 11);
    s = js(o, 3, 2);
    CHECK(has(s, "\"lost\":7") && has(s, "\"s\":11,") && has(s, "\"next\":13"));
    CHECK(has(js(o, 0, 1), "\"lost\":10"));
    CHECK(has(js(o, 9999, 1), "\"s\":11,"));                                     // cursor from a previous boot
    s = js(o, o.next() - 1);
    CHECK(has(s, "\"ev\":[]") && has(s, "\"lost\":0"));

    // escaping, truncation, unknown kind, too small output buffer
    Ring e;
    e.add(ERR, 1, "quote \" backslash \\ tab\tend");
    CHECK(has(js(e, 0), "quote \\\" backslash \\\\ tab\\u0009end"));
    std::string longmsg(300, 'a');
    e.add(99, 2, "%s", longmsg.c_str());
    s = js(e, 1);
    CHECK(has(s, "\"k\":\"INFO\"") && has(s, std::string(99, 'a').c_str()) && !has(s, std::string(100, 'a').c_str()));
    CHECK(e.json(0, 16, 0, out, 20) == 0 && out[0] == 0);                         // never a cut-off JSON
    CHECK(e.json(0, 16, 0, nullptr, 0) == 0);
    CHECK(sizeof(Ring) < 8 * 1024);                                              // fixed, bounded RAM

    // masking and frame text: never a full device ID
    char id[12]; maskId(ramses::DeviceId(29, 123456), id);
    CHECK(!strcmp(id, "29:****56"));
    ramses::Frame f; f.type = ramses::MsgType::I; f.addr[0] = ramses::DeviceId(29, 123456);
    f.has_addr[1] = true; f.addr[1] = ramses::DeviceId(29, 654321); f.has_addr[2] = false;
    f.cmd = 0x22F1; f.len = 3; f.payload[0] = 0; f.payload[1] = 4; f.payload[2] = 4;
    char t[96]; frameText(f, 251, -80, t, sizeof(t));
    CHECK(!strcmp(t, "#251 -80 dBm I 22F1 29:****56>29:****21 pl=000404"));
    CHECK(!strstr(t, "123456") && !strstr(t, "654321"));
    f.has_addr[1] = false; f.has_addr[2] = true; f.addr[2] = f.addr[0]; f.cmd = 0x31DA; f.len = 30;   // broadcast, long payload
    frameText(f, 7, -77, t, sizeof(t));
    CHECK(strstr(t, "29:****56>--") && strstr(t, "\xE2\x80\xA6 (30 B)"));                     // full length shown
    f.len = 64; frameText(f, 4294967295u, -120, t, sizeof(t));
    CHECK(strstr(t, "\xE2\x80\xA6 (64 B)") != nullptr);                                         // worst case still fits
    printf("rf console: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
