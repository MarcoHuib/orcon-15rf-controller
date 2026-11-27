// The pure rules of Matter -> Orcon control, host-tested in test/matter/test_control.cpp.
// The unit shows as ONE fan, "Orcon ventilator": stand 1/2/3 = the fan's Low/Medium/High (slider, three speeds),
// Auto = the fan's Auto mode, Afwezig = the fan switched off (FanMode Off, PercentSetting 0, or the fan's own On/Off
// cluster off). Switching it on again returns to the last stand the unit itself reported, or Auto when there is none.
// A Matter change becomes one Orcon key ("1","2","3","auto","away") for the same admission as the web page and Siri
// (setupweb::matterControl -> ctlAdmit). What Matter shows follows the unit's own reports (31D9), never the request:
// a refused or unconfirmed request falls back to the unit's real mode (docs/MATTER.md).
#pragma once
#include <stdint.h>
#include <string.h>

namespace orcon_matter {

enum class Req : uint8_t { None = 0, Low = 1, Medium = 2, High = 3, Auto = 4, AwayOn = 5, AwayOff = 6 };

// Matter FanModeEnum (Fan Control): Off 0, Low 1, Medium 2, High 3, On 4, Auto 5, Smart 6. When the cluster tells its
// delegate, it has already turned On into High, Smart into Auto and a PercentSetting into a mode (1-33 Low, 34-66
// Medium, 67-100 High, 0 Off: FanControlCluster.cpp).
constexpr uint8_t kFanOff = 0, kFanLow = 1, kFanMedium = 2, kFanHigh = 3, kFanAuto = 5, kFanKeep = 0xFF;

inline Req reqOfFanMode(uint8_t m) {
    switch (m) {
        case kFanOff: return Req::AwayOn;   // Orcon has no "off": the fan's off is Afwezig
        case kFanLow: return Req::Low;
        case kFanMedium: return Req::Medium;
        case kFanHigh: return Req::High;
        case kFanAuto: return Req::Auto;
        default: return Req::None;   // anything unknown: no request
    }
}
// The fan's On/Off cluster: off is Afwezig, on leaves Afwezig.
inline Req reqOfOnOff(bool on) { return on ? Req::AwayOff : Req::AwayOn; }

// Orcon's unit modes (orcon_frames.h): Away 0, Low 1, Medium 2, High 3, Auto 4, Auto2 5, Boost 6, Disable 7, 0xFF
// unknown. Where "on again" returns to: the last stand the unit itself reported; nullptr when there is none.
inline const char* standKeyOfUnit(uint8_t mode) {
    switch (mode) {
        case 1: return "1";
        case 2: return "2";
        case 3: return "3";
        case 4: case 5: return "auto";
        default: return nullptr;   // away, the timer (temporary), disabled, unknown
    }
}

// The Orcon key for a request. "On again" without a confirmed stand this boot is Auto, the unit's own default.
inline const char* keyOfReq(Req r, const char* lastStand) {
    switch (r) {
        case Req::Low: return "1";
        case Req::Medium: return "2";
        case Req::High: return "3";
        case Req::Auto: return "auto";
        case Req::AwayOn: return "away";
        case Req::AwayOff: return lastStand ? lastStand : "auto";
        default: return nullptr;
    }
}

// What Matter shows for the unit's reported mode: the fan's mode, and its On/Off (off = Afwezig).
struct View { uint8_t fanMode; bool on; };
inline bool viewOfUnit(uint8_t mode, View& v) {
    switch (mode) {
        case 0: v = {kFanOff, false}; return true;
        case 1: v = {kFanLow, true}; return true;
        case 2: v = {kFanMedium, true}; return true;
        case 3: case 6: v = {kFanHigh, true}; return true;   // Boost (the 15RF timer) runs high
        case 4: case 5: v = {kFanAuto, true}; return true;
        default: return false;   // unknown or disabled: nothing true to show, so nothing changes
    }
}
// Before the unit's first report (it often sends its mode only as the answer to a command): the cluster's own start
// state (FanMode Off, On/Off still on) shows the placeholder Auto. A mirrored Afwezig (Off with On/Off off, kept
// across a restart) and every other mode stay as they are.
inline uint8_t placeholderFor(uint8_t shown, bool on) { return shown == kFanOff && on ? kFanAuto : kFanKeep; }
inline uint8_t percentOfFanMode(uint8_t m) { return m == kFanLow ? 33 : m == kFanMedium ? 66 : m == kFanHigh ? 100 : 0; }
inline uint8_t speedOfFanMode(uint8_t m) { return m == kFanLow ? 1 : m == kFanMedium ? 2 : m == kFanHigh ? 3 : 0; }
constexpr uint8_t kSpeedMax = 3;   // MultiSpeed: Orcon's three stands

// Refusals that clear by themselves within seconds: an open transaction or wizard session, the 2 s spacing between
// commands (RATE_CTL_MS), a physical remote within the last 3 s (REMOTE_GUARD_MS). Tried again every kRetryEveryMs
// for at most kRetryMs; every other refusal (budget, profile, identity, update, ...) is final at once.
constexpr uint32_t kRetryMs = 10000, kRetryEveryMs = 500;
inline bool transientRefusal(int code, const char* e) {
    return (code == 409 || code == 429) && e &&
           (!strcmp(e, "session-busy") || !strcmp(e, "too-fast") || !strcmp(e, "remote-active"));
}
inline bool admitted(int code, const char* e) {   // queued, a duplicate, or the unit is already there
    return code == 202 || code == 200 || (code == 409 && e && !strcmp(e, "unit-already-in-mode"));
}

// One open request, the latest wins (Woning's slider sends several in a row). Loop task only.
struct Pending {
    Req req = Req::None;
    uint32_t sinceMs = 0, nextMs = 0;
    void take(Req r, uint32_t now) { req = r; sinceMs = now; nextMs = now; }
    bool due(uint32_t now) const { return req != Req::None && int32_t(now - nextMs) >= 0; }
    // After one admission attempt: true = keep it and try again later, false = done (admitted or final refusal).
    bool retryAfter(int code, const char* e, uint32_t now) {
        if (transientRefusal(code, e) && now - sinceMs < kRetryMs) { nextMs = now + kRetryEveryMs; return true; }
        req = Req::None;
        return false;
    }
};

// Names. Not Orcon's product names and no certification claim: a self-built controller for an Orcon unit.
// NodeLabel is a user name: the default below is written once, only when no label is stored (a controller's own
// label, or one a user chose, is never replaced); Matter limits both labels to 32 bytes.
constexpr const char* kDeviceLabel = "Orcon ventilator";
constexpr size_t kMaxLabel = 32;
static_assert(sizeof("Orcon ventilator") - 1 <= kMaxLabel, "NodeLabel max 32 bytes");

}  // namespace orcon_matter
