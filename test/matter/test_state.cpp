// Host test of idf/components/orcon_matter/orcon_matter_state.h: the telemetry bring-up and its failure paths as
// earlyBegin() runs them, the start gate begin() applies, and Matter's state machine (Codex review 2, H3 and M1).
//   test/run_host_tests.sh (matter_state)
#include "orcon_matter_state.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace orcon_matter;

struct FakeOs {   // records what startTelemetry() did; each step can fail
    bool queueOk = true, taskOk = true;
    int readyAfterMs = 0;   // -1: the task never confirms
    bool queue = false, task = false;
    int slept = 0, queueDeletes = 0;
    bool createQueue() { return queue = queueOk; }
    void deleteQueue() { queue = false; ++queueDeletes; }
    bool createTask() { return task = taskOk; }
    bool taskReady() { return task && readyAfterMs >= 0 && slept >= readyAfterMs; }
    void sleepMs(int ms) { slept += ms; }
};

static Telemetry run(FakeOs& os) { return startTelemetry(os, 100); }

int main() {
    {   // the normal path: queue, task, the task's own confirmation after 3 ms
        FakeOs os; os.readyAfterMs = 3;
        assert(run(os) == Telemetry::Ready && os.queue && os.task && os.slept == 3);
    }
    {   // no queue: no task is created, nothing to clean up
        FakeOs os; os.queueOk = false;
        assert(run(os) == Telemetry::NoQueue && !os.queue && !os.task && os.queueDeletes == 0);
    }
    {   // queue made, task creation fails: the queue goes again (no mailbox without a consumer, Codex review 2 H3)
        FakeOs os; os.taskOk = false;
        assert(run(os) == Telemetry::NoTask && !os.queue && os.queueDeletes == 1);
    }
    {   // the task exists but never confirms: not ready after the bounded wait, final
        FakeOs os; os.readyAfterMs = -1;
        assert(run(os) == Telemetry::NotReady && os.slept == 101 && os.task);
    }
    {   // confirmation exactly at the limit still counts; one tick later does not
        FakeOs a; a.readyAfterMs = 100;
        assert(run(a) == Telemetry::Ready);
        FakeOs b; b.readyAfterMs = 102;
        assert(run(b) == Telemetry::NotReady);
    }

    // begin()'s gate: without running telemetry Matter never starts; the measurement build also needs its hooks.
    for (Telemetry t : {Telemetry::NotStarted, Telemetry::NoQueue, Telemetry::NoTask, Telemetry::NotReady}) {
        assert(startBlocker({t, false, true, true}) && strcmp(startBlocker({t, false, true, true}), telemetryText(t)) == 0);
        assert(startBlocker({t, true, true, true}) != nullptr);
    }
    assert(startBlocker({Telemetry::Ready, false, false, false}) == nullptr);   // clean B1: telemetry is enough
    assert(startBlocker({Telemetry::Ready, true, true, true}) == nullptr);
    assert(strcmp(startBlocker({Telemetry::Ready, true, false, true}), "alloc-hook") == 0);
    assert(strcmp(startBlocker({Telemetry::Ready, true, true, false}), "event-marks") == 0);

    {   // waiting -> starting -> ready needs BOTH the server event and the Wi-Fi hand-over, in either order
        MatterState m;
        assert(m.state() == MatterState::Waiting && strcmp(MatterState::name(m.state()), "waiting") == 0);
        assert(m.start(1000) && m.state() == MatterState::Starting && m.sinceMs() == 1000);
        assert(!m.start(1001));                                   // a second begin() is refused
        m.serverReady(1200);
        assert(m.state() == MatterState::Starting && m.serverReadySeen());
        m.handedOver(1300);
        assert(m.state() == MatterState::Ready && m.sinceMs() == 1300);
        m.fail("late", 5, 1400);                                  // ready is never turned into failed afterwards
        assert(m.state() == MatterState::Ready && strcmp(m.step(), "-") == 0);
        assert(!m.expire(999999, 30000));
    }
    {
        MatterState m;
        m.start(0);
        m.handedOver(10);
        assert(m.state() == MatterState::Starting);
        m.serverReady(20);
        assert(m.state() == MatterState::Ready);
    }
    {   // a fault is final: step, error and time stay; a late server event does not revive it
        MatterState m;
        m.start(0);
        m.fail("nvs", 0x103, 50);
        assert(m.state() == MatterState::Failed && strcmp(m.step(), "nvs") == 0 && m.err() == 0x103 && m.sinceMs() == 50);
        m.serverReady(60); m.handedOver(70);
        assert(m.state() == MatterState::Failed && !m.start(80));
        m.fail("other", 1, 90);
        assert(strcmp(m.step(), "nvs") == 0);
    }
    {   // a server that never reports ready: failed after the limit, not before
        MatterState m;
        m.start(1000);
        m.handedOver(1001);
        assert(!m.expire(31000, 30000) && m.state() == MatterState::Starting);
        assert(m.expire(31001, 30000) && m.state() == MatterState::Failed && strcmp(m.step(), "server-timeout") == 0);
        assert(!m.expire(99999, 30000));
    }
    {   // a telemetry failure in earlyBegin() fails Matter before it ever waits: begin() can no longer start it
        MatterState m;
        m.fail("telemetry-task", -1, 5);
        assert(m.state() == MatterState::Failed && !m.start(6));
    }
    {   // waiting has no time limit: a router that is away for hours is not a Matter fault
        MatterState m;
        assert(!m.expire(100000000, 30000) && m.state() == MatterState::Waiting);
    }
    printf("test_state: PASS\n");
    return 0;
}
