// Pure transaction model: no Arduino, radio driver, SPI or NVS.
#pragma once
#include <atomic>
#include <stdint.h>
#include <stddef.h>
#include "orcon_frames.h"
namespace txpoc {
constexpr uint32_t QUEUE_MS=500, OWNER_MS=100, RECOVERY_MS=20;
constexpr uint32_t TRANSACTION_MS=1000, CONFIRM_MS=10000, RATE_MS=10000;
constexpr uint8_t RECOVERY_MAX=2;
bool due(uint32_t now, uint32_t deadline);
bool validIds(const orcon::Ids& ids);
bool makeMode04(const orcon::Ids& ids, uint8_t mode, ramses::Frame& out);
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
struct Request { Input input; orcon::Ids ids; uint32_t expires=0; };
enum class Error : uint8_t { None, Expired, RadioUnavailable, Prepare, Start, Send,
    SendTimeout, Cleanup, RxRestore, OwnerTimeout, UnitTimeout, Ambiguous };
const char* errorName(Error e);
struct Result {
    Request request;
    Error error=Error::None;
    bool rxRestored=false, rfSent=false;
    uint32_t sentMs=0, restoredMs=0;
    size_t bytes=0;
};
struct Exchange { Mailbox<Request> requests; Mailbox<Result> results; };
enum class Step : uint8_t { Idle, Prepare, Simulate, Cleanup, Restore, Recover, Publish };
enum class Completion : uint8_t { Pending, Complete, Failed };
// Fault-test port. A future hardware implementation is intentionally absent.
class Backend {
public:
    virtual ~Backend() = default;
    virtual bool prepare(const Request&,size_t& bytes)=0;
    virtual bool start(uint32_t now)=0;
    virtual Completion poll(uint32_t now)=0;
    virtual bool cleanup()=0;
    virtual bool restoreRx()=0;
    virtual bool reinitializeRx()=0;
};
class DryRunBackend : public Backend {
public:
    void setRxHealthy(bool v) { healthy_=v; }
    bool prepare(const Request&,size_t& bytes) override;
    bool start(uint32_t now) override { started_=now; return true; }
    Completion poll(uint32_t now) override { return due(now,started_+5)?Completion::Complete:Completion::Pending; }
    bool cleanup() override { return true; }
    bool restoreRx() override { return healthy_; }
    bool reinitializeRx() override { return healthy_; }
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
    void fail(Error e);
    Exchange& ex_; Backend& port_;
    Result result_;
    Step step_=Step::Idle;
    uint32_t deadline_=0,retryAt_=0;
    uint8_t retries_=0;
};
enum class Phase : uint8_t { Idle, Queued, DryRunComplete, AwaitUnit, UnitReported, Failed };
const char* phaseName(Phase p);
enum class Admission : uint8_t { Accepted, Duplicate, Invalid, UnknownUnit, Busy, Limited, Replay, Conflict, Unavailable };
struct Status { Phase phase=Phase::Idle; Result result; uint32_t nextRequest=1; bool busy=false, locked=false; };
// Main-loop only. Cross-task traffic is exclusively through Exchange.
class Controller {
public:
    explicit Controller(Exchange& ex):ex_(ex) {}
    Admission submit(Input,const orcon::Ids&,bool known,bool ready,uint32_t now);
    void consumeResult();
    void tick(uint32_t now);
    void onFrame(const ramses::Frame&,uint32_t receivedMs);
    const Status& status() const { return status_; }
private:
    Exchange& ex_; Status status_;
    uint32_t acceptedMs_=0,lastAcceptedMs_=0;
    bool haveAccepted_=false,ownerTimedOut_=false;
};
struct Access { bool station=false,host=false,origin=false,json=false; };
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
