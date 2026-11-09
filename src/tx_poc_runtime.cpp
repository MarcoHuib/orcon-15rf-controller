#include "tx_poc_runtime.h"
#if ORCON_TX_POC
#include <Arduino.h>
namespace txpoc {
static std::atomic<uint32_t> health{0}; // radio owner -> any task (packHealth word)
static Exchange exchange;
static Controller commands(exchange);
static DryRunBackend dryrun;
static Owner owner(exchange,dryrun);
Controller& controller() { return commands; }
void serviceOwner(uint32_t now,bool rxHealthy) {
    health.store(packHealth(now,rxHealthy),std::memory_order_release);
    dryrun.setRxHealthy(rxHealthy);
    owner.tick(now,rxHealthy);
}
bool rxReady() {
    const uint32_t word=health.load(std::memory_order_acquire); // before the clock read
    return healthReady(word,millis());
}
}
#endif
