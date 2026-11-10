#include "tx_poc_runtime.h"
#if ORCON_TX_POC
#include <Arduino.h>
#include <string.h>
namespace txpoc {
static std::atomic<uint32_t> health{0}; // radio owner -> any task (packHealth word)
static Exchange exchange;
static Controller commands(exchange);
Controller& controller() { return commands; }

#if ORCON_TX_RF_ENABLE
// Real RF backend: one frame per transaction, encoded with the proven on-air
// layout (software preamble + 2 extra training bytes, as the CC1101 async build).
// prepare() encodes (pure), start()/poll() drive the SX1276 packet engine in the
// RX task, restoreRx() returns the radio to continuous direct RX.
class Sx1276Backend : public Backend {
public:
    bool prepare(const Request& r, size_t& bytes) override {
        ramses::Frame f;
        // The captured offer is truncated. The legacy 24-byte offer is not proven
        // for this device and exceeds the 64-byte FIFO. Never put it on air.
        if (r.kind != (uint8_t)FrameKind::Mode) return false;
        bool ok = makeModeCmd(r.ids, r.input.mode, r.tail, f);
        if (!ok) return false;
        size_t bits = 0;
        bytes_ = ramses::encodeFrame(f, buf_, sizeof(buf_), TX_PREAMBLE_BYTES, &bits);
        bytes = bytes_;
        return bytes_ > 0 && bits > 0;
    }
    bool start(uint32_t) override { return radio::sx1276::txBegin(buf_, bytes_); }
    Completion poll(uint32_t) override { return radio::sx1276::txDone() ? Completion::Complete : Completion::Pending; }
    bool cleanup() override { return true; }                       // restoreRx does the mode change
    bool restoreRx() override { return radio::sx1276::txRestoreRx(); }
    bool onAir() const override { return true; }                   // a completed cycle really transmitted
private:
    size_t bytes_ = 0;
    uint8_t buf_[ramses::MAX_ONAIR];
};
static Sx1276Backend backend;
#else
static DryRunBackend backend;
#endif

static Owner owner(exchange, backend);
bool ownerBusy() { return owner.step() != Step::Idle; }
void serviceOwner(uint32_t now, bool rxHealthy) {
    health.store(packHealth(now, rxHealthy), std::memory_order_release);
#if !ORCON_TX_RF_ENABLE
    backend.setRxHealthy(rxHealthy);
#endif
    owner.tick(now, rxHealthy);
}
bool rxReady() {
    const uint32_t word = health.load(std::memory_order_acquire); // before the clock read
    return healthReady(word, millis());
}
}
#endif
