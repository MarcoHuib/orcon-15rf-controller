// config.h — user-tunable settings for the Orcon 15RF emulator.
// Most of these can also be changed at runtime from the web UI; the values here
// are the compile-time defaults that ship in flash.
#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Radio selection
// ---------------------------------------------------------------------------
// Default: use the Heltec WiFi LoRa 32 V3 built-in SX1262 (software Manchester).
// To use an external CC1101 instead, build with  -D RADIO_CC1101  (see
// platformio.ini [env:heltec_v3_cc1101]) and wire it per README.
#if !defined(RADIO_SX1262) && !defined(RADIO_CC1101) && !defined(RADIO_CC1101_ASYNC) && !defined(RADIO_SX1276)
  #define RADIO_SX1262 1
#endif

// RX-only builds: the SX1276 backend has no TX path. RX_ONLY additionally gates
// every command, pairing and network firmware-update path (orcon.cpp, net.cpp).
#ifndef RX_ONLY
#if defined(RADIO_SX1276)
#define RX_ONLY 1
#else
#define RX_ONLY 0
#endif
#endif

#include "tx_poc_caps.h"

#ifndef FW_VERSION
#define FW_VERSION "dev"     // CI sets the git commit (platformio build flag)
#endif

// RAMSES-II / Orcon RF parameters (see docs/PROTOCOL.md §1)
#define RF_FREQ_MHZ        868.300f
#define RF_BITRATE_KBPS    38.4f
#define RF_DEVIATION_KHZ   50.78f
#if defined(BOARD_HELTEC_V2)
#define RF_RXBW_KHZ        250.0f      // SX1276 max RxBw — wide enough for RAMSES
#else
#define RF_RXBW_KHZ        312.0f      // nearest SX1262-valid step to the protocol's 325k
                                       // (CC1101 path maps this to ~325k automatically)
#endif
#define RF_PREAMBLE_BYTES  6

// ---------------------------------------------------------------------------
// Heltec WiFi LoRa 32 V3 (ESP32-S3) pin map
// ---------------------------------------------------------------------------
#if defined(BOARD_HELTEC_V2)
// On-board SX1276 (Heltec WiFi LoRa 32 V2, ESP32). In continuous/direct FSK mode
// the demodulated NRZ data stream comes out on DIO2 — we read it with a UART.
#define PIN_SX_NSS    18
#define PIN_SX_SCK     5
#define PIN_SX_MOSI   27
#define PIN_SX_MISO   19
#define PIN_SX_RST    14
#define PIN_SX_DIO0   26
#define PIN_SX_DIO1   35
#define PIN_SX_DIO2   34     // demod data out (continuous mode) -> ESP UART RX (input-only GPIO, fine)
#else
// On-board SX1262 (fixed on the Heltec V3/V4)
#define PIN_SX_NSS    8
#define PIN_SX_SCK    9
#define PIN_SX_MOSI   10
#define PIN_SX_MISO   11
#define PIN_SX_RST    12
#define PIN_SX_BUSY   13
#define PIN_SX_DIO1   14
#endif

