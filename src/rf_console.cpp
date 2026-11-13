#include "rf_console.h"
#include <stdarg.h>
#include <stdio.h>

namespace rfcon {

const char* kindName(uint8_t k) {
    static const char* const K[] = { "RAW", "RX", "OK", "FILTER", "INFO", "WARN", "ERR" };
    return k < KIND_N ? K[k] : "INFO";
}

void Ring::add(uint8_t kind, uint32_t ms, const char* fmt, ...) {
    Event& e = ev_[(next_ - 1) % N];
    e.seq = next_++; e.ms = ms; e.kind = kind < KIND_N ? kind : uint8_t(INFO);
    va_list a; va_start(a, fmt);
    vsnprintf(e.msg, sizeof(e.msg), fmt, a);               // truncates, always terminated
    va_end(a);
    if (count_ < N) count_++;
}

// Appends with bounds; ok stays false once anything did not fit.
struct Buf {
    char* p; size_t cap, n = 0; bool ok = true;
    Buf(char* out, size_t c) : p(out), cap(c) {}            // C++11 (ESP32 toolchain): not an aggregate
    void put(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (!ok) return;
        va_list a; va_start(a, fmt);
        int k = vsnprintf(p + n, cap - n, fmt, a);
        va_end(a);
        if (k < 0 || size_t(k) >= cap - n) ok = false; else n += size_t(k);
    }
    void str(const char* s) {                               // JSON string body, escaped
        for (; *s && ok; s++) {
            unsigned char c = (unsigned char)*s;
            if (c == '"' || c == '\\') put("\\%c", c);
            else if (c < 0x20) put("\\u%04x", c);
            else put("%c", c);
        }
    }
};

size_t Ring::json(uint32_t after, uint32_t max, uint32_t now, char* out, size_t cap) const {
    if (!out || !cap) return 0;
    if (after >= next_) after = 0;                          // a cursor from an earlier boot
    const uint32_t first = after + 1 > oldest() ? after + 1 : oldest();
    const uint32_t lost = first > after + 1 ? first - after - 1 : 0;
    const uint32_t avail = next_ - first, take = avail < max ? avail : max;
    Buf b{out, cap};
    b.put("{\"now\":%lu,\"next\":%lu,\"oldest\":%lu,\"lost\":%lu,\"more\":%s,\"ev\":[", (unsigned long)now,
          (unsigned long)(first + take), (unsigned long)oldest(), (unsigned long)lost, take < avail ? "true" : "false");
    for (uint32_t s = first; s < first + take; s++) {
        const Event& e = ev_[(s - 1) % N];
        b.put("%s{\"s\":%lu,\"t\":%lu,\"k\":\"%s\",\"m\":\"", s == first ? "" : ",", (unsigned long)e.seq,
              (unsigned long)e.ms, kindName(e.kind));
        b.str(e.msg);
        b.put("\"}");
    }
    b.put("]}");
    if (!b.ok) { out[0] = 0; return 0; }
    return b.n;
}

void maskId(const ramses::DeviceId& d, char (&out)[12]) {
    snprintf(out, sizeof(out), "%02u:****%02lu", d.cls % 100u, (unsigned long)(d.serial % 100));
}

void frameText(const ramses::Frame& f, uint32_t seq, int16_t rssi, char* out, size_t cap) {
    static const char* const T[] = { "I", "RQ", "RP", "W" };
    char src[12] = "--", dst[12] = "--";
    if (f.has_addr[0]) maskId(f.addr[0], src);
    const int d = f.has_addr[1] ? 1 : (f.has_addr[2] && !(f.has_addr[0] && f.addr[2] == f.addr[0])) ? 2 : -1;
    if (d > 0) maskId(f.addr[d], dst);
    char pl[13] = "";                                       // first 6 payload bytes
    const uint8_t n = f.len < 6 ? f.len : 6;
    for (uint8_t i = 0; i < n; i++) snprintf(pl + 2 * i, 3, "%02X", f.payload[i]);
    const unsigned t = unsigned(f.type);
    // Longer payloads end in "… (N B)": the full bytes of 1FC9/10E0 are in /api/setup/capture.
    char more[16] = "";
    if (f.len > 6) snprintf(more, sizeof(more), "\xE2\x80\xA6 (%u B)", f.len);
    snprintf(out, cap, "#%lu %d dBm %s %04X %s>%s pl=%s%s", (unsigned long)seq, rssi, t < 4 ? T[t] : "?", f.cmd,
             src, dst, pl, more);
}

} // namespace rfcon
