// wifi_watch.h — keeps the station on the home network, whatever the router. Pure logic (host-tested:
// test/test_wifi_watch.cpp); net.cpp feeds it the link state and acts on its answer.
//
// The Arduino core (2.0.16) retries a lost connection by itself, but only for the disconnect reasons on
// its own list, and after the first failure never again for any other reason (for example 202 AUTH_FAIL
// from an access point that is still starting up). The device then stays offline until a reset. This
// watch retries whatever the reason, and as a last resort restarts the device, as a power cycle would.
#pragma once
#include <stdint.h>

namespace wifiwatch {
constexpr uint32_t FIRST_RETRY_MS   = 10000;              // the core's own immediate retry goes first
constexpr uint32_t RETRY_EVERY_MS   = 30000;              // a scan and join take seconds: never cut one short
constexpr uint32_t RESTART_AFTER_MS = 15UL * 60 * 1000;   // still offline after 15 minutes: restart

enum class Act : uint8_t { None, Retry, Restart };

class Watch {
public:
    // Every loop pass in normal station mode. up = joined, with an IPv4 address. mayRestart = false while a
    // restart could make the bootloader select the other firmware slot (an unconfirmed update): only retries.
    Act tick(bool up, uint32_t now, bool mayRestart) {
        if (up) { down_ = false; return Act::None; }
        if (!down_) { down_ = true; since_ = now; next_ = now + FIRST_RETRY_MS; ++outages_; return Act::None; }
        if (mayRestart && now - since_ >= RESTART_AFTER_MS) return Act::Restart;
        if (int32_t(now - next_) < 0) return Act::None;   // wrap-safe: millis() wraps after 49.7 days
        next_ = now + RETRY_EVERY_MS;
        ++retries_;
        return Act::Retry;
    }
    bool     down() const { return down_; }
    uint32_t downMs(uint32_t now) const { return down_ ? now - since_ : 0; }
    uint32_t outages() const { return outages_; }
    uint32_t retries() const { return retries_; }

private:
    bool     down_ = false;
    uint32_t since_ = 0, next_ = 0, outages_ = 0, retries_ = 0;
};
}  // namespace wifiwatch
