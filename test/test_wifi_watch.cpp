// The station watch (src/wifi_watch.h): when a lost home-network connection is retried, and when the
// device restarts as a last resort; never while connected, never before the core's own retry had its
// chance, never during an unconfirmed update, and correct across a millis() wrap.
#include "wifi_watch.h"
#include <cstdio>
using namespace wifiwatch;
static int fails = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

// Drives a Watch with a 100 ms loop from t; counts what it asks for.
struct Rig {
    Watch w;
    uint32_t t;
    int retries = 0, restarts = 0;
    uint32_t firstRetry = 0, firstRestart = 0;
    explicit Rig(uint32_t start = 0) : t(start) {}
    void run(bool up, uint32_t ms, bool mayRestart = true, bool joined = false) {
        for (uint32_t i = 0; i < ms; i += 100, t += 100) {   // t may wrap
            const Act a = w.tick(up, up || joined, t, mayRestart);
            if (a == Act::Retry && !retries++) firstRetry = t;
            if (a == Act::Restart && !restarts++) firstRestart = t;
        }
    }
};

int main() {
    {   // Connected for a day: nothing at all.
        Rig r; r.run(true, 24UL * 3600 * 1000);
        CHECK(r.retries == 0 && r.restarts == 0 && !r.w.down() && r.w.outages() == 0);
    }
    {   // A short drop the core repairs by itself (under 10 s): no retry of ours, one outage counted.
        Rig r(5000); r.run(true, 1000); r.run(false, 9900); r.run(true, 60000);
        CHECK(r.retries == 0 && r.restarts == 0 && r.w.outages() == 1 && !r.w.down() && r.w.downMs(r.t) == 0);
    }
    {   // An outage: first retry after 10 s, then every 30 s; the device restarts at 15 minutes, not before.
        Rig r(1000); r.run(true, 1000);
        const uint32_t lost = r.t;
        r.run(false, RESTART_AFTER_MS - 100);
        CHECK(r.firstRetry == lost + FIRST_RETRY_MS);
        CHECK(r.retries == 1 + int((RESTART_AFTER_MS - 100 - FIRST_RETRY_MS) / RETRY_EVERY_MS));
        CHECK(r.restarts == 0 && r.w.down() && r.w.downMs(r.t) == RESTART_AFTER_MS - 100);
        r.run(false, 200);
        CHECK(r.restarts > 0 && r.firstRestart == lost + RESTART_AFTER_MS);
    }
    {   // Back online: the next outage starts over (10 s wait, a fresh 15 minutes).
        Rig r; r.run(true, 1000); r.run(false, 14 * 60000UL); r.run(true, 5000);
        CHECK(!r.w.down() && r.restarts == 0);
        const int before = r.retries;
        r.run(false, 9900); CHECK(r.retries == before);
        r.run(false, 200);  CHECK(r.retries == before + 1);
        r.run(false, 14 * 60000UL); CHECK(r.restarts == 0);        // not 15 minutes since THIS outage began
        r.run(false, 60000UL);       CHECK(r.restarts > 0);
        CHECK(r.w.outages() == 2);
    }
    {   // An unconfirmed update: never a restart (it could select the other slot), the retries go on;
        // once the update is settled, the long outage restarts the device.
        Rig r; r.run(true, 1000); r.run(false, 60 * 60000UL, false);
        CHECK(r.restarts == 0 && r.retries > 100);
        r.run(false, 200, true);
        CHECK(r.restarts > 0);
    }
    {   // millis() wraps during the outage: same timing as anywhere else.
        Rig r(0xFFFFFFFFu - 4000); r.run(true, 1000);
        const uint32_t lost = r.t;
        r.run(false, RESTART_AFTER_MS + 100);
        CHECK(r.firstRetry == lost + FIRST_RETRY_MS && r.firstRestart == lost + RESTART_AFTER_MS);
        CHECK(r.retries == 1 + int((RESTART_AFTER_MS - FIRST_RETRY_MS) / RETRY_EVERY_MS));
    }
    {   // Down from the very first tick (no "up" seen before): the same rules.
        Rig r(123); r.run(false, FIRST_RETRY_MS + 100);
        CHECK(r.retries == 1 && r.firstRetry == 123 + FIRST_RETRY_MS && r.w.outages() == 1);
    }
    {   // The address is lost while still joined (DHCP renewing): no retry cuts DHCP short for 60 s;
        // after that the retries resume, and the 15-minute restart still applies.
        Rig r; r.run(true, 1000);
        const uint32_t lost = r.t;
        r.run(false, DHCP_WINDOW_MS, true, true);
        CHECK(r.retries == 0);
        r.run(false, 200, true, true);
        CHECK(r.retries == 1 && r.firstRetry >= lost + DHCP_WINDOW_MS && r.firstRetry <= lost + DHCP_WINDOW_MS + 200);
        r.run(false, RESTART_AFTER_MS, true, true);
        CHECK(r.restarts > 0 && r.firstRestart == lost + RESTART_AFTER_MS);
    }
    {   // A retry ends the join; the next join gets a fresh DHCP window, then a retry 30 s later at the earliest.
        Rig r; r.run(true, 1000); r.run(false, 12000);              // first retry at 10 s
        CHECK(r.retries == 1);
        r.run(false, 3000, true, true);                              // joined again 2 s after it, waiting for DHCP
        r.run(false, DHCP_WINDOW_MS - 3000 - 100, true, true);
        CHECK(r.retries == 1);                                       // inside the window: nothing
        r.run(false, 3000, true, true);
        CHECK(r.retries == 2);                                       // window over: retry
        r.run(false, 1000);                                          // that retry dropped the join
        r.run(false, 20000, true, true);                             // joined again: a new window
        CHECK(r.retries == 2);
    }
    {   // A retry while joined, and the next sample is joined again (the drop fell between two samples):
        // that join still gets a fresh 60 s DHCP window, not the 30 s retry cadence (Codex WIFI-05).
        Rig r; r.run(true, 1000);
        r.run(false, DHCP_WINDOW_MS + 200, true, true);             // window over: the first retry
        CHECK(r.retries == 1);
        r.run(false, DHCP_WINDOW_MS - 300, true, true);             // joined at every sample since
        CHECK(r.retries == 1);                                       // no retry 30 s later
        r.run(false, 500, true, true);
        CHECK(r.retries == 2);                                       // after a full window
    }
    {   // An address arrives inside the window: back online, nothing else happens.
        Rig r; r.run(true, 1000); r.run(false, 5000); r.run(false, 20000, true, true); r.run(true, 60000);
        CHECK(r.retries == 0 && r.restarts == 0 && !r.w.down() && r.w.outages() == 1);
    }
    printf("wifi_watch: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
