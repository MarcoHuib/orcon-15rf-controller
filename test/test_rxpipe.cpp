// test_rxpipe.cpp — RX task pipeline under network stalls (model), queue concurrency,
// bounded recovery. Uses the firmware's rxs::Decoder / FrameQueue / Recovery.
//   g++ -std=c++17 -O2 -pthread -I src test/test_rxpipe.cpp src/rx_stream.cpp src/ramses_codec.cpp
//
// MODEL, not hardware: the UART is an 8192+128 byte buffer filled at 38400 baud
// (3.84 bytes/ms) that drops bytes when full, like the ESP32 driver; the RX task
// drains it every 5 ms; the main loop is "blocked" during the given stalls.
#include "rx_stream.h"
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
using namespace ramses;
static int fails = 0;
#define CHECK(c,m) do{ if(!(c)){printf("  [FAIL] %s\n",m);fails++;} else printf("  [ ok ] %s\n",m);}while(0)

static std::vector<uint8_t> wireOf(uint16_t k) {           // unique 31D9 per frame number
    Frame f; f.type = MsgType::I; f.addr[0] = DeviceId(0x1D, 202); f.has_addr[1] = false;
    f.addr[2] = f.addr[0]; f.cmd = 0x31D9; f.len = 4;
    f.payload[0] = 0; f.payload[1] = 0; f.payload[2] = (uint8_t)k; f.payload[3] = (uint8_t)(k >> 8);
    uint8_t l[MAX_LOGICAL], w[MAX_WIRE];
    size_t ln = buildLogical(f, l, sizeof(l)), wn = wireFromLogical(l, ln, w, sizeof(w));
    std::vector<uint8_t> v = {0x55, 0x55, 0xD5};           // misaligned preamble: no FF
    v.insert(v.end(), w + 1, w + wn);
    return v;
}
static uint16_t numOf(const Frame& f) { return (uint16_t)(f.payload[2] | (f.payload[3] << 8)); }

struct Stall { uint32_t at, dur; };
struct Result { uint32_t frames, delivered, uartLost, queueDrops, highWater; bool ordered; };

// newArch=false: one loop reads the UART and decodes (the pre-fix firmware).
// newArch=true : RX task every 5 ms -> FrameQueue<32> -> loop pops when not stalled.
static Result run(bool newArch, uint32_t simMs, uint32_t frameEveryMs, std::vector<Stall> stalls) {
    const size_t UART_CAP = 8192 + 128;
    std::vector<uint8_t> uart; uart.reserve(UART_CAP);
    rxs::Decoder dec; rxs::FrameQueue<32> q;
    uint32_t rng = 12345, nextFrame = frameEveryMs, k = 0, uartLost = 0, delivered = 0;
    int last = -1; bool ordered = true;
    std::vector<uint8_t> pending;                           // bytes of the frame on air
    double acc = 0;
    auto deliver = [&](const Frame& f) { int n = numOf(f); if (n <= last) ordered = false; last = n; delivered++; };
    auto drain = [&]() {                                    // UART -> decoder
        size_t i = 0;
        while (i < uart.size()) {
            while (i < uart.size() && dec.room()) if (dec.put(uart[i++])) dec.noteSync(-60, 0);
            rxs::Item it;
            while (dec.next(it)) { if (newArch) q.push(it); else deliver(it.f); }
        }
        uart.clear();
    };
    for (uint32_t t = 0; t < simMs; t++) {
        if (t >= nextFrame && pending.empty()) { pending = wireOf((uint16_t)k++); nextFrame += frameEveryMs; }
        for (acc += 3.84; acc >= 1; acc -= 1) {            // one byte time on the wire
            uint8_t b;
            if (!pending.empty()) { b = pending.front(); pending.erase(pending.begin()); }
            else { rng = rng * 1103515245u + 12345u; b = (uint8_t)(rng >> 16); }
            if (uart.size() < UART_CAP) uart.push_back(b); else uartLost++;
        }
        bool stalled = false;
        for (const Stall& s : stalls) if (t >= s.at && t < s.at + s.dur) stalled = true;
        if (newArch) {
            if (t % 5 == 0) drain();                        // RX task never waits for the network
            rxs::Item it;
            if (!stalled) while (q.pop(it)) deliver(it.f);
        } else if (!stalled) {
            drain();
        }
    }
    return { k, delivered, uartLost, q.dropped.load(), q.highWater.load(), ordered };
}

