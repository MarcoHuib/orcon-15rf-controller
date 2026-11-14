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
// Daily-use RF budget (tx_budget.h: 10 per minute, 60 per hour, measured airtime per hour),
// enforced by the RF backend in the RX task before the radio is touched. The history lives in
// RTC memory, so software restarts, crashes and OTA reboots do not reset it. Any task.
// Without the RF backend nothing reaches the air: an empty budget and zero waits.
struct BudgetView { uint8_t minute = 0, hour = 0; uint32_t airUsHour = 0; bool kept = false; };
void budgetBegin();                                   // setup(): restore the RTC-kept history
uint32_t budgetWaitMs(uint8_t packets, size_t onairBytes);   // 0: these packets may start now
BudgetView budget();
uint32_t lastAirUs();                                 // measured airtime of the last packet
uint32_t lastTxStartMs();                             // owner pass of the last radio start (0: none)
}
#if ORCON_TX_RF_ENABLE
// ramses_radio.cpp (SX1276 back-end, RX-task context only). The txpoc owner backend
// is the only caller; these are the only radio functions that ever transmit.
namespace radio { namespace sx1276 {
static const size_t TX_MAX_BYTES = 128;   // on-air bytes per packet (offer ~106)
bool txBegin(const uint8_t* onair, size_t n);   // sends the whole packet, then restores RX
bool txDone();
bool txFailed();
bool txRestoreRx();
uint32_t txLastAirUs();   // RX task: TX start to PacketSent (or abort) of the last packet; 0 = never on air
uint32_t restoreDiag(); // any task: why the last fast RX restore failed (0 = it succeeded)
} }
#endif
#endif
