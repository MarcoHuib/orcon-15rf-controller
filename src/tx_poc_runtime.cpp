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
        bool ok = false;
        if (r.kind == (uint8_t)FrameKind::Mode) ok = makeModeCmd(r.ids, r.input.mode, r.tail, f);
#if ORCON_TX_PAIR
        // Pairing frames exist on the capped RF test image only: the offer as the user's
        // remote sends it (captured), and the closing ack after the unit's W 1FC9.
        else if (r.kind == (uint8_t)FrameKind::BindOffer) ok = makeBindOffer(r.ids, f);
        else if (r.kind == (uint8_t)FrameKind::BindAck) ok = makeBindAck(r.ids, f);
#endif
        if (!ok) return false;
        size_t bits = 0;
        bytes_ = ramses::encodeFrame(f, buf_, sizeof(buf_), TX_PREAMBLE_BYTES, &bits);
        bytes = bytes_;
        return bytes_ > 0 && bits > 0 && bytes_ <= radio::sx1276::TX_MAX_BYTES;
    }
    bool start(uint32_t) override {
#if ORCON_TX_MAX_PER_BOOT
        // RF test image: every attempt is counted before the radio is touched, and the
        // per-boot cap fails closed in the RX task, whatever the web layer decided.
        if (starts.load(std::memory_order_acquire) >= ORCON_TX_MAX_PER_BOOT) return false;
#endif
        starts.fetch_add(1, std::memory_order_acq_rel);
        return radio::sx1276::txBegin(buf_, bytes_);
    }
    static std::atomic<uint32_t> starts;
    Completion poll(uint32_t) override {
        if (radio::sx1276::txDone()) return Completion::Complete;
        return radio::sx1276::txFailed() ? Completion::Failed : Completion::Pending;
    }
    bool cleanup() override { return true; }                       // restoreRx does the mode change
    bool restoreRx() override { return radio::sx1276::txRestoreRx(); }
    bool onAir() const override { return true; }                   // a completed cycle really transmitted
private:
    size_t bytes_ = 0;
    uint8_t buf_[ramses::MAX_ONAIR];
};
std::atomic<uint32_t> Sx1276Backend::starts{0};
static Sx1276Backend backend;
uint32_t rfTxStarts() { return Sx1276Backend::starts.load(std::memory_order_acquire); }
#else
static DryRunBackend backend;
#endif
#if !ORCON_TX_RF_ENABLE
uint32_t rfTxStarts() { return 0; }   // dry-run: nothing ever reaches the air
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
