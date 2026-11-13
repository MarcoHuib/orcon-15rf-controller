#pragma once
#include "config.h"
#if ORCON_TX_POC
#include "tx_poc.h"
namespace txpoc {
Controller& controller(); // Arduino loop only
void serviceOwner(uint32_t now,bool rxHealthy); // existing radio owner only
bool rxReady(); // any task: fresh, healthy heartbeat published by the radio owner
bool ownerBusy(); // owner thread only (the RX task): a transaction is mid-flight
uint32_t rfTxStarts(); // any task: RF packets started this boot (0 without the RF backend)
}
#if ORCON_TX_RF_ENABLE
// ramses_radio.cpp (SX1276 back-end, RX-task context only). The txpoc owner backend
// is the only caller; these are the only radio functions that ever transmit.
namespace radio { namespace sx1276 {
bool txBegin(const uint8_t* onair, size_t n);
bool txDone();
bool txRestoreRx();
uint32_t restoreDiag(); // any task: why the last fast RX restore failed (0 = it succeeded)
} }
#endif
#endif
