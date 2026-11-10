// rf_console.h — the live RF console's event ring. Pure C++, host-tested in
// test/test_rf_console.cpp; fed from the main loop by setup_web.cpp (never from the RX task).
// Fixed storage, no heap: N events of at most MSG chars. Every event gets the next sequence
// number; a client asks for "after=<last seen>" and learns how many it missed when the ring
// overwrote them. Messages hold masked device IDs only (rfcon::maskId).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ramses_codec.h"

namespace rfcon {

// RAW: radio activity (a sync, decoded or not). RX: a checksum-clean decoded frame.
// OK: a press the wizard accepted. FILTER: a remote command that did not count, and why.
enum Kind : uint8_t { RAW, RX, OK, FILTER, INFO, WARN, ERR, KIND_N };
const char* kindName(uint8_t k);

struct Event { uint32_t seq = 0, ms = 0; uint8_t kind = 0; char msg[100] = {0}; };

class Ring {
public:
    static const uint32_t N = 64;
    void add(uint8_t kind, uint32_t ms, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
    uint32_t next() const { return next_; }                 // the next event's sequence number (first: 1)
    uint32_t oldest() const { return next_ - count_; }      // == next() when empty
    // {"now":..,"next":..,"oldest":..,"lost":..,"more":..,"ev":[{"s","t","k","m"},..]} for events with
    // seq > after, at most max. after >= next() (e.g. from before a reboot) restarts at the oldest.
    // Returns the length, or 0 if it did not fit (nothing usable written).
    size_t json(uint32_t after, uint32_t max, uint32_t now, char* out, size_t cap) const;
private:
    Event ev_[N];
    uint32_t next_ = 1, count_ = 0;
};

void maskId(const ramses::DeviceId& d, char (&out)[12]);    // "29:****01": class + last 2 digits
// "#251 -80 dBm I 22F1 29:****01>29:****02 pl=000104" (first 6 payload bytes, "…" when longer)
void frameText(const ramses::Frame& f, uint32_t seq, int16_t rssi, char* out, size_t cap);

} // namespace rfcon
