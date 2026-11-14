// setup_flow.h — setup wizard logic: read a physical remote step by step, pair, and the
// virtual remote's command status. Pure C++ (time, sync numbers and radio facts are passed
// in), host-tested in test/test_setup.cpp. HTTP glue: setup_web.cpp.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ramses_codec.h"

namespace setupflow {

// The six buttons of the Orcon remote, in reading order.
enum Key : uint8_t { K_AWAY, K_AUTO, K_TIMER, K_1, K_2, K_3, K_N, K_NONE = 0xFF };
extern const char* const KEY_NAME[K_N];          // "away","auto","timer","1","2","3"
uint8_t keyFromName(const char* s);               // K_NONE if unknown
uint8_t modeOfKey(uint8_t k);                     // 22F1 mode byte, 0xFF for timer/none

// The button a received frame proves, or K_NONE. Only I-frames: 22F1 with mode byte
// 00/04/01/02/03 (the third payload byte differs per model: 07 HRC400, 04 MVS-15RHBP),
// or 22F3 (timer; minutes/tail bytes differ, the code alone identifies the button).
uint8_t keyOfFrame(const ramses::Frame& f);

bool parseId(const char* s, ramses::DeviceId& out);   // exactly "CC:SSSSSS", class < 64
bool tokenEqual(const char* a, const char* b);        // constant time for equal lengths
// Browser-side request forgery guard for POSTs: JSON body, and an Origin (when sent)
// that is exactly this host.
bool requestAllowed(const char* contentType, const char* origin, const char* host);
// DNS-rebinding guard: the Host header must name this device (its IP, optional port) or an
// fixed "orcon15rf.local" name; a foreign domain rebound to our IP never gets the token or a POST.
bool hostAllowed(const char* host, const char* deviceIp);
// The request arrived on the setup access point: the socket's local address is the AP address and
// the client is in the AP subnet (addresses as the platform's IPAddress converts them).
bool apClient(uint32_t local, uint32_t remote, uint32_t apIp, uint32_t apMask);
bool uniqueObjectKeys(const char* json, const char* const* keys, size_t nkeys);

enum class St : uint8_t { Idle, Active, Done, Timeout, Cancelled };
const char* stName(St s);

// ---- reading a physical remote ------------------------------------------------
enum class Why : uint8_t { None, Accepted, Duplicate, WrongKey, Unknown, Stale };
const char* whyName(Why w);

struct Step { uint32_t ev = 0; int16_t rssi = 0; uint16_t cmd = 0; uint8_t len = 0; uint8_t pl[8] = {0}; };

class Read {
public:
    static const uint32_t STEP_MS  = 60000;   // per button
    static const uint32_t GUARD_MS = 250;     // no sync number: ignore frames this soon after arming
    St st = St::Idle;
    uint32_t sid = 0;
    ramses::DeviceId remote;
    ramses::DeviceId target{0,0};
    bool targetKnown=false, targetConflict=false;
    uint8_t  step = 0;                 // index of the button expected now
    Step     got[K_N];
    Why      why = Why::None;          // last outcome for a frame of the selected remote
    uint32_t whyEv = 0;
    uint16_t whyCmd = 0;
    uint8_t  whyMode = 0xFF;           // 22F1 mode byte of that frame (0xFF: none)
    uint32_t others = 0;               // remote commands from other devices, ignored (count only)

