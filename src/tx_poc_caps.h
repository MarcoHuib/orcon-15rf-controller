// Phase 1/2: independent, compile-time PoC capabilities; legacy RX_ONLY stays locked.
#pragma once
#ifndef ORCON_TX_POC
#define ORCON_TX_POC 0
#endif
#ifndef ORCON_TX_RF_ENABLE
#define ORCON_TX_RF_ENABLE 0
#endif
#ifndef ORCON_WEB_CONTROL
#define ORCON_WEB_CONTROL 0
#endif
#ifndef ORCON_TX_MAX_PER_BOOT
#define ORCON_TX_MAX_PER_BOOT 0   // 0: no per-boot cap (the 2 s admission floor still applies)
#endif
#ifndef ORCON_TX_PAIR
#define ORCON_TX_PAIR 0           // 1: RF pairing (1FC9 offer + closing ack) on the capped RF test image
#endif
#if (ORCON_TX_PAIR != 0 && ORCON_TX_PAIR != 1) || (ORCON_TX_PAIR && (!ORCON_TX_RF_ENABLE || !ORCON_WEB_CONTROL || !ORCON_TX_MAX_PER_BOOT))
#error "ORCON_TX_PAIR requires the capped RF web-control test image (RF, web control, ORCON_TX_MAX_PER_BOOT)"
#endif
#if ORCON_TX_MAX_PER_BOOT < 0 || ORCON_TX_MAX_PER_BOOT > 100 || (ORCON_TX_MAX_PER_BOOT && !ORCON_TX_RF_ENABLE)
#error "ORCON_TX_MAX_PER_BOOT is 0, or 1..100 on the RF build only"
#endif
#if (ORCON_TX_POC != 0 && ORCON_TX_POC != 1) || (ORCON_TX_RF_ENABLE != 0 && ORCON_TX_RF_ENABLE != 1) || (ORCON_WEB_CONTROL != 0 && ORCON_WEB_CONTROL != 1)
#error "PoC capabilities must be 0 or 1"
#endif
#if ORCON_TX_POC && (!defined(BOARD_HELTEC_V2) || !defined(RADIO_SX1276) || RX_ONLY != 1 || defined(RADIO_SX1262) || defined(RADIO_CC1101) || defined(RADIO_CC1101_ASYNC) || defined(USE_CC1101_TX) || defined(PINTEST) || defined(BOARD_4848S040))
#error "PoC requires the exclusive Heltec V2 SX1276 backend and RX_ONLY=1"
#endif
#if ORCON_WEB_CONTROL && !ORCON_TX_POC
#error "Web control requires the TX-PoC control layer (ORCON_TX_POC=1)"
#endif
#if ORCON_TX_RF_ENABLE && !ORCON_TX_POC
#error "RF TX requires the TX-PoC control layer (ORCON_TX_POC=1)"
#endif
// The legacy orcon command/pairing layer and radio::sendFrame stay locked on every
// V2 build (incl. RF builds): the ONLY transmission path is the txpoc owner backend.
