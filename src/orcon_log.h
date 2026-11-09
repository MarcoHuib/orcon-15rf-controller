// orcon_log.h — in-RAM ring buffer of RAMSES frames (RX + TX), viewable over
// the web with no USB serial. Also auto-detects a REM (remote) controlling a
// FAN so the device can be "cloned" without binding.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ramses_codec.h"

namespace rlog {

static const size_t  CAP = 48;          // ring-buffer depth

// Format a frame as a ramses-style string, e.g.
//   "I --- 29:012345 32:178990 --:------ 22F1 003 000307"
// `out` should be >= 96 bytes. Returns length.
size_t format(const ramses::Frame& f, char* out, size_t cap);

// Record one frame. dir = 'R' (received) or 'T' (transmitted).
void add(char dir, const ramses::Frame& f, int rssi);

// Serialize recent entries (newest last) as a JSON array of
//   {"t":<ms>,"d":"R","s":"<frame str>","r":<rssi>}
size_t toJson(char* out, size_t cap);

// --- clone auto-detect ------------------------------------------------------
// Roles by message, not device class: the sender of 31D9/31DA is the fan; the
// sender of 22F1/22F3 is a remote and its destination the fan.
struct Seen {
    bool             haveRemote = false;
    bool             haveFan    = false;
    ramses::DeviceId remote;
    ramses::DeviceId fan;
};
const Seen& seen();

// --- discovered devices ------------------------------------------------------
// Every sender heard, with the role its messages imply. Discovery only: nothing
// here ever selects or replaces the unit (that is orcon::ids().fan, set explicitly).
enum : uint8_t { ROLE_UNIT = 1, ROLE_REMOTE = 2 };   // sent 31D9/31DA | sent 22F1/22F3
struct Device {
    ramses::DeviceId id;
    uint8_t  roles = 0;
    uint16_t lastCmd = 0;
    uint32_t count = 0, lastMs = 0;
    int16_t  rssi = 0;
};
static const size_t MAX_DEVICES = 12;
size_t devices(const Device** out);      // returns how many entries are valid
size_t devicesJson(char* out, size_t cap); // [{"id","role","n","age_s","rssi","cmd"}...]

} // namespace rlog
