#include "tx_budget.h"
#include <stddef.h>
#include <string.h>

namespace txbudget {

static uint32_t crcOf(const State& s) {   // CRC-32 over every byte before the crc field
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&s);
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < offsetof(State, crc); ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
static void seal(State& s) { s.crc = crcOf(s); }
static Entry& at(State& s, uint8_t i) { return s.e[(s.head + i) % PER_HOUR]; }            // i-th oldest
static const Entry& at(const State& s, uint8_t i) { return s.e[(s.head + i) % PER_HOUR]; }
static uint64_t age(uint64_t now, uint64_t ms) { return now > ms ? now - ms : 0; }
static void prune(State& s, uint64_t now) {
    while (s.count && age(now, at(s, 0).ms) >= HOUR_MS) { s.head = uint8_t((s.head + 1) % PER_HOUR); --s.count; }
}

void reset(State& s) { memset(&s, 0, sizeof(s)); s.magic = MAGIC; seal(s); }

bool restore(State& s, uint64_t now) {
    if (s.magic != MAGIC || s.head >= PER_HOUR || s.count > PER_HOUR || s.crc != crcOf(s)) { reset(s); return false; }
    for (uint8_t i = 0; i < s.count; ++i) if (at(s, i).ms > now) at(s, i).ms = now;   // order is kept
    prune(s, now);
    seal(s);
    return true;
}

Usage usage(const State& s, uint64_t now) {
    Usage u;
    for (uint8_t i = 0; i < s.count; ++i) {
        const Entry& e = at(s, i);
        if (age(now, e.ms) >= HOUR_MS) continue;
        ++u.hour; u.airUsHour += e.airUs;
        if (age(now, e.ms) < MINUTE_MS) ++u.minute;
    }
    return u;
}

uint32_t waitMs(const State& s, uint64_t now, uint8_t n, uint32_t estAirUs) {
    if (!n) return 0;
    const uint64_t need = uint64_t(n) * estAirUs;
    if (n > PER_MINUTE || need > AIR_US_PER_HOUR) return HOUR_MS;      // can never fit
    uint64_t ms[PER_HOUR]; uint32_t air[PER_HOUR]; uint8_t h = 0;      // live entries, oldest first
    for (uint8_t i = 0; i < s.count; ++i) {
        const Entry& e = at(s, i);
        if (age(now, e.ms) < HOUR_MS) { ms[h] = e.ms; air[h] = e.airUs; ++h; }
    }
    uint64_t until = 0;                                                // when all three limits allow it
    if (h + n > PER_HOUR) {
        const uint64_t t = ms[h + n - PER_HOUR - 1] + HOUR_MS;
        if (t > until) until = t;
    }
    uint8_t first = h;                                                 // the last minute is a suffix
    while (first && age(now, ms[first - 1]) < MINUTE_MS) --first;
    const uint8_t m = uint8_t(h - first);
    if (m + n > PER_MINUTE) {
        const uint64_t t = ms[first + (m + n - PER_MINUTE) - 1] + MINUTE_MS;
        if (t > until) until = t;
    }
    uint64_t a = 0;
    for (uint8_t i = 0; i < h; ++i) a += air[i];
    if (a + need > AIR_US_PER_HOUR) {
        uint8_t i = 0;
        while (i < h && a + need > AIR_US_PER_HOUR) a -= air[i++];
        const uint64_t t = ms[i - 1] + HOUR_MS;                         // i >= 1: need alone fits
        if (t > until) until = t;
    }
    if (until <= now) return 0;
    const uint64_t w = until - now;
    return w > HOUR_MS ? HOUR_MS : uint32_t(w);
}

void record(State& s, uint64_t now, uint32_t airUs) {
    prune(s, now);
    if (s.count && now < at(s, uint8_t(s.count - 1)).ms) now = at(s, uint8_t(s.count - 1)).ms;  // stay ordered
    if (s.count == PER_HOUR) { s.head = uint8_t((s.head + 1) % PER_HOUR); --s.count; }      // never expected
    Entry& e = at(s, s.count);
    e.ms = now; e.airUs = airUs; e.pad = 0;
    ++s.count;
    seal(s);
}

void settle(State& s, uint32_t measuredAirUs) {
    if (!s.count) return;
    at(s, uint8_t(s.count - 1)).airUs = measuredAirUs;
    seal(s);
}

uint32_t estimateUs(size_t onairBytes) { return uint32_t((onairBytes * 20834u + 99u) / 100u) + 1000u; }

}  // namespace txbudget
