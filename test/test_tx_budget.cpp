// Rolling RF budget: 10 per minute, 60 per hour, measured airtime per hour, restart carry-over.
#include "tx_budget.h"
#include <cstdio>
#include <cstring>
using namespace txbudget;
static int fails = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static const uint32_t AIR = estimateUs(53);   // a mode command

int main() {
    State s; reset(s);
    CHECK(AIR > 11000 && AIR < 13000);                                  // 53 B at 38.4 kbit/s + ramp
    CHECK(estimateUs(105) > 22000 && estimateUs(105) < 24000);
    // Empty: everything allowed; n beyond the minute limit can never fit.
    CHECK(waitMs(s, 1000, 1, AIR) == 0 && waitMs(s, 1000, 0, AIR) == 0);
    CHECK(waitMs(s, 1000, PER_MINUTE + 1, AIR) == HOUR_MS);
    CHECK(waitMs(s, 1000, 1, AIR_US_PER_HOUR + 1) == HOUR_MS);
    // Minute window: 10 packets 2 s apart, the 11th waits until the first is 60 s old.
    const uint64_t T = 5000000;
    for (int i = 0; i < 10; ++i) { CHECK(waitMs(s, T + 2000 * i, 1, AIR) == 0); record(s, T + 2000 * i, AIR); }
    CHECK(usage(s, T + 18000).minute == 10 && usage(s, T + 18000).hour == 10);
    CHECK(waitMs(s, T + 20000, 1, AIR) == 40000);
    CHECK(waitMs(s, T + 59999, 1, AIR) == 1);
    CHECK(waitMs(s, T + 60000, 1, AIR) == 0);                           // the first just left the window
    CHECK(waitMs(s, T + 60000, 2, AIR) == 2000);                        // offer + ack need two free slots
    CHECK(waitMs(s, T + 62000, 2, AIR) == 0);
    // Hour window: 60 packets, 10 per minute; the 61st waits until the first is an hour old.
    reset(s);
    for (int i = 0; i < 60; ++i) record(s, T + uint64_t(i / 10) * 60000 + (i % 10) * 2000, AIR);
    const uint64_t last = T + 5 * 60000 + 18000;
    CHECK(usage(s, last).hour == 60);
    CHECK(waitMs(s, last + 60000, 1, AIR) == uint32_t(T + HOUR_MS - (last + 60000)));
    CHECK(waitMs(s, T + HOUR_MS, 1, AIR) == 0);
    CHECK(waitMs(s, T + HOUR_MS, 2, AIR) == 2000);
    // Measured airtime: three abnormally long packets fill 3.0 s; 0.6 s more reaches the cap.
    reset(s);
    record(s, T, AIR); settle(s, 1000000);
    record(s, T + 2000, AIR); settle(s, 1000000);
    record(s, T + 4000, AIR); settle(s, 1000000);
    CHECK(usage(s, T + 5000).airUsHour == 3000000);
    CHECK(waitMs(s, T + 6000, 1, AIR) == 0);
    record(s, T + 6000, AIR); settle(s, 600000 - AIR);                  // one more lands exactly on the cap
    CHECK(waitMs(s, T + 8000, 1, AIR) == 0);
    settle(s, 600000 - AIR + 1);                                        // one microsecond over
    CHECK(waitMs(s, T + 8000, 1, AIR) == uint32_t(T + HOUR_MS - (T + 8000)));   // until the first expires
    CHECK(waitMs(s, T + HOUR_MS, 1, AIR) == 0);
    // settle() replaces the estimate (a failed start records 0 airtime but still counts).
    reset(s); record(s, T, AIR); settle(s, 0);
    CHECK(usage(s, T).hour == 1 && usage(s, T).airUsHour == 0);
    // Never more than an hour of waiting is reported.
    for (int i = 1; i < 60; ++i) record(s, T + i, AIR);
    CHECK(waitMs(s, T + 100, 1, AIR) <= HOUR_MS);
    // Ring wraps over many hours; entries stay ordered and old ones drop out.
    reset(s);
    uint64_t t = T;
    for (int i = 0; i < 1000; ++i) { t += 7000; if (waitMs(s, t, 1, AIR) == 0) record(s, t, AIR); }
    const Usage u = usage(s, t);
    CHECK(u.minute <= PER_MINUTE && u.hour <= PER_HOUR && u.hour >= 50);
    // record() keeps chronological order even if the clock reads earlier.
    reset(s); record(s, T + 1000, AIR); record(s, T + 500, AIR);
    CHECK(usage(s, T + 1000).minute == 2 && waitMs(s, T + 1000 + 60000 - 1, PER_MINUTE - 1, AIR) == 1);
    // Restore after a soft reset: a valid state is kept, a corrupted or foreign one starts empty.
    reset(s); record(s, T, AIR); record(s, T + 2000, AIR);
    State copy; memcpy(&copy, &s, sizeof(s));
    CHECK(restore(copy, T + 10000) && usage(copy, T + 10000).minute == 2);
    memcpy(&copy, &s, sizeof(s)); copy.e[0].airUs ^= 1;
    CHECK(!restore(copy, T + 10000) && usage(copy, T + 10000).hour == 0);
    memcpy(&copy, &s, sizeof(s)); copy.magic ^= 1;
    CHECK(!restore(copy, T + 10000) && copy.magic == MAGIC);
    State junk; memset(&junk, 0x5A, sizeof(junk));                      // power-on RTC contents
    CHECK(!restore(junk, T) && usage(junk, T).hour == 0);
    // Entries "in the future" (clock went back) count for a full window from now.
    memcpy(&copy, &s, sizeof(s));
    CHECK(restore(copy, T - 30000) && usage(copy, T - 30000).minute == 2);
    CHECK(waitMs(copy, T - 30000, PER_MINUTE - 1, AIR) == 60000);
    // Old history is pruned on restore.
    memcpy(&copy, &s, sizeof(s));
    CHECK(restore(copy, T + HOUR_MS + 5000) && copy.count == 0);
    printf("tx budget: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
