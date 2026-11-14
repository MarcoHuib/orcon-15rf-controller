// Rolling RF transmit budget for daily use. Pure logic, host-tested; the RF backend keeps
// the State in RTC memory (survives software restarts, panics, watchdogs and OTA reboots,
// not a power cut) and enforces it in the RX task just before the radio is touched.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace txbudget {
constexpr uint32_t MINUTE_MS = 60000, HOUR_MS = 3600000;
constexpr uint8_t PER_MINUTE = 10, PER_HOUR = 60;
// Measured airtime per rolling hour: 3.6 s = 0.1 % duty cycle (the 868.0-868.6 MHz limit is
// 1 %). Normal packets are ~11 ms (53 B) or ~22 ms (105 B): 60 of them stay under 1.4 s, so
// this only bites when packets take far longer on air than they should.
constexpr uint32_t AIR_US_PER_HOUR = 3600000;
constexpr uint32_t MAGIC = 0x4F524232u;          // "ORB2": layout version included

struct Entry { uint64_t ms; uint32_t airUs; uint32_t pad; };
// Plain old data, oldest entry at head. crc covers everything before it.
struct State { uint32_t magic; uint8_t head, count, pad[2]; Entry e[PER_HOUR]; uint32_t crc; };

struct Usage { uint8_t minute = 0, hour = 0; uint32_t airUsHour = 0; };

void reset(State&);
// At boot: keep a valid state carried over a soft reset, else start empty. Entries "in the
// future" (the clock went back) are clamped to now: they count for a full window again.
// Returns true when the earlier history was kept.
bool restore(State&, uint64_t nowMs);
Usage usage(const State&, uint64_t nowMs);
// 0 when n more packets of estAirUs each may start at nowMs, else the ms until they may.
uint32_t waitMs(const State&, uint64_t nowMs, uint8_t n, uint32_t estAirUs);
void record(State&, uint64_t nowMs, uint32_t airUs);   // a packet starts (counted before TX)
void settle(State&, uint32_t measuredAirUs);           // the last packet's measured airtime
uint32_t estimateUs(size_t onairBytes);                // 38.4 kbit/s + ramp
}
