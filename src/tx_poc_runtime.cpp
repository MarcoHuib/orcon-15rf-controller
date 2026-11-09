#include "tx_poc_runtime.h"
#if ORCON_TX_POC
namespace txpoc {
static Exchange exchange;
static Controller commands(exchange);
static DryRunBackend dryrun;
static Owner owner(exchange,dryrun);
Controller& controller() { return commands; }
void serviceOwner(uint32_t now,bool rxHealthy) {
    dryrun.setRxHealthy(rxHealthy);
    owner.tick(now,rxHealthy);
}
}
#endif