    // seqNow: the radio's sync count at activation (0 if the radio has none).
    void start(uint32_t sid, const ramses::DeviceId& r, uint32_t now, uint32_t seqNow);
    void cancel() { if (st == St::Active) st = St::Cancelled; }
    void tick(uint32_t now) { if (st == St::Active && int32_t(now - armedMs_) >= int32_t(STEP_MS)) st = St::Timeout; }
    uint32_t leftMs(uint32_t now) const;
    // ev: our monotonic event number; seq: the frame's sync number (0 unknown).
    Why onFrame(const ramses::Frame& f, uint32_t ev, uint32_t seq, uint32_t now, int16_t rssi);
private:
    uint32_t armedMs_ = 0, armedSeq_ = 0;
};

// ---- pairing (TX builds) ------------------------------------------------------
// Bind request = our 1FC9 offer actually sent. RF confirmation = a 1FC9 (not RQ) from
// another device addressed to our ID. Paired = confirmation AND the firmware learned
// that same device as its unit (legacy path) resp. our closing ack was transmitted
// (web-control path, webctlPaired()).
class Pair {
public:
    static const uint32_t WINDOW_MS = 180000;   // orcon::startPairing() window
    St st = St::Idle;
    uint32_t sid = 0;
    bool sent = false, txFailed = false, confirmed = false, ackSent = false;
    ramses::DeviceId from;
    // Web-control pairing (txpoc backend): paired once the unit confirmed (W 1FC9 to
    // us) AND our closing I 1FC9 ack was actually transmitted. Pass webctlPaired() as
    // the fwPaired argument of tick()/paired()/status() on that path.
    bool webctlPaired() const { return confirmed && ackSent; }
    void markAckSent() { ackSent = true; }
    void start(uint32_t sid, uint32_t now) { *this = Pair(); this->sid = sid; st = St::Active; startMs_ = now; }
    void cancel() { if (st == St::Active) st = St::Cancelled; }
    static bool isBindCombo(uint8_t keyMask) { return keyMask == ((1u << K_AUTO) | (1u << K_1)); }
    // A confirmation seen while the offer was still in flight counts only if the offer
    // really went out (the unit can only answer an offer it received).
    void bindResult(bool ok, uint32_t now) { sent = ok; txFailed = !ok; sentMs_ = now; if (!ok) confirmed = false; }
    // offerInFlight: our offer was handed to the radio and its result is not yet known. The
    // unit answers within milliseconds, well before the owner publishes that result.
    void onFrame(const ramses::Frame& f, const ramses::DeviceId& us, bool strict=false, bool offerInFlight=false);
    void tick(uint32_t now, bool fwPaired, const ramses::DeviceId& fan);
    // "ready" | "bind_sent" | "tx_failed" | "rf_confirmed" | "ack_sent" | "paired" | st name
    const char* status(bool fwPaired, const ramses::DeviceId& fan) const;
    bool paired(bool fwPaired, const ramses::DeviceId& fan) const { return confirmed && fwPaired && fan == from; }
    uint32_t leftMs(uint32_t now) const;
    uint32_t sentAtMs() const { return sentMs_; }
private:
    uint32_t startMs_ = 0, sentMs_ = 0;
};

// ---- virtual remote command status (TX builds) ----------------------------------
// accepted -> sent (the radio actually transmitted) -> confirmed (the selected unit
// reported the requested mode in a 31D9 that began after the send, within CONFIRM_MS).
// Timer (22F3) has no unit report that proves it: NotAvailable, never a guess.
class Cmd {
public:
    static const uint32_t CONFIRM_MS = 10000;
    static const uint32_t MIN_GAP_MS = 1500;    // no command flooding (and never an automatic retry)
    enum Confirm : uint8_t { None, Pending, Confirmed, NotConfirmed, NotAvailable };
    uint32_t id = 0;
    uint8_t  key = K_NONE;
    uint16_t minutes = 0;
    bool     sent = false;
    Confirm  confirm = None;
    bool tooSoon(uint32_t now) const { return id && int32_t(now - atMs_) < int32_t(MIN_GAP_MS); }
    void record(uint32_t id, uint8_t key, uint16_t minutes, bool sent, uint32_t now, uint32_t seqNow);
    // A 31D9 from the selected unit: mode byte, the frame's sync number (0 unknown).
    void onUnitMode(uint8_t mode, uint32_t seq);
    void tick(uint32_t now) { if (confirm == Pending && int32_t(now - atMs_) >= int32_t(CONFIRM_MS)) confirm = NotConfirmed; }
    uint32_t ageMs(uint32_t now) const { return now - atMs_; }
private:
    uint32_t atMs_ = 0, seq_ = 0;
};
const char* confirmName(Cmd::Confirm c);

} // namespace setupflow
