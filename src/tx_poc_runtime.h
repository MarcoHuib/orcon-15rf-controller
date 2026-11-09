#pragma once
#include "config.h"
#if ORCON_TX_POC
#include "tx_poc.h"
namespace txpoc {
Controller& controller(); // Arduino loop only
void serviceOwner(uint32_t now,bool rxHealthy); // existing radio owner only
}
#endif
