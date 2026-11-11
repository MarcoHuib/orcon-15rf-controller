// Pure transaction model: no Arduino, radio driver, SPI or NVS.
#pragma once
#include <atomic>
#include <stdint.h>
#include <stddef.h>
#include "orcon_frames.h"
#include "orcon_log.h"
namespace txpoc {
constexpr uint32_t QUEUE_MS=500, OWNER_MS=100;
// Match the proven CC1101 UART training (RF_PREAMBLE_BYTES + 2).
constexpr unsigned TX_PREAMBLE_BYTES=8;
constexpr uint32_t TRANSACTION_MS=1000, RATE_MS=10000;
// Web-control admission floor. Actual RF timing/power still require measurement.
constexpr uint32_t RATE_CTL_MS=2000;
// Unit status window. Measured (HELTEC_V2_RX.md, measurement 2): the unit answered 16/17
// remote presses within 300 ms and repeats ~1 s later; 3 s covers both plus RX-task lag.
constexpr uint32_t CONFIRM_MS=3000;
constexpr uint32_t EXPIRY_GRACE_MS=250;      // loop-time slack so a timely, still-queued frame counts
constexpr uint32_t REMOTE_GUARD_MS=3000;     // a remote action this close before TX = interference
constexpr uint32_t UNIT_STATE_MAX_AGE_MS=600000; // prior unit mode must be this fresh (PoC choice)
constexpr uint32_t HEALTH_MAX_AGE_MS=250;    // RX-task heartbeat (one pass ~5 ms)
bool due(uint32_t now, uint32_t deadline);
bool validIds(const orcon::Ids& ids);
static inline bool validId(const ramses::DeviceId& id) {
    return id.cls>0 && id.cls<63 && id.serial>0 && id.serial<=0x3FFFF;
}
// One RF action per transaction: a fan-mode command, a 1FC9 bind offer or the final
// 1FC9 ack. kind travels in the request so one owner/backend serves all three.
enum class FrameKind : uint8_t { Mode=0, BindOffer=1, BindAck=2 };
// 22F1 fan mode, payload 00 NN TT (NN = 0 away, 1..3, 4 auto; TT = the unit family's
// third byte: 04 MVS-15RHBP, 07 HRC400). TT must be nonzero.
bool makeModeCmd(const orcon::Ids& ids, uint8_t mode, uint8_t tail, ramses::Frame& out);
// I 1FC9 us -> fan, payload 00 (the closing frame of the observed bind handshake).
bool makeBindAck(const orcon::Ids& ids, ramses::Frame& out);
// Source ID equal to any sender heard (rlog devices, last remote/unit) = possible clone.
bool identityConflict(const orcon::Ids& ids, const rlog::Seen& seen, const rlog::Device* devs, size_t n);
// RX health as ONE atomic word from the radio-owner task: heartbeat ms, bit 0 = healthy.
// One word cannot tear into a stale/fresh mix. 0 = never published. Load before reading
// the clock, so a newer heartbeat is never "in the future".
inline uint32_t packHealth(uint32_t now, bool healthy) { return (now & ~1u) | (healthy ? 1u : 0u); }
inline bool healthReady(uint32_t word, uint32_t now) {
    return word != 0 && (word & 1u) && (uint32_t)(now - (word & ~1u)) < HEALTH_MAX_AGE_MS;
}
struct Input { uint32_t request=0; uint8_t mode=0; };
bool parseInput(const char* json, size_t len, Input& out);
// One producer / one consumer. No spinlock which could deadlock the high-priority
// owner on a preempted main loop. Copy completes before release publication.
template<class T> class Mailbox {
public:
    bool push(const T& value) {
        if (full_.load(std::memory_order_acquire)) return false;
        item_=value; full_.store(true,std::memory_order_release); return true;
    }
    bool pop(T& value) {
        if (!full_.load(std::memory_order_acquire)) return false;
        value=item_; full_.store(false,std::memory_order_release); return true;
    }
    bool full() const { return full_.load(std::memory_order_acquire); }
private:
    T item_{};
    std::atomic<bool> full_{false};
};
struct Request { Input input; orcon::Ids ids; uint32_t expires=0; uint8_t prior=0xFF;
                 uint8_t kind=(uint8_t)FrameKind::Mode; uint8_t tail=0; };
enum class Error : uint8_t { None, Expired, RadioUnavailable, Prepare, Start, Send,
    SendTimeout, Cleanup, RxRestore, OwnerTimeout };
const char* errorName(Error e);
struct Result {
    Request request;
    Error error=Error::None;          // the FIRST failure; later steps never overwrite it
    bool cleanupFailed=false;          // independent of the first diagnostic
    bool touched=false;               // radio work began (prepare called): cleanup/restore needed
    bool rxRestored=false, rfSent=false;
    uint32_t sentMs=0, restoredMs=0;
    size_t bytes=0;
};
struct Exchange { Mailbox<Request> requests; Mailbox<Result> results; };
enum class Step : uint8_t { Idle, Prepare, Simulate, Cleanup, Restore, Publish };
enum class Completion : uint8_t { Pending, Complete, Failed };
// Fault-test port. A future hardware implementation is intentionally absent.
// Contract for it: restoreRx() does the fast restore only. On failure it marks the
// radio not-ready (g_rxReady=false) so the EXISTING bounded g_rec recovery restarts
// it; the owner never runs a second recovery loop.
class Backend {
public:
    virtual ~Backend() = default;
    virtual bool prepare(const Request&,size_t& bytes)=0;
    virtual bool start(uint32_t now)=0;
    virtual Completion poll(uint32_t now)=0;
    virtual bool cleanup()=0;
    virtual bool restoreRx()=0;
    // True when a completed start/poll cycle put the frame on air (result.rfSent).
    // The dry-run backend never does; the SX1276 packet backend does.
    virtual bool onAir() const { return false; }
};
class DryRunBackend : public Backend {
public:
    void setRxHealthy(bool v) { healthy_=v; }
    bool prepare(const Request&,size_t& bytes) override;
    bool start(uint32_t now) override { started_=now; return true; }
    Completion poll(uint32_t now) override { return due(now,started_+5)?Completion::Complete:Completion::Pending; }
    bool cleanup() override { return true; }
    bool restoreRx() override { return healthy_; }   // observation only: no radio access
private:
    bool healthy_=false;
    uint32_t started_=0;
};
class Owner {
public:
    Owner(Exchange& ex,Backend& port):ex_(ex),port_(port) {}
    void tick(uint32_t now,bool rxHealthy);
    Step step() const { return step_; } // owner thread only
private:
    void fail(Error e);               // radio touched: cleanup + restore, keep first error
    void finish(Error e);             // radio untouched: publish directly
    Exchange& ex_; Backend& port_;
    Result result_;
    Step step_=Step::Idle;
    uint32_t deadline_=0;
};
// Outcomes are observations, never causal proof that THIS transmission was accepted.
enum class Phase : uint8_t { Idle, Queued, DryRunComplete, AwaitUnit, UnitReported,
    Unchanged, NotConfirmed, Ambiguous, Failed };
const char* phaseName(Phase p);
enum class Admission : uint8_t { Accepted, Duplicate, Invalid, UnknownUnit, Busy, Limited, Replay,
    Conflict, Unavailable, IdentityConflict, IdentityUnverified, UnitStateUnknown, Unchanged, RemoteActive };
struct Status {
    Phase phase=Phase::Idle; Result result; uint32_t nextRequest=1; bool busy=false, locked=false;
    uint32_t acceptedMs=0;        // when the current/open request was admitted (0 = none)
    // Main-loop observations of the selected unit, by owner-observed receive time.
    bool haveUnit=false, haveRemote=false;
    uint8_t unitMode=0xFF; uint32_t unitMs=0, remoteMs=0;
    ramses::DeviceId remoteId;
};
// Main-loop only. Cross-task traffic is exclusively through Exchange.
class Controller {
public:
    explicit Controller(Exchange& ex):ex_(ex) {}
    // identityClear: identityConflict() was false for these ids.
    // Phase-1 endpoint protocol: client-sequenced requests, 10 s rate, modes 0..4.
    Admission submit(Input,const orcon::Ids&,bool known,bool ready,bool identityClear,uint32_t now);
    // Wizard/web control: same admission, but the server assigns the sequence and the
    // spacing is RATE_CTL_MS. tail must be 04 or 07 (from a verified profile); anything else is Invalid.
    Admission submitCtl(uint8_t mode,uint8_t tail,const orcon::Ids&,bool known,bool ready,
                        bool identityClear,uint32_t now,uint32_t request=0);
    // Pairing sends: BindOffer (ids.fan = broadcast is allowed) and BindAck (ids.fan = the
    // unit that confirmed). Lighter admission: valid own id, RX ready, identity clear, one
    // open transaction; the ack is exempt from the rate floor (it answers the unit's W).
    Admission submitBind(uint8_t kind,const orcon::Ids&,bool ready,bool identityClear,uint32_t now);
    void consumeResult(uint32_t now);
    void tick(uint32_t now);
    // receivedMs: owner-observed sync time; 0 or later than now = invalid (never evidence).
    void onFrame(const ramses::Frame&,uint32_t receivedMs,uint32_t now,const orcon::Ids& current);
    const Status& status() const { return status_; }
private:
    bool interfered() const;
    void selectUnit(const ramses::DeviceId&);
    ramses::DeviceId observedUnit_;
    bool unitSelected_=false;
    void settle(Phase p) { status_.phase=p; status_.busy=false; }
    Admission begin(Request&,uint32_t now);
    Exchange& ex_; Status status_;
    uint32_t acceptedMs_=0,lastAcceptedMs_=0;
    bool haveAccepted_=false,ownerTimedOut_=false,identityCollided_=false,priorChanged_=false;
    Phase observed_=Phase::NotConfirmed;
    uint32_t observedMs_=0;
};
struct Access { bool station=false,host=false,origin=false,json=false; };
// Header checks; missing headers are passed as "". localIp "" or "0.0.0.0" never matches.
Access accessFrom(bool station,const char* host,const char* localIp,const char* origin,const char* contentType);
enum class AccessResult : uint8_t { Allowed, Unauthorized, Forbidden, Limited };
class Security {
public:
    bool begin(const char* token,const char* csrf,uint32_t now=0);
    void expire(uint32_t now); // latched; a millis wrap must not revive credentials
    AccessResult check(const char* token,const char* csrf,Access,bool mutation,uint32_t now);
    const char* csrf() const { return csrf_; } // only after successful authentication
private:
    char token_[33]{},csrf_[33]{};
    bool configured_=false;
    uint32_t window_=0,createdMs_=0;
    uint8_t failures_=0;
};
} // namespace txpoc