#if defined(BOARD_4848S040)
// ---------------------------------------------------------------------------
// Guition ESP32-4848S040 (ESP32-S3 + 480x480 RGB TFT). No SSD1306 OLED.
// Solder the CC1101 directly to the ESP32 pins. The radio SHARES the display/
// SD SPI bus (idle after the display init); the three free relay/audio pins
// carry chip-select and the two async data lines.
// IMPORTANT: lift R21/R22/R23 (the audio 0R jumpers) so the NS4168 amp doesn't
// load these lines; lift R25/R26/R27 too if the relay board is fitted.
// ---------------------------------------------------------------------------
#define NO_OLED       1
#define PIN_OLED_SDA  17      // (unused on this board — OLED code is compiled out)
#define PIN_OLED_SCL  18
#define PIN_OLED_RST  21
#define PIN_VEXT      21      // NOT 36 here (GPIO36 is OPI-PSRAM); never driven
#define PIN_CC_SCK    48      // shared SPI clock (display 9-bit init / SD)
#define PIN_CC_MOSI   47      // shared SPI MOSI
#define PIN_CC_MISO   41      // shared SPI MISO (SD DO)
#define PIN_CC_NSS    40      // free pin (relay1 / audio DIN)
#define PIN_CC_GDO0   2       // free pin (relay2 / audio LRCLK) -> async UART TX
#define PIN_CC_GDO2   1       // free pin (relay3 / audio BCLK)  -> async UART RX
#elif defined(BOARD_HELTEC_V2)
// On-board SSD1306 OLED (Heltec V2): different I2C pins than V3.
// No external CC1101 — the on-board SX1276 does RX (direct) + TX (packet).
#define PIN_OLED_SDA  4
#define PIN_OLED_SCL  15
#define PIN_OLED_RST  16
#define PIN_VEXT      21      // drive LOW to power the OLED/Vext rail on V2
#define PIN_PRG_BTN   0       // PRG button -> GND. GPIO0 is the ESP32 boot-mode
                              // strapping pin: read-only use (INPUT_PULLUP, never
                              // driven), so reset+hold still enters the ROM
                              // flasher and short presses only wake the OLED.
#else
// On-board SSD1306 OLED (Heltec V3/V4): I2C + Vext power + reset
#define PIN_OLED_SDA  17
#define PIN_OLED_SCL  18
#define PIN_OLED_RST  21
#define PIN_VEXT      36      // drive LOW to power OLED/Vext rail on V3

// External CC1101 (used when -D USE_CC1101_TX / -D RADIO_CC1101_ASYNC).
// The SX1262's SPI (GPIO 8-14) is NOT broken out on the Heltec V3/V4 headers,
// so the CC1101 gets its OWN dedicated SPI bus on the SAFE free header pins
// {1,2,4,5,6,7,47,48}. (GPIO26 and 33-38 are flash/SubSPI on the S3 — unusable.)
#define PIN_CC_SCK    4     // CC1101 SCLK
#define PIN_CC_MOSI   2     // CC1101 MOSI
#define PIN_CC_MISO   1     // CC1101 MISO
#define PIN_CC_NSS    7     // CC1101 CSN
#define PIN_CC_GDO0   5     // CC1101 GDO0 = async serial data line (UART TX)
#define PIN_CC_GDO2   6     // CC1101 GDO2 (optional / status)
#endif

// ---------------------------------------------------------------------------
// Device identities (RAMSES device IDs:  class:serial, serial is 18-bit)
// ---------------------------------------------------------------------------
// These are what the fan will "see". Keep the serial unique on your RF network.
// Class 0x25 (=37) = a 15RF CO2/HRU controller — what the Orcon registers when you
// pair a 15RF (the "Auto+1" procedure). Proven to drive the HRC400. We send REM
// (mode/bypass) and CO2/demand from this same ID for simplicity. (0x1D/REM also
// works on many Itho/Orcon units — change here if a unit won't obey.)
#define DEV_US_CLASS     0x25
// Serial is auto-generated uniquely from the ESP chip ID on first boot (net.cpp);
// this compile-time value is only a fallback if generation is ever skipped.
#define DEV_US_SERIAL    0x012345UL

// The fan's device ID. Leave 0 to auto-learn it from the first 042F/31D9/31DA
// broadcast we receive (recommended), or hard-code it if you already know it.
#define DEV_FAN_CLASS    0x20          // 0x20 (=32) = FAN/HVAC
#define DEV_FAN_SERIAL   0x000000UL    // 0 => auto-learn

// Broadcast address (63:262143) used as destination for some I-messages.
#define DEV_BCAST_CLASS  0x3F
#define DEV_BCAST_SERIAL 0x3FFFFUL

