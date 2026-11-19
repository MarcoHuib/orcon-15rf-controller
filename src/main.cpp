// main.cpp — Orcon 15RF emulator for the Heltec WiFi LoRa 32 V2 (ESP32 + SX1276)
//
// Emulates the Orcon 15RF display controller over 868.3 MHz RAMSES-II, exposes
// control via an onboard web UI + MQTT, and shows live status on the OLED.
//
// Layers:
//   ramses_codec   protocol (host-tested)      <- the hard part, proven correct
//   ramses_radio   SX1276 direct-mode RX (+ the txpoc packet TX on RF builds)
//   orcon          15RF device emulation + state
//   net            WiFi / web / MQTT / OLED
#include <Arduino.h>
#include "config.h"
#if ORCON_TX_POC
#include "tx_poc_runtime.h"
#endif
#include "ramses_radio.h"
#include "orcon.h"
#include "net.h"
#include "orcon_log.h"
#if !ORCON_TX_POC || ORCON_WEB_CONTROL
#include "setup_web.h"
#endif
#include "ota_signed.h"

static uint32_t g_loopMaxUs = 0;

void setup() {
    Serial.begin(DEBUG_SERIAL_BAUD);
    delay(200);
    Serial.printf("\nOrcon 15RF emulator starting... fw=%s\n", FW_VERSION);
#if RX_ONLY && !ORCON_TX_POC
    // OTA contract: the signed-OTA policy (ota_policy.cpp, also on boards in use) installs an rx-only image only
    // when it contains this text. It used to come from the legacy web pages; keep it in the RX-only image.
    Serial.println("RX-ONLY TEST FIRMWARE: receive only, no 868 MHz transmission");
#endif

#if ORCON_TX_POC
    txpoc::budgetBegin();      // the RF budget history kept in RTC memory, before any TX path runs
#endif

    if (!radio::begin()) {
        Serial.println("[FATAL] radio init failed - check wiring / build flags");
    } else {
        Serial.printf("[radio] %s up on %.3f MHz\n", radio::name(), RF_FREQ_MHZ);
    }

    orcon::begin(DEV_US_CLASS, DEV_US_SERIAL, DEV_FAN_CLASS, DEV_FAN_SERIAL);
    radio::startReceive();

    net::begin();
    Serial.println("Ready.");
}

void loop() {
    const uint32_t loopStart = micros();
    // 1) drain any received RAMSES frames into the state model
#if ORCON_TX_POC
    txpoc::controller().consumeResult(millis());
#if ORCON_WEB_CONTROL
    setupweb::consumeTxResults(millis());
#endif
#endif
    ramses::Frame f;
    while (radio::poll(f)) {
        if (orcon::onFrame(f)) net::notifyStateChanged();
#if !ORCON_TX_POC || ORCON_WEB_CONTROL
        setupweb::onFrame(f, radio::lastSeq(), (int16_t)radio::lastRssi());   // same frame, same order
#endif
#if ORCON_TX_POC
        txpoc::controller().onFrame(f, radio::lastReceivedMs(), millis(), orcon::ids());
#endif
#if DEBUG_RX_HEX && defined(RADIO_SX1276)
        char s[200]; rlog::format(f, s, sizeof(s));   // full payload (31DA is 30 bytes)
        Serial.printf("[rx %lu %lu %ddBm] %s\n", (unsigned long)radio::lastSeq(), (unsigned long)millis(),
                      (int)radio::lastRssi(), s);
#endif
    }

#if ORCON_TX_POC
    txpoc::controller().tick(millis()); // expire only after timestamped RX queue processing
#endif
#if !ORCON_TX_POC || ORCON_WEB_CONTROL
    setupweb::tick(millis());           // wizard timeouts: here, never inside a GET handler
#endif
#if defined(RADIO_SX1276)
    // Serial RX counters for USB-only measurements (same fields as /api/state).
    static uint32_t lastStat = 0;
    if (millis() - lastStat >= 10000) {
        lastStat = millis();
        const radio::RxDiagnostics d = radio::rxDiagnostics();
        Serial.printf("[rxstat] t=%lu uart=%lu ovf=%lu sync5=%lu sync4=%lu frames=%lu resets=%lu pend=%u "
                      "pendmax=%u q=%u qmax=%u drop=%lu taskus=%lu stack=%lu rxst=%u err=%d rec=%u lost=%lu "
                      "op=%02X lna=%02X rxcfg=%02X rxbw=%02X rssi=%d heap=%lu minheap=%lu loopmax=%lu\n",
                      (unsigned long)millis(), (unsigned long)d.uartBytes, (unsigned long)d.uartOverflows,
                      (unsigned long)d.syncHits, (unsigned long)d.coreSyncHits, (unsigned long)d.frames,
                      (unsigned long)d.bufferResets, d.pendingBytes, d.uartPendMax, d.queueDepth,
                      d.queueHighWater, (unsigned long)d.queueDrops, (unsigned long)d.taskMaxUs,
                      (unsigned long)d.stackFree, d.rxState, d.rxErr, d.rxRecoveries, (unsigned long)d.rxLost,
                      d.opMode, d.lna, d.rxConfig, d.rxBw, d.rssiNow, (unsigned long)ESP.getFreeHeap(),
                      (unsigned long)ESP.getMinFreeHeap(), (unsigned long)g_loopMaxUs);
    }
#endif

    // 2) periodic sends (RQ 31DA/10D0, 31E0 demand, 1298 CO2, 10E0 info, keeper, selftest).
    //    TX builds only: not compiled into RX-only, and tick() itself refuses there too.
#if !RX_ONLY
    orcon::tick(millis());
#endif

    // 3) web / MQTT / OLED
    net::loop();
#if ORCON_SIGNED_OTA
    ota::tick(millis());               // confirm a freshly installed image, or roll back
#endif

    const uint32_t loopUs = micros() - loopStart;   // longest main-loop pass = worst network stall
    if (loopUs > g_loopMaxUs) g_loopMaxUs = loopUs;
}
