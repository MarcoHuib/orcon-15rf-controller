// ramses_radio.cpp — see ramses_radio.h
#include "ramses_radio.h"
#include "config.h"
#if ORCON_TX_POC
#include "tx_poc_runtime.h"
#endif
#include <Arduino.h>
#include <RadioLib.h>
#if defined(USE_CC1101_TX) || defined(RADIO_CC1101_ASYNC)
#include "cc1101_tx.h"
#endif
#if defined(RADIO_SX1276)
#include <HardwareSerial.h>
#include <esp_task_wdt.h>
#include "rx_stream.h"
#endif

// SX1276 packet-mode TX (defined below the RX back-end block): the RX task asks this
// while draining the UART so our own transmission is never fed into the decoder.
#if defined(RADIO_SX1276) && ORCON_TX_POC && ORCON_TX_RF_ENABLE
namespace radio { namespace sx1276 { bool txActive(); } }
#endif

namespace radio {

static volatile bool   g_rxFlag = false;
static float           g_rssi   = -127.0f;
static uint8_t         g_onair[ramses::MAX_ONAIR];

#if defined(ESP32)
IRAM_ATTR
#endif
static void onDio() { g_rxFlag = true; }

// ===========================================================================
//  SX1262 back-end (built-in Heltec radio). RX always; TX via SX1262 unless
//  USE_CC1101_TX is set, in which case an external CC1101 does the transmit.
// ===========================================================================
#if defined(RADIO_SX1262)

static SX1262 g_radio = new Module(PIN_SX_NSS, PIN_SX_DIO1, PIN_SX_RST, PIN_SX_BUSY);

static const size_t  RX_WINDOW = 96;
static uint8_t       g_syncWord[2] = { 0x55, 0x53 };

#if defined(USE_CC1101_TX)
const char* name() { return "SX1262rx+CC1101tx"; }
#else
const char* name() { return "SX1262"; }
#endif

static bool configure() {
    int st = g_radio.beginFSK(RF_FREQ_MHZ, RF_BITRATE_KBPS, RF_DEVIATION_KHZ,
                              RF_RXBW_KHZ, 10 /*dBm*/, 16 /*preamble bits*/);
    if (st != RADIOLIB_ERR_NONE) { Serial.printf("[radio] beginFSK=%d\n", st); return false; }
    // Heltec V3/V4: the antenna is connected to the SX1262 through an RF switch
    // driven by DIO2. Without this, the receiver is electrically disconnected
    // from the antenna -> dead-flat ~-112 dBm RSSI no matter the antenna. REQUIRED.
    g_radio.setDio2AsRfSwitch(true);
    g_radio.setTCXO(1.6);                       // Heltec uses a DIO3-powered TCXO
    g_radio.setSyncWord(g_syncWord, sizeof(g_syncWord));
    g_radio.setCRC(0);
    g_radio.setDataShaping(RADIOLIB_SHAPING_0_5);
    g_radio.setEncoding(RADIOLIB_ENCODING_NRZ);
    g_radio.fixedPacketLengthMode(RX_WINDOW);
    g_radio.setDio1Action(onDio);
    return true;
}

void startReceive() {
    g_rxFlag = false;
    g_radio.startReceive();
}

bool begin() {
    SPI.begin(PIN_SX_SCK, PIN_SX_MISO, PIN_SX_MOSI, PIN_SX_NSS);
    bool ok = configure();
#if defined(USE_CC1101_TX)
    cc1101tx::begin();          // CC1101 handles transmit; SX1262 keeps RX
#endif
    return ok;
}

bool sendFrame(const ramses::Frame& f) {
#if defined(USE_CC1101_TX)
    bool ok = cc1101tx::send(f);    // transmit via CC1101 async UART
    startReceive();                 // keep the SX1262 listening
    return ok;
#else
    size_t bits = 0;
    size_t n = ramses::encodeFrame(f, g_onair, sizeof(g_onair), RF_PREAMBLE_BYTES, &bits);
    if (!n) return false;
    static uint8_t txSync[2] = { 0x55, 0x55 };
    g_radio.setSyncWord(txSync, sizeof(txSync));
    g_radio.fixedPacketLengthMode((uint8_t)n);
    int st = g_radio.transmit(g_onair, n);
    g_radio.setSyncWord(g_syncWord, sizeof(g_syncWord));
    g_radio.fixedPacketLengthMode(RX_WINDOW);
    startReceive();
    if (st != RADIOLIB_ERR_NONE) { Serial.printf("[radio] tx=%d\n", st); return false; }
    return true;
#endif
}

bool poll(ramses::Frame& out) {
    if (!g_rxFlag) return false;
    g_rxFlag = false;
    uint8_t buf[RX_WINDOW];
    int st = g_radio.readData(buf, RX_WINDOW);
    g_rssi = g_radio.getRSSI();
    startReceive();
    if (st != RADIOLIB_ERR_NONE && st != RADIOLIB_ERR_CRC_MISMATCH) return false;
    bool ok = ramses::decodeFrame(buf, RX_WINDOW, RX_WINDOW * 8, out);
#if DEBUG_RX_HEX
    if (ok) {
        Serial.printf("[rx %.0fdBm] cmd=%04X len=%u :", g_rssi, out.cmd, out.len);
        for (uint8_t i = 0; i < out.len; i++) Serial.printf("%02X", out.payload[i]);
        Serial.println();
    }
#endif
    return ok;
}

#endif // RADIO_SX1262

// ===========================================================================
//  CC1101 stand-alone packet back-end (legacy fallback, -D RADIO_CC1101)
// ===========================================================================
#if defined(RADIO_CC1101)

static CC1101 g_radio = new Module(PIN_CC_NSS, PIN_CC_GDO0, RADIOLIB_NC, PIN_CC_GDO2);
static const size_t RX_WINDOW = 96;

const char* name() { return "CC1101"; }

static bool configure() {
    int st = g_radio.begin(RF_FREQ_MHZ, RF_BITRATE_KBPS, RF_DEVIATION_KHZ,
                           RF_RXBW_KHZ, 10 /*dBm*/, RF_PREAMBLE_BYTES);
    if (st != RADIOLIB_ERR_NONE) { Serial.printf("[radio] cc begin=%d\n", st); return false; }
    g_radio.setSyncWord(0x55, 0x53);
    g_radio.setCrcFiltering(false);
    g_radio.setEncoding(RADIOLIB_ENCODING_NRZ);
    g_radio.fixedPacketLengthMode(RX_WINDOW);
    g_radio.setGdo0Action(onDio, RISING);
    return true;
}

bool begin() {
    SPI.begin(PIN_SX_SCK, PIN_SX_MISO, PIN_SX_MOSI, PIN_CC_NSS);
    return configure();
}

void startReceive() { g_rxFlag = false; g_radio.startReceive(); }

bool sendFrame(const ramses::Frame& f) {
    size_t bits = 0;
    size_t n = ramses::encodeFrame(f, g_onair, sizeof(g_onair), RF_PREAMBLE_BYTES, &bits);
    if (!n) return false;
    int st = g_radio.transmit(g_onair, n);
    startReceive();
    return st == RADIOLIB_ERR_NONE;
}

bool poll(ramses::Frame& out) {
    if (!g_rxFlag) return false;
    g_rxFlag = false;
    uint8_t buf[RX_WINDOW];
    int st = g_radio.readData(buf, RX_WINDOW);
    g_rssi = g_radio.getRSSI();
    startReceive();
    if (st != RADIOLIB_ERR_NONE && st != RADIOLIB_ERR_CRC_MISMATCH) return false;
    return ramses::decodeFrame(buf, RX_WINDOW, RX_WINDOW * 8, out);
}

#endif // RADIO_CC1101

// ===========================================================================
//  CC1101 async-serial back-end — RX *and* TX on ONE CC1101 (the evofw3 way).
//  No SX1262. Receive = the mirror of transmit: CC1101 demodulates onto GDO2,
//  the UART re-frames the wire bytes, software decodes them.
// ===========================================================================
#if defined(RADIO_CC1101_ASYNC)
const char* name() { return "CC1101 async RX+TX"; }
bool begin() { return cc1101tx::begin(); }
void startReceive() { cc1101tx::startRx(); }
bool sendFrame(const ramses::Frame& f) {
    bool ok = cc1101tx::send(f);
    cc1101tx::startRx();                 // straight back to listening
    return ok;
}
bool poll(ramses::Frame& out) {
    bool ok = cc1101tx::poll(out);
    g_rssi = (float)cc1101tx::rssiDbm();
    return ok;
}
#endif // RADIO_CC1101_ASYNC

// ===========================================================================
//  SX1276 back-end (Heltec WiFi LoRa 32 V2) — single-board RX+TX, NO CC1101.
//  The SX127x family keeps the "continuous/direct" FSK mode the SX1262 dropped,
//  so we can mirror the CC1101 trick on the on-board radio:
//   RX: continuous FSK — the demodulated NRZ stream appears on DIO2; a hardware
//       UART at 38400 8N1 (== the RAMSES byte framing) re-frames it and
//       extractWireFrame() decodes it (the exact path proven on CC1101 GDO2).
//   TX: FSK packet mode — the full on-air frame is stuffed as raw payload.
// ===========================================================================
#if defined(RADIO_SX1276)
#if !RX_ONLY
#error "SX1276 backend is receive-only: it must be built with RX_ONLY=1"
#endif
// One FreeRTOS task owns the SX1276 after begin(): it alone touches SPI and the RX
// UART. The main loop (web/MQTT/WiFi, can block for seconds) only pops decoded
// frames from a bounded queue, so network stalls cannot overflow the UART buffer.
static Module g_mod(PIN_SX_NSS, PIN_SX_DIO0, PIN_SX_RST, PIN_SX_DIO1);  // own handle: read-only register diagnostics
static SX1276 g_radio(&g_mod);
static HardwareSerial rxUart(1);     // reads the demodulated data off DIO2
static rxs::Decoder        g_dec;    // RX task only (its counters are read by diagnostics)
static rxs::FrameQueue<32> g_q;      // RX task -> loop; full: newest frame dropped + counted
static rxs::Recovery       g_rec;    // bounded (re)starts
static volatile bool     g_rxReady = false;     // configured and in RX
static volatile bool     g_taskUp = false;
static volatile int16_t  g_rxErr = 0;           // last RadioLib status of a (re)start
static volatile uint32_t g_rxLost = 0;          // times RegOpMode showed the radio out of RX
static volatile uint32_t g_uartOverflows = 0;   // HW FIFO / ring buffer full: bytes LOST
static volatile uint32_t g_lastUartMs = 0, g_lastSyncMs = 0;
static volatile uint32_t g_pendMax = 0, g_taskMaxUs = 0, g_stackFree = 0;
static volatile uint8_t  g_op = 0, g_lna = 0, g_rxcfg = 0, g_rxbw = 0;
static volatile int16_t  g_rssiNow = 0;
#if ORCON_TX_POC
static uint32_t g_lastReceivedMs = 0;
#endif
static uint32_t g_lastSeq = 0;                  // loop: sync number of the frame poll() returned
static rxs::FrameQueue<16, SyncInfo> g_sq;      // RX task -> loop: syncs for the live RF console

#if ORCON_TX_RF_ENABLE && ORCON_TX_MAX_PER_BOOT
#define ORCON_STR2(x) #x
#define ORCON_STR(x) ORCON_STR2(x)
const char* name() { return "SX1276 direct RX + txpoc packet TX (RF TEST: max " ORCON_STR(ORCON_TX_MAX_PER_BOOT) " packets per boot)"; }
#elif ORCON_TX_RF_ENABLE
const char* name() { return "SX1276 direct RX + txpoc packet TX"; }
#else
const char* name() { return "SX1276 direct RX (TX disabled)"; }
#endif

static int16_t configure() {
    int16_t st = g_radio.beginFSK(RF_FREQ_MHZ, RF_BITRATE_KBPS, RF_DEVIATION_KHZ,
                                  RF_RXBW_KHZ, 17 /*dBm, unused: no TX*/, 16 /*preamble bits*/);
    if (st != RADIOLIB_ERR_NONE) return st;
    g_radio.setEncoding(RADIOLIB_ENCODING_NRZ);
    g_radio.setDataShaping(RADIOLIB_SHAPING_NONE);
    g_radio.setCRC(false);
    return g_radio.receiveDirect();             // continuous FSK; demod streams on DIO2
}

// (Re)start RX. A failed RadioLib begin ends SPI (Module::term), so begin it again.
static bool startRx() {
    SPI.begin(PIN_SX_SCK, PIN_SX_MISO, PIN_SX_MOSI, PIN_SX_NSS);   // no-op while SPI is up
    int16_t st = configure();
    g_rxErr = st;
    while (rxUart.available()) rxUart.read();   // bytes from before the (re)start
    if (st != RADIOLIB_ERR_NONE) Serial.printf("[radio] SX1276 start failed: %d (attempt %u/%u)\n",
                                               st, g_rec.attempts + 1, rxs::Recovery::MAX);
    return st == RADIOLIB_ERR_NONE;
}

static void snapshot() {                        // diagnostics registers; RX task only (owns SPI)
    g_op    = g_mod.SPIreadRegister(RADIOLIB_SX127X_REG_OP_MODE);
    g_lna   = g_mod.SPIreadRegister(RADIOLIB_SX127X_REG_LNA);
    g_rxcfg = g_mod.SPIreadRegister(RADIOLIB_SX127X_REG_RX_CONFIG);
    g_rxbw  = g_mod.SPIreadRegister(RADIOLIB_SX127X_REG_RX_BW);
    g_rssiNow = (int16_t)g_radio.getRSSI(false, true);
    g_stackFree = uxTaskGetStackHighWaterMark(nullptr);
}

static void rxTask(void*) {
    esp_task_wdt_add(nullptr);                  // a hung RX task is reported by the task watchdog
    uint32_t lastCheck = 0;
    for (;;) {
        esp_task_wdt_reset();
        const uint32_t t0 = micros(), now = millis();
        if (!g_rxReady && g_rec.due(now)) {
            bool ok = startRx();
            g_rec.record(now, ok);
            g_rxReady = ok;
        }
        if (g_rxReady
#if ORCON_TX_RF_ENABLE
            && !radio::sx1276::txActive()   // while we transmit, nothing here is decoded
#endif
        ) {
            uint32_t pend = (uint32_t)rxUart.available();
            if (pend > g_pendMax) g_pendMax = pend;
            if (pend) g_lastUartMs = now;
            while (rxUart.available() && g_dec.room()) {
                if (g_dec.put((uint8_t)rxUart.read())) {
                    // skipReceive=true is required: with default args RadioLib does
                    // startReceive()+standby() in FSK = RX dead.
                    int16_t rssi = (int16_t)g_radio.getRSSI(false, true);
                    g_dec.noteSync(rssi, now);
                    g_lastSyncMs = now;
                    SyncInfo si; si.seq = g_dec.syncs; si.ms = now; si.rssi = rssi; si.pre = g_dec.lastPre();
                    g_sq.push(si);                  // never blocks: full = dropped + counted
#if DEBUG_RX_HEX
                    // one line per sync: no [rx] line with the same number = garbled frame
                    Serial.printf("[sync %lu %lu %ddBm pre=%02X]\n", (unsigned long)g_dec.syncs,
                                  (unsigned long)now, rssi, g_dec.lastPre());
#endif
                }
            }
            rxs::Item it;
            while (g_dec.next(it)) g_q.push(it);
        }
        if (now - lastCheck >= 1000) {
            lastCheck = now;
            snapshot();
            if (g_rxReady && (g_op & 0x07) != 0x05
#if ORCON_TX_RF_ENABLE
                && !txpoc::ownerBusy()     // mid-TX the radio is intentionally out of RX
#endif
            ) {   // radio reset / brown-out / left RX
                g_rxReady = false; ++g_rxLost;
                Serial.printf("[radio] SX1276 not in RX (op=%02X): restarting\n", g_op);
            }
        }
#if ORCON_TX_POC
        // Dry-run model only. No radio, SPI, DIO, UART or decoder state changes.
        // With RF enabled, "healthy" means the RX pipeline is up: the radio may be
        // out of RX on purpose while the owner transmits.
        txpoc::serviceOwner(now, g_rxReady && ((g_op & 0x07) == 0x05
#if ORCON_TX_RF_ENABLE
            || txpoc::ownerBusy()
#endif
        ));
#endif
        uint32_t us = micros() - t0;
        if (us > g_taskMaxUs) g_taskMaxUs = us;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

bool begin() {
    rxUart.setRxBufferSize(8192);
    rxUart.begin(38400, SERIAL_8N1, PIN_SX_DIO2 /*rx*/, -1 /*tx unused*/);
    rxUart.onReceiveError([](hardwareSerial_error_t e) {   // runs in the UART event task
        if (e == UART_FIFO_OVF_ERROR || e == UART_BUFFER_FULL_ERROR) g_uartOverflows = g_uartOverflows + 1;
    });
    g_rxReady = startRx();
    if (!g_rxReady) g_rec.record(millis(), false);         // the RX task retries, bounded
    // Above the Arduino loop (prio 1) on the loop's core: preempts it whenever it runs.
    g_taskUp = xTaskCreatePinnedToCore(rxTask, "ramses_rx", 4096, nullptr, 3, nullptr,
                                       xPortGetCoreID()) == pdPASS;
    if (!g_taskUp) { Serial.println("[radio] RX task could not be created"); g_rxReady = false; }
    return g_rxReady && g_taskUp;
}

void startReceive() {}                          // the RX task keeps the radio in RX

bool sendFrame(const ramses::Frame& f) {
    (void)f;
    Serial.println("[radio] TX blocked: RX-only firmware");
    return false;
}

bool poll(ramses::Frame& out) {                 // loop side: frames the RX task decoded
    rxs::Item it;
    if (!g_q.pop(it)) return false;
    out = it.f;
    g_rssi = it.rssi;                           // RSSI of this frame's own sync
    g_lastSeq = it.seq;
#if ORCON_TX_POC
    g_lastReceivedMs = it.ms;
#endif
    return true;
}

uint32_t lastSeq() { return g_lastSeq; }
bool pollSync(SyncInfo& out) { return g_sq.pop(out); }
uint32_t syncInfoDrops() { return g_sq.dropped.load(); }
#if ORCON_TX_POC
uint32_t lastReceivedMs() { return g_lastReceivedMs; }
#endif

#endif // RADIO_SX1276

// ===========================================================================
//  SX1276 packet-mode TX — the ONLY transmission path on the Heltec V2, and
//  only with the RF capability explicitly compiled in (ORCON_TX_RF_ENABLE).
//  Runs exclusively in the RX-task owner context (txpoc::Owner); the main loop
//  never touches the radio. The legacy radio::sendFrame above keeps refusing
//  on every V2 build, so no arbitrary/legacy payload can reach the air.
//
//  Software payload matches CC1101: eight UART training bytes, sync, Manchester,
//  checksum, stop and four idle pad bits = 53 bytes / 424 bits. Two hardware
//  training bytes precede it: nominal total 440 bits / 11.46 ms at 38.4 kbit/s.
//  No hardware sync/CRC/encoding. Power is explicitly set to 10 dBm.
//  The two-second admission floor is a software rate limit, not RF compliance
//  evidence: actual power, ramp, duration and RX return still require measurement.
// ===========================================================================
#if defined(RADIO_SX1276) && ORCON_TX_POC && ORCON_TX_RF_ENABLE
namespace sx1276 {

static bool g_txActive = false, g_savedRx = false;
// Registers changed by packet TX / RadioLib 7.8.1. FSK preamble lives at 25/26,
// NOT the LoRa 20/21 aliases (those are FSK receiver timeout registers).
static const uint8_t TX_REGS[] = { 0x09, 0x0A, 0x4D, 0x25, 0x26, 0x27,
                                  0x30, 0x31, 0x32, 0x35, 0x40, 0x41 };
static uint8_t g_saved[sizeof(TX_REGS)];
static const uint8_t FIFO_BYTES = 64;                // SX1276 FSK FIFO
static const uint32_t TX_US_PER_BYTE = 260;          // 208 us at 38.4 kbit/s, plus margin
static const uint32_t TX_SLACK_US = 10000;           // mode switch, ramp, 2 preamble bytes
// Why the last fast restore failed, for the web layer (0 = it succeeded):
// stage<<24 | reg<<16 | wrote<<8 | read; stage 1 standby, 2 register readback,
// 3 receiveDirect, 4 op mode not RX (low byte = RegOpMode).
static std::atomic<uint32_t> g_restoreDiag{0};
uint32_t restoreDiag() { return g_restoreDiag.load(std::memory_order_acquire); }
bool txActive() { return g_txActive; }

// Fast RX restore (also the owner's Restore step). A full beginFSK/reset/calibration is
// exclusively the existing bounded recovery's job, never repeated here.
static bool restoreNow() {
    uint32_t diag = 0;
    bool ok = g_radio.standby() == RADIOLIB_ERR_NONE;
    if (!ok) diag = 0x01000000u;
    if (g_savedRx) {
        for (size_t i = 0; i < sizeof(TX_REGS); ++i) {
            g_mod.SPIwriteRegister(TX_REGS[i], g_saved[i]);
            const uint8_t got = g_mod.SPIreadRegister(TX_REGS[i]);
            if (got != g_saved[i]) {
                ok = false;
                if (!diag) diag = 0x02000000u | uint32_t(TX_REGS[i]) << 16 | uint32_t(g_saved[i]) << 8 | got;
            }
        }
    }
    g_mod.SPIwriteRegister(RADIOLIB_SX127X_REG_IRQ_FLAGS_2, RADIOLIB_SX127X_FLAG_FIFO_OVERRUN);
    if (g_radio.receiveDirect() != RADIOLIB_ERR_NONE) { ok = false; if (!diag) diag = 0x03000000u; }
    // FSK RX passes through FSRx (op 0x04) first; RadioLib itself skips the RX bit check
    // here (#276). Poll briefly instead of judging the transition: bounded, ~1 ms.
    g_op = g_mod.SPIreadRegister(RADIOLIB_SX127X_REG_OP_MODE);
    for (int i = 0; i < 20 && (g_op & 0x87) != 0x05; ++i) {
        delayMicroseconds(50);
        g_op = g_mod.SPIreadRegister(RADIOLIB_SX127X_REG_OP_MODE);
    }
    if ((g_op & 0x87) != 0x05) { ok = false; if (!diag) diag = 0x04000000u | g_op; }
    g_restoreDiag.store(diag, std::memory_order_release);
    while (rxUart.available()) rxUart.read();
    g_dec.reset();  // discard partial frame / local echo, retain queued pre-TX frames
    g_rxReady = ok;
    g_savedRx = g_txActive = false;
    return ok;
}

// Outcome of the last started packet, completed inside txBegin (see there).
static bool g_finished = false, g_sentOk = false, g_restoredOk = false;

bool txBegin(const uint8_t* onair, size_t n) {
    // Fixed-length packets up to TX_MAX_BYTES (8-bit payload length; the 1FC9 offer is
    // ~106 B on air). Above the 64-byte FIFO the rest is refilled below while sending.
    if (!onair || n == 0 || n > TX_MAX_BYTES || g_txActive) return false;
    g_txActive = true;  // exclude normal UART decoding until restore, also on failure
    g_finished = g_sentOk = g_restoredOk = false;
    for (size_t i = 0; i < sizeof(TX_REGS); ++i) g_saved[i] = g_mod.SPIreadRegister(TX_REGS[i]);
    g_savedRx = true;
    const uint8_t fifoLen = n > FIFO_BYTES ? FIFO_BYTES : (uint8_t)n;   // RadioLib's own length check
    if (g_radio.standby() != RADIOLIB_ERR_NONE ||
        g_radio.packetMode() != RADIOLIB_ERR_NONE ||
        g_radio.fixedPacketLengthMode(fifoLen) != RADIOLIB_ERR_NONE ||
        g_radio.setEncoding(RADIOLIB_ENCODING_NRZ) != RADIOLIB_ERR_NONE ||
        g_radio.setCRC(false) != RADIOLIB_ERR_NONE ||
        g_radio.setDataShaping(RADIOLIB_SHAPING_0_5) != RADIOLIB_ERR_NONE ||
        g_radio.setOutputPower(10) != RADIOLIB_ERR_NONE) return false;
    // Raw buffer already contains all training, UART sync, Manchester, checksum
    // and stop bits. Use two explicit hardware training bytes; no hardware sync,
    // length, address or CRC bytes. Actual preamble/ramp must be measured on air.
    g_mod.SPIwriteRegister(RADIOLIB_SX127X_REG_SYNC_CONFIG, 0x00);
    g_mod.SPIwriteRegister(RADIOLIB_SX127X_REG_PREAMBLE_MSB_FSK, 0x00);
    g_mod.SPIwriteRegister(RADIOLIB_SX127X_REG_PREAMBLE_LSB_FSK, 0x02);
    g_mod.SPIwriteRegister(RADIOLIB_SX127X_REG_PACKET_CONFIG_1, 0x00);
    g_mod.SPIwriteRegister(RADIOLIB_SX127X_REG_PAYLOAD_LENGTH_FSK, (uint8_t)n);   // the real length
    // Fail closed on register write failure before entering TX.
    if (g_mod.SPIreadRegister(RADIOLIB_SX127X_REG_SYNC_CONFIG) != 0 ||
        g_mod.SPIreadRegister(RADIOLIB_SX127X_REG_PREAMBLE_MSB_FSK) != 0 ||
        g_mod.SPIreadRegister(RADIOLIB_SX127X_REG_PREAMBLE_LSB_FSK) != 2 ||
        g_mod.SPIreadRegister(RADIOLIB_SX127X_REG_PACKET_CONFIG_1) != 0 ||
        g_mod.SPIreadRegister(RADIOLIB_SX127X_REG_PACKET_CONFIG_2) != 0x40 ||
        g_mod.SPIreadRegister(RADIOLIB_SX127X_REG_PAYLOAD_LENGTH_FSK) != n) return false;
    // RadioLib loads all of a <= 64-byte packet, or the first 31 bytes of a longer one
    // (TX starts as soon as the FIFO is not empty).
    if (g_radio.startTransmit(onair, n) != RADIOLIB_ERR_NONE) return false;
    // The radio is now on air. Finish the packet here, in the RX task, without yielding:
    // refill byte by byte while the FIFO is not full, then wait for PacketSent and return
    // to RX at once. The unit answers a 1FC9 offer within a few ms; the owner's
    // one-step-per-5-ms-pass Cleanup/Restore would be too late. Bounded by airtime.
    size_t i = n > FIFO_BYTES ? RADIOLIB_SX127X_FIFO_THRESH - 1 : n;   // exactly what RadioLib loaded
    const uint32_t t0 = micros(), budget = (uint32_t)n * TX_US_PER_BYTE + TX_SLACK_US;
    bool ok = true;
    for (;;) {
        const uint8_t f = g_mod.SPIreadRegister(RADIOLIB_SX127X_REG_IRQ_FLAGS_2);
        if (f == 0xFF || (f & RADIOLIB_SX127X_FLAG_FIFO_OVERRUN)) { ok = false; break; }
        if (f & RADIOLIB_SX127X_FLAG_PACKET_SENT) { ok = i == n; break; }      // early = wrong length
        if (i < n) {
            if (f & RADIOLIB_SX127X_FLAG_FIFO_EMPTY) { ok = false; break; }   // underrun: frame broken on air
            if (!(f & RADIOLIB_SX127X_FLAG_FIFO_FULL)) g_mod.SPIwriteRegister(RADIOLIB_SX127X_REG_FIFO, onair[i++]);
        }
        if ((uint32_t)(micros() - t0) > budget) { ok = false; break; }
    }
    g_sentOk = ok;
    g_restoredOk = restoreNow();
    g_finished = true;
    return true;   // started: the outcome is read by txDone()/txFailed()/txRestoreRx()
}

bool txDone() { return g_finished && g_sentOk; }
bool txFailed() { return g_finished && !g_sentOk; }

bool txRestoreRx() {
    // txBegin already restored RX right after the packet: report that outcome once.
    if (g_finished) { g_finished = false; return g_restoredOk; }
    return restoreNow();   // txBegin failed before transmitting
}

} // namespace sx1276
#endif // RADIO_SX1276 && ORCON_TX_POC && ORCON_TX_RF_ENABLE

RxDiagnostics rxDiagnostics() {
    RxDiagnostics d;
#if defined(RADIO_SX1276)
    d.uartBytes = g_dec.bytes;
    d.syncHits = g_dec.syncsFF;
    d.coreSyncHits = g_dec.syncs;
    d.frames = g_dec.frames;
    d.bufferResets = g_dec.resets;
    d.bufferedBytes = (uint16_t)g_dec.buffered();
    d.uartOverflows = g_uartOverflows;
    d.pendingBytes = (uint16_t)rxUart.available();
    d.uartPendMax = (uint16_t)g_pendMax;
    d.lastUartMs = g_lastUartMs;
    d.lastSyncMs = g_lastSyncMs;
    d.opMode = g_op; d.lna = g_lna; d.rxConfig = g_rxcfg; d.rxBw = g_rxbw; d.rssiNow = g_rssiNow;
    d.queueDepth = (uint16_t)g_q.depth();
    d.queueHighWater = (uint16_t)g_q.highWater.load();
    d.queueDrops = g_q.dropped.load();
    d.taskMaxUs = g_taskMaxUs;
    d.stackFree = g_stackFree;
    d.rxState = (g_rxReady && g_taskUp) ? 0 : (g_rec.exhausted() || !g_taskUp) ? 2 : 1;
    d.rxErr = g_rxErr;
    d.rxRecoveries = g_rec.attempts;
    d.rxLost = g_rxLost;
#endif
    return d;
}

float lastRssi() { return g_rssi; }
#if !defined(RADIO_SX1276)
uint32_t lastSeq() { return 0; }
bool pollSync(SyncInfo&) { return false; }
uint32_t syncInfoDrops() { return 0; }
#endif

} // namespace radio
