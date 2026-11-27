// Host test of the Matter -> Orcon control rules (idf/components/orcon_matter/orcon_matter_control.h):
// one fan, its off = Afwezig, on again = the last stand (or Auto).
//   test/run_host_tests.sh (matter_control)
#include "orcon_matter_control.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

using namespace orcon_matter;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
static bool same(const char* a, const char* b) { return a == b || (a && b && !std::strcmp(a, b)); }

int main() {
    // Fan modes as the cluster reports them: off is Afwezig; unknown values never become a request.
    CHECK(reqOfFanMode(kFanOff) == Req::AwayOn);
    CHECK(reqOfFanMode(kFanLow) == Req::Low && reqOfFanMode(kFanMedium) == Req::Medium);
    CHECK(reqOfFanMode(kFanHigh) == Req::High && reqOfFanMode(kFanAuto) == Req::Auto);
    CHECK(reqOfFanMode(4) == Req::None && reqOfFanMode(6) == Req::None && reqOfFanMode(0xFE) == Req::None);
    CHECK(reqOfOnOff(false) == Req::AwayOn && reqOfOnOff(true) == Req::AwayOff);

    // Requests become exactly Orcon's five web/Siri keys; "on again" goes to the last confirmed stand, else Auto.
    CHECK(same(keyOfReq(Req::Low, nullptr), "1") && same(keyOfReq(Req::Medium, nullptr), "2"));
    CHECK(same(keyOfReq(Req::High, nullptr), "3") && same(keyOfReq(Req::Auto, nullptr), "auto"));
    CHECK(same(keyOfReq(Req::AwayOn, "2"), "away") && same(keyOfReq(reqOfFanMode(kFanOff), nullptr), "away"));
    CHECK(same(keyOfReq(Req::AwayOff, "2"), "2") && same(keyOfReq(Req::AwayOff, "auto"), "auto"));
    CHECK(same(keyOfReq(Req::AwayOff, nullptr), "auto"));
    CHECK(keyOfReq(Req::None, "1") == nullptr);
    for (uint8_t m : {kFanOff, kFanLow, kFanMedium, kFanHigh, kFanAuto}) CHECK(keyOfReq(reqOfFanMode(m), nullptr) != nullptr);

    // The stand to return to: only stands the unit itself reported; never away, the timer, disabled or unknown.
    CHECK(same(standKeyOfUnit(1), "1") && same(standKeyOfUnit(2), "2") && same(standKeyOfUnit(3), "3"));
    CHECK(same(standKeyOfUnit(4), "auto") && same(standKeyOfUnit(5), "auto"));
    CHECK(!standKeyOfUnit(0) && !standKeyOfUnit(6) && !standKeyOfUnit(7) && !standKeyOfUnit(0xFF));

    // The Matter view follows the unit: Afwezig is the fan off; unknown and disabled show nothing new.
    View v{};
    CHECK(viewOfUnit(0, v) && !v.on && v.fanMode == kFanOff);
    CHECK(viewOfUnit(1, v) && v.on && v.fanMode == kFanLow);
    CHECK(viewOfUnit(2, v) && v.on && v.fanMode == kFanMedium);
    CHECK(viewOfUnit(3, v) && v.on && v.fanMode == kFanHigh && viewOfUnit(6, v) && v.fanMode == kFanHigh);
    CHECK(viewOfUnit(4, v) && v.on && v.fanMode == kFanAuto && viewOfUnit(5, v) && v.fanMode == kFanAuto);
    CHECK(!viewOfUnit(7, v) && !viewOfUnit(0xFF, v));
    // Round trip: a request that the unit then reports shows as exactly that request (no flip-flop).
    for (uint8_t m : {kFanOff, kFanLow, kFanMedium, kFanHigh, kFanAuto}) {
        const char* k = keyOfReq(reqOfFanMode(m), nullptr);
        if (!k) { CHECK(k != nullptr); continue; }
        const uint8_t unit = !std::strcmp(k, "auto") ? 4 : !std::strcmp(k, "away") ? 0 : uint8_t(k[0] - '0');
        CHECK(viewOfUnit(unit, v) && v.fanMode == m && v.on == (m != kFanOff));
    }
    // Placeholder: only the cluster's own start state (Off, On/Off on) shows Auto; a kept Afwezig stays off.
    CHECK(placeholderFor(kFanOff, true) == kFanAuto && placeholderFor(kFanOff, false) == kFanKeep);
    for (uint8_t m : {kFanLow, kFanMedium, kFanHigh, kFanAuto})
        CHECK(placeholderFor(m, true) == kFanKeep && placeholderFor(m, false) == kFanKeep);
    CHECK(percentOfFanMode(kFanLow) == 33 && percentOfFanMode(kFanMedium) == 66 && percentOfFanMode(kFanHigh) == 100);
    CHECK(percentOfFanMode(kFanOff) == 0 && percentOfFanMode(kFanAuto) == 0);
    CHECK(speedOfFanMode(kFanLow) == 1 && speedOfFanMode(kFanMedium) == 2 && speedOfFanMode(kFanHigh) == kSpeedMax);
    CHECK(speedOfFanMode(kFanOff) == 0 && speedOfFanMode(kFanAuto) == 0);
    // The cluster's percent -> speed (ceil(pct * SpeedMax / 100)) lands on the same stand as its percent -> mode.
    for (int pct = 1; pct <= 100; ++pct) {
        const int speed = (pct * kSpeedMax + 99) / 100;
        const uint8_t mode = pct <= 33 ? kFanLow : pct <= 66 ? kFanMedium : kFanHigh;
        CHECK(speed == speedOfFanMode(mode));
    }

    // Names: within Matter's 32-byte label limit.
    CHECK(std::strlen(kDeviceLabel) <= kMaxLabel && std::strlen(kDeviceLabel) > 0);

    // Admission results.
    CHECK(admitted(202, "") && admitted(200, "") && admitted(409, "unit-already-in-mode"));
    CHECK(!admitted(409, "session-busy") && !admitted(429, "tx-budget") && !admitted(503, "rf-unavailable"));
    CHECK(transientRefusal(409, "session-busy") && transientRefusal(429, "too-fast") && transientRefusal(409, "remote-active"));
    CHECK(!transientRefusal(429, "tx-budget") && !transientRefusal(409, "update-busy") && !transientRefusal(409, "profile-required"));
    CHECK(!transientRefusal(403, "identity-conflict") && !transientRefusal(409, nullptr) && !transientRefusal(500, "too-fast"));

    // Pending: the latest request wins, transient refusals retry every 500 ms for at most 10 s, others end at once.
    Pending p;
    CHECK(!p.due(0));
    p.take(Req::Low, 1000);
    p.take(Req::High, 1100);   // the slider moved on before the loop ran: High replaces Low
    CHECK(p.req == Req::High && p.due(1100));
    CHECK(p.retryAfter(409, "session-busy", 1100) && p.req == Req::High && !p.due(1599) && p.due(1600));
    CHECK(p.retryAfter(429, "too-fast", 1600) && p.due(2100));
    CHECK(!p.retryAfter(202, "", 2100) && p.req == Req::None && !p.due(5000));   // admitted: done
    p.take(Req::Auto, 10000);
    CHECK(!p.retryAfter(429, "tx-budget", 10000) && p.req == Req::None);           // final refusal: done at once
    p.take(Req::Medium, 20000);
    CHECK(p.retryAfter(409, "remote-active", 29999));                              // still inside the 10 s
    CHECK(!p.retryAfter(409, "remote-active", 30000) && p.req == Req::None);       // the deadline ends it
    p.take(Req::Low, 0xFFFFFF00u);                                                  // millis() wrap
    CHECK(p.retryAfter(409, "session-busy", 0xFFFFFF00u) && !p.due(0xFFFFFFFFu) && p.due(0x000001F4u));
    CHECK(!p.retryAfter(409, "session-busy", 0xFFFFFF00u + kRetryMs) && p.req == Req::None);

    std::printf(fails ? "matter control: %d FAILED\n" : "matter control: all checks passed\n", fails);
    return fails ? 1 : 0;
}