static void scenario(const char* name, uint32_t simMs, uint32_t every, std::vector<Stall> st) {
    Result o = run(false, simMs, every, st), n = run(true, simMs, every, st);
    printf("  %-34s old: %3u/%3u frames, %6u UART bytes lost | new: %3u/%3u, lost %u, drops %u, queue max %u\n",
           name, o.delivered, o.frames, o.uartLost, n.delivered, n.frames, n.uartLost, n.queueDrops, n.highWater);
    char m[128];
    snprintf(m, sizeof(m), "%s: RX task delivers every frame in order, no UART loss", name);
    CHECK(n.delivered + 1 >= n.frames && n.uartLost == 0 && n.queueDrops == 0 && n.ordered, m);
}

static void networkStalls() {
    printf("\n== network stalls during continuous UART input (model) ==\n");
    // one RAMSES frame per second ~= a burst of remote presses + unit replies
    scenario("DNS lookup failure 5 s", 30000, 1000, {{10000, 5000}});
    scenario("MQTT connect 2 s, backoff 5/10/20 s", 60000, 1000, {{5000, 2000}, {15000, 2000}, {35000, 2000}});
    scenario("slow HTTP client 3 s x3", 40000, 1000, {{5000, 3000}, {15000, 3000}, {25000, 3000}});
    scenario("WiFi reconnect 15 s", 40000, 1000, {{10000, 15000}});
    Result o = run(false, 30000, 1000, {{10000, 5000}});
    CHECK(o.uartLost > 0 && o.delivered + 2 < o.frames,
          "old single-loop design loses UART bytes and frames on a 5 s stall (finding M4 reproduced)");
}

static void overflow() {
    printf("\n== queue overflow + recovery ==\n");
    // 5 frames/s for a 15 s stall = ~75 frames for a 32-slot queue
    Result n = run(true, 30000, 200, {{5000, 15000}});
    printf("  frames %u delivered %u drops %u queue max %u uart lost %u\n",
           n.frames, n.delivered, n.queueDrops, n.highWater, n.uartLost);
    CHECK(n.highWater == 32 && n.queueDrops > 0, "queue fills to its bound and counts drops");
    CHECK(n.delivered + n.queueDrops + 1 >= n.frames && n.delivered + n.queueDrops <= n.frames,
          "every frame is either delivered or counted as dropped");
    CHECK(n.ordered && n.uartLost == 0, "delivery stays in order, UART never overflows");
}

static void queueThreads() {
    printf("\n== FrameQueue with real producer/consumer threads ==\n");
    rxs::FrameQueue<32> q;
    const uint32_t N = 1000000;
    std::vector<uint32_t> got; got.reserve(N);
    std::atomic<bool> done{false};
    std::thread prod([&] {
        rxs::Item it;
        for (uint32_t i = 1; i <= N; i++) { it.seq = i; it.f.len = (uint8_t)i; q.push(it); }
        done = true;
    });
    std::thread cons([&] {
        rxs::Item it; uint32_t spins = 0;
        while (!done || q.depth()) {
            if (q.pop(it)) { if (it.f.len != (uint8_t)it.seq) got.push_back(0); got.push_back(it.seq); }
            if (++spins % 4096 == 0) std::this_thread::yield();   // let the queue fill up now and then
        }
    });
    prod.join(); cons.join();
    bool inc = true; for (size_t i = 1; i < got.size(); i++) if (got[i] <= got[i - 1]) inc = false;
    char m[128];
    snprintf(m, sizeof(m), "1e6 items: %zu received + %u dropped == %u, strictly increasing, no torn items",
             got.size(), q.dropped.load(), N);
    CHECK(inc && got.size() + q.dropped.load() == N && q.highWater.load() <= 32, m);
}

static void recovery() {
    printf("\n== bounded radio recovery ==\n");
    rxs::Recovery r; uint32_t now = 0;
    const uint32_t want[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000, 30000};
    bool sched = true;
    for (uint32_t w : want) { r.record(now, false); if (r.nextMs - now != w) sched = false; now = r.nextMs; }
    CHECK(sched, "consecutive failures back off 1,2,4,8,16 then 30 s");
    r.record(now, false);
    CHECK(r.exhausted() && !r.due(now + 3600000), "after 10 attempts recovery stops (reported, not retried)");
    rxs::Recovery ok; ok.record(5000, false); ok.record(6000, true);
    CHECK(ok.streak == 0 && ok.due(6000), "a successful re-init clears the backoff");
    rxs::Recovery wrap; wrap.record(0xFFFFFC00u, false);
    CHECK(!wrap.due(0xFFFFFE00u) && wrap.due(0x00000100u), "schedule survives millis() wrap-around");
}

int main() {
    printf("RX pipeline tests\n");
    networkStalls();
    overflow();
    queueThreads();
    recovery();
    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails ? 1 : 0;
}
