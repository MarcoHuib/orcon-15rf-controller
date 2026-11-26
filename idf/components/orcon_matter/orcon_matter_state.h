#pragma once
// Pure logic of the Matter glue (no ESP-IDF; host test: test/matter/test_state.cpp), so its failure paths are
// tested on the host exactly as the firmware runs them (Codex review 2, H3 and M1):
//   startTelemetry()  earlyBegin(): mailbox queue, telemetry task, and the task's own "I run" confirmation
//   startBlocker()    begin(): Matter starts only when everything this build measures with is up
//   MatterState       waiting -> starting -> ready | failed, the [mem] line's "mtr"
#include <atomic>
#include <stdint.h>

namespace orcon_matter {

// ---- telemetry bring-up ----------------------------------------------------------------------------------------
enum class Telemetry : uint8_t { NotStarted, NoQueue, NoTask, NotReady, Ready };

inline const char* telemetryText(Telemetry t) {
    switch (t) {
        case Telemetry::NotStarted: return "not-started";
        case Telemetry::NoQueue: return "no-queue";
        case Telemetry::NoTask: return "telemetry-task";
        case Telemetry::NotReady: return "not-ready";
        case Telemetry::Ready: return "ready";
    }
    return "?";
}

// Os: createQueue(), deleteQueue(), createTask() -> bool, taskReady() -> bool (set by the task itself after its first
// sample), sleepMs(1). A failed step leaves no half-made state: no task means no queue (attributeCb then refuses).
template <class Os>
Telemetry startTelemetry(Os& os, int readyWaitMs) {
    if (!os.createQueue()) return Telemetry::NoQueue;
    if (!os.createTask()) {
        os.deleteQueue();
        return Telemetry::NoTask;
    }
    for (int ms = 0; ms <= readyWaitMs; ++ms) {
        if (os.taskReady()) return Telemetry::Ready;
        os.sleepMs(1);
    }
    return Telemetry::NotReady;   // the task exists but never ran: final for this boot (it is not deleted mid-sample)
}

// Why Matter stays off, or nullptr. The measurement build (heap monitor) starts Matter only when the measurement is
// complete: running telemetry, the failed-allocation hook, and the Wi-Fi/IP event marks (else the wifi phase is blind).
struct StartCheck {
    Telemetry telemetry;
    bool monitorBuild, allocHookOk, eventsOk;
};
inline const char* startBlocker(const StartCheck& c) {
    if (c.telemetry != Telemetry::Ready) return telemetryText(c.telemetry);
    if (!c.monitorBuild) return nullptr;
    if (!c.allocHookOk) return "alloc-hook";
    if (!c.eventsOk) return "event-marks";
    return nullptr;
}

// ---- Matter's state --------------------------------------------------------------------------------------------
//   waiting   no connected home network (setup AP, AP fallback, router not up yet): not a fault
//   starting  begin() runs, or esp_matter::start() returned and CHIP's server is not ready yet (bounded)
//   ready     CHIP posted kServerReady (DNS-SD up) AND Wi-Fi station control is Orcon's
//   failed    a definitive local fault: step, esp_err, time. Final for this boot: CHIP cannot be started twice
class MatterState {
public:
    enum S : uint32_t { Waiting, Starting, Ready, Failed };

    S state() const { return S(s_.load()); }
    uint32_t sinceMs() const { return since_.load(); }
    int32_t err() const { return err_.load(); }
    const char* step() const { return step_.load(); }
    bool serverReadySeen() const { return server_.load(); }

    // false: not waiting (a second begin()). The time goes out BEFORE the state: expire() on the other core must never
    // see Starting with the old time 0 (race 1). Only begin() leaves Waiting this way.
    bool start(uint32_t now) {
        if (state() != Waiting) return false;
        since_.store(now);
        return move(Waiting, Starting, now);
    }
    void fail(const char* step, int32_t err, uint32_t now) {
        uint32_t s = s_.load();
        while (s == Starting || s == Waiting) {   // never out of ready (a later fault is reported elsewhere) or failed
            step_.store(step);
            err_.store(err);
            if (s_.compare_exchange_weak(s, Failed)) { since_.store(now); return; }
        }
    }
    void serverReady(uint32_t now) { server_.store(true); promote(now); }   // CHIP task
    void handedOver(uint32_t now) { handed_.store(true); promote(now); }    // begin()
    // telemetry task: a server that never reports ready (e.g. DNS-SD did not come up) is a fault, not a wait
    bool expire(uint32_t now, uint32_t limitMs) {
        if (state() != Starting || now - sinceMs() <= limitMs) return false;
        fail("server-timeout", 0x107 /* ESP_ERR_TIMEOUT */, now);
        return state() == Failed;
    }
    static const char* name(S s) {
        return s == Waiting ? "waiting" : s == Starting ? "starting" : s == Ready ? "ready" : "failed";
    }

private:
    bool move(S from, S to, uint32_t now) {
        uint32_t f = from;
        if (!s_.compare_exchange_strong(f, to)) return false;
        since_.store(now);
        return true;
    }
    void promote(uint32_t now) {
        if (server_.load() && handed_.load()) move(Starting, Ready, now);
    }
    std::atomic<uint32_t> s_{Waiting}, since_{0};
    std::atomic<int32_t> err_{0};
    std::atomic<const char*> step_{"-"};
    std::atomic<bool> server_{false}, handed_{false};
};

}  // namespace orcon_matter
