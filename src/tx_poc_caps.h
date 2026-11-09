// Phase 1: independent, compile-time PoC capabilities; legacy RX_ONLY stays locked.
#pragma once
#ifndef ORCON_TX_POC
#define ORCON_TX_POC 0
#endif
#ifndef ORCON_TX_RF_ENABLE
#define ORCON_TX_RF_ENABLE 0
#endif
#if (ORCON_TX_POC != 0 && ORCON_TX_POC != 1) || (ORCON_TX_RF_ENABLE != 0 && ORCON_TX_RF_ENABLE != 1)
#error "PoC capabilities must be 0 or 1"
#endif
#if ORCON_TX_POC && (!defined(BOARD_HELTEC_V2) || !defined(RADIO_SX1276) || RX_ONLY != 1 || defined(RADIO_SX1262) || defined(RADIO_CC1101) || defined(RADIO_CC1101_ASYNC) || defined(USE_CC1101_TX) || defined(PINTEST) || defined(BOARD_4848S040))
#error "PoC requires the exclusive Heltec V2 SX1276 backend and RX_ONLY=1"
#endif
#if ORCON_TX_RF_ENABLE
#error "RF TX is not released in phase 1; use heltec_v2_sx1276_tx_poc_dryrun"
#endif
