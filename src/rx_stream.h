// rx_stream.h — receive pipeline pieces for the SX1276 RX task. Pure C++, host-tested.
//   Decoder    : UART bytes -> RAMSES frames, each tagged with the sync it began at
//   FrameQueue : bounded single-producer/single-consumer queue between the RX task
//                and the main loop; when full the NEWEST frame is dropped and counted
//   Recovery   : bounded re-initialisation schedule for the radio
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <atomic>
#include "ramses_codec.h"

namespace rxs {

struct Item {
    ramses::Frame f;
    uint32_t seq   = 0;     // number of the sync this frame started at (0 = unknown)
    uint32_t ms    = 0;     // time that sync was seen
    int16_t  rssi  = 0;     // RSSI sampled when that sync arrived
};

class Decoder {
public:
    // Append one UART byte. Returns true when it completes a core sync (00 33 55 53);
    // the caller then reports the RSSI/time of that sync with noteSync().
    bool put(uint8_t b);
    void noteSync(int16_t rssi, uint32_t ms);
    // Next complete, checksum-clean frame from the buffered bytes, if any.
    bool next(Item& out);
    // Drop all buffered bytes and partial sync state. Used after an SX1276 TX burst:
    // bytes received while the radio transmitted are echo/noise and must never be
    // decoded. Sync counters keep counting (sequence numbers stay monotonic).
    void reset() { base_ += len_; len_ = 0; progress_ = 0; prev_ = 0; pre_ = 0; }
    size_t  room() const { return sizeof(buf_) - len_; }
    size_t  buffered() const { return len_; }
    uint8_t lastPre() const { return ring_[(syncs - 1) % RING].pre; }   // byte before the last 00
    // Written by the producer only; 32-bit reads from another task are atomic on ESP32.
    uint32_t bytes = 0, syncs = 0, syncsFF = 0, frames = 0, resets = 0;
private:
    static const uint32_t RING = 8;
    struct Sync { uint64_t at = 0; uint32_t ms = 0; int16_t rssi = 0; uint8_t pre = 0; };
    uint8_t  buf_[320];
    size_t   len_ = 0;
    uint64_t base_ = 0;          // stream offset of buf_[0]
    uint8_t  progress_ = 0, prev_ = 0, pre_ = 0;
    Sync     ring_[RING];        // the last RING syncs, to tag frames
};

template <uint32_t N, class T = Item>
class FrameQueue {
    static_assert(N && (N & (N - 1)) == 0, "N must be a power of two");
public:
    bool push(const T& it) {                          // producer only
        uint32_t h = head_.load(std::memory_order_relaxed);
        uint32_t t = tail_.load(std::memory_order_acquire);
        if (h - t >= N) { dropped.fetch_add(1, std::memory_order_relaxed); return false; }
        slot_[h % N] = it;
        head_.store(h + 1, std::memory_order_release);
        if (h + 1 - t > highWater.load(std::memory_order_relaxed))
            highWater.store(h + 1 - t, std::memory_order_relaxed);
        return true;
    }
    bool pop(T& it) {                                 // consumer only
        uint32_t t = tail_.load(std::memory_order_relaxed);
        uint32_t h = head_.load(std::memory_order_acquire);
        if (t == h) return false;
        it = slot_[t % N];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
    uint32_t depth() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }
    std::atomic<uint32_t> dropped{0}, highWater{0};
private:
    T slot_[N];
    std::atomic<uint32_t> head_{0}, tail_{0};
};

// At most MAX re-initialisations per boot; consecutive failures wait 1,2,4,8,16,30 s.
// Exhausted = RX stays down and is reported; the radio is not hammered forever.
struct Recovery {
    static const uint8_t MAX = 10;
    uint8_t  attempts = 0, streak = 0;
    uint32_t nextMs = 0;
    bool exhausted() const { return attempts >= MAX; }
    bool due(uint32_t now) const { return !exhausted() && (int32_t)(now - nextMs) >= 0; }
    void record(uint32_t now, bool ok) {
        ++attempts;
        streak = ok ? 0 : (uint8_t)(streak + 1);
        uint32_t wait = streak ? (1000u << (streak > 5 ? 5 : streak - 1)) : 0;
        nextMs = now + (wait > 30000 ? 30000 : wait);
    }
};

} // namespace rxs
