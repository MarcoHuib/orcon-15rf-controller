// config.h — user-tunable settings for the Orcon 15RF emulator.
// Most of these can also be changed at runtime from the web UI; the values here
// are the compile-time defaults that ship in flash.
#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Hardware: Heltec WiFi LoRa 32 V2 (ESP32 + on-board SX1276) only
// ---------------------------------------------------------------------------
// platformio.ini [env] defines RADIO_SX1276 and BOARD_HELTEC_V2 for every firmware build
// (ramses_radio.cpp refuses a build without them). Host tests leave them out on purpose to
// exercise the build switches below.

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
#define RF_RXBW_KHZ        250.0f      // SX1276 max RxBw — wide enough for RAMSES
#define RF_PREAMBLE_BYTES  6

// ---------------------------------------------------------------------------
// Heltec WiFi LoRa 32 V2 pin map
// ---------------------------------------------------------------------------
// On-board SX1276. In continuous/direct FSK mode the demodulated NRZ data stream
// comes out on DIO2 — we read it with a UART.
#define PIN_SX_NSS    18
#define PIN_SX_SCK     5
#define PIN_SX_MOSI   27
#define PIN_SX_MISO   19
#define PIN_SX_RST    14
#define PIN_SX_DIO0   26
#define PIN_SX_DIO1   35
#define PIN_SX_DIO2   34     // demod data out (continuous mode) -> ESP UART RX (input-only GPIO, fine)

// On-board SSD1306 OLED.
#define PIN_OLED_SDA  4
#define PIN_OLED_SCL  15
#define PIN_OLED_RST  16
#define PIN_VEXT      21      // drive LOW to power the OLED/Vext rail on V2
#define PIN_PRG_BTN   0       // PRG button -> GND. GPIO0 is the ESP32 boot-mode
                              // strapping pin: read-only use (INPUT_PULLUP, never
                              // driven), so reset+hold still enters the ROM
                              // flasher and short presses only wake the OLED.

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
// Siri via Apple Shortcuts (docs/SIRI.md): the control image only. It transmits, and its web login
// manages the Siri key.
#if ORCON_TX_CONTROL && ORCON_OWNER_MODEL
#define ORCON_SIRI 1
#else
#define ORCON_SIRI 0
#endif
// Published releases set this (platformio build flag, docs/RELEASES.md); a release manifest (v2)
// is never installed when older than the running release. 0 = a development build.
#ifndef ORCON_RELEASE_VERSION
#define ORCON_RELEASE_VERSION 0
#endif