// ---------------------------------------------------------------------------
// Emulated-sensor behaviour
// ---------------------------------------------------------------------------
#define SEND_CO2_INTERVAL_MS     (10UL*60UL*1000UL)   // 1298 every ~10 min
#define SEND_DEMAND_INTERVAL_MS  ( 5UL*60UL*1000UL)   // 31E0 every ~5 min
#define SEND_DEVINFO_INTERVAL_MS (24UL*3600UL*1000UL) // 10E0 daily
#define SEND_STATUS_INTERVAL_MS  (60UL*1000UL)         // RQ 31DA every 60s (live status)

// ---------------------------------------------------------------------------
// Networking (defaults; override at runtime via the web UI / SoftAP portal)
// ---------------------------------------------------------------------------
#define WIFI_SSID        ""            // empty => start SoftAP setup portal
#define WIFI_PASSWORD    ""
#define WIFI_AP_NAME     "Orcon15RF-Setup"
#define WIFI_AP_PASS     "orconsetup"  // ONLY the OLED-less 4848; boards with an OLED use a random per-device password

#if ORCON_TX_POC
#define MQTT_ENABLED     0          // no broker, discovery or command route in phase 1
#else
#define MQTT_ENABLED     1
#endif
#if RX_ONLY
#define MQTT_HOST        ""             // RX-only: off until a broker is set explicitly
#else
#define MQTT_HOST        "192.168.1.10" // TX builds: unchanged default (existing setups)
#endif
#define MQTT_PORT        1883
#define MQTT_USER        ""
#define MQTT_PASS        ""
#if RX_ONLY
// Own namespace so an RX-only test board never overwrites a production board's
// retained state / Home Assistant entities on the same broker.
#define MQTT_BASE_TOPIC  "orcon15rf_rx"
#define MQTT_NODE_ID     "orcon15rf_rx"
#define MQTT_UID_PREFIX  "orcon_rx_"
#else
#define MQTT_BASE_TOPIC  "orcon15rf"   // orcon15rf/state, orcon15rf/cmd, ...
#define MQTT_NODE_ID     "orcon15rf"
#define MQTT_UID_PREFIX  "orcon_"      // keep: existing Home Assistant entity IDs
#endif

// ---------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------
#define DEBUG_SERIAL_BAUD 115200
#define DEBUG_RX_HEX      1            // log raw decoded frames to serial

// Signed A/B OTA over WiFi (ota_signed.cpp): Heltec V2 + SX1276 only. ECDSA-signed manifest,
// approved variants only, bootloader rollback until the new image confirms its own health.
#if defined(BOARD_HELTEC_V2) && defined(RADIO_SX1276)
#define ORCON_SIGNED_OTA 1
#else
#define ORCON_SIGNED_OTA 0
#endif
// Web login (web_auth / auth_store): password, sessions and CSRF for every control and settings
// route. Needs the signed-OTA key: a password is only (re)set with a code the device issues for
// a request signed with that key. Other boards keep the per-boot token of the setup page.
#ifndef ORCON_WEB_AUTH
#if ORCON_SIGNED_OTA
#define ORCON_WEB_AUTH 1
#else
#define ORCON_WEB_AUTH 0
#endif
#endif
#if ORCON_WEB_AUTH && !ORCON_SIGNED_OTA
#error "ORCON_WEB_AUTH needs ORCON_SIGNED_OTA (password recovery is signed with the OTA key)"
#endif
#if ORCON_TX_CONTROL && !ORCON_WEB_AUTH
#error "the control image transmits for anyone who can use its web page: it requires the web login"
#endif
// Ownership model (docs/PROVISIONING.md): installation wizard over the setup access point, a
// per-device owner key, the PRG recovery menu and the factory reset. Every build with the web login
// and the setup web UI (not the old TX-PoC portal).
#if ORCON_WEB_AUTH && (!ORCON_TX_POC || ORCON_WEB_CONTROL)
#define ORCON_OWNER_MODEL 1
#else
#define ORCON_OWNER_MODEL 0
#endif
// Published releases set this (platformio build flag, docs/RELEASES.md); a release manifest (v2)
// is never installed when older than the running release. 0 = a development build.
#ifndef ORCON_RELEASE_VERSION
#define ORCON_RELEASE_VERSION 0
#endif
