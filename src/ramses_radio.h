// ramses_radio.h — thin RF transport for RAMSES-II frames.
//
// Two back-ends selected at compile time (see config.h):
//   RADIO_SX1262  : Heltec V3 built-in SX1262. GFSK + *software* Manchester.
//                   TX sends the fully pre-encoded on-air buffer; RX captures a
//                   raw window and runs the host-tested software decoder.
//   RADIO_CC1101  : external CC1101 with hardware Manchester/async framing.
//
// The protocol logic lives entirely in ramses_codec; this file is only "get
// these bytes on/off the air".
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ramses_codec.h"

namespace radio {

// Initialise the selected radio. Returns true on success.
bool begin();

// Transmit one RAMSES frame. Blocks until the packet is sent (~a few ms).
bool sendFrame(const ramses::Frame& f);

// Put the radio into continuous receive. Call once after begin().
void startReceive();

// Poll for a received frame. Returns true and fills `out` when a valid,
// checksum-clean RAMSES frame has been decoded since the last call.
bool poll(ramses::Frame& out);

// SX1276 direct-mode diagnostics. UART bytes / sync markers are NOT verified RF
// packets; these counters separate demodulator activity from codec acceptance.
struct RxDiagnostics {
    uint32_t uartBytes = 0;
    uint32_t syncHits = 0;       // exact FF 00 33 55 53 (metric of the pre-fix decoder)
    uint32_t coreSyncHits = 0;   // 00 33 55 53 (what the decoder now accepts)
    uint32_t frames = 0;         // decoded, checksum-clean frames
    uint32_t uartOverflows = 0;  // UART FIFO/ring-buffer overflows (bytes lost)
    uint8_t  opMode = 0;         // SX1276 RegOpMode; low 3 bits 5 = RX
    uint8_t  lna = 0;            // RegLna: bits 7-5 = LNA gain in use (1 = max)
    uint8_t  rxConfig = 0;       // RegRxConfig: bit3 AgcAutoOn, bits 2-0 RxTrigger
    uint8_t  rxBw = 0;           // RegRxBw raw (mant bits 4-3, exp bits 2-0)
    int16_t  rssiNow = 0;        // instantaneous RSSI (noise floor when idle)
    uint16_t queueDepth = 0, queueHighWater = 0;  // RX task -> loop frame queue (32 slots)
    uint32_t queueDrops = 0;     // frames dropped because the loop did not drain the queue
    uint16_t uartPendMax = 0;    // most bytes waiting in the 8 KB UART buffer at an RX-task pass
    uint32_t taskMaxUs = 0;      // longest RX-task pass
    uint32_t stackFree = 0;      // RX-task stack high-water mark (bytes never used)
    uint8_t  rxState = 0;        // 0 receiving, 1 recovering, 2 failed (recovery exhausted)
    int16_t  rxErr = 0;          // RadioLib status of the last radio (re)start (0 = ok)
    uint8_t  rxRecoveries = 0;   // (re)start attempts used, of rxs::Recovery::MAX
    uint32_t rxLost = 0;         // times the radio was found out of RX mode
    uint32_t lastUartMs = 0;
    uint32_t lastSyncMs = 0;
    uint32_t bufferResets = 0;
    uint16_t bufferedBytes = 0;
    uint16_t pendingBytes = 0;
};
RxDiagnostics rxDiagnostics();

// Last measured RSSI (dBm) of a received packet, for diagnostics.
float lastRssi();
// Sync number of the frame poll() returned last (SX1276; 0 elsewhere), pairs [rx] with [sync] lines.
uint32_t lastSeq();

// Human-readable back-end name ("SX1262" / "CC1101").
const char* name();

} // namespace radio
