# Orcon 15RF — native Rust migration

This directory is the **replacement implementation**, not a benchmark or a
wrapper around the Arduino firmware. It is deliberately separate from
PlatformIO while the entire firmware is being ported.

## Current code

- **orcon-protocol**: a dependency-free \`#![no_std]\` RAMSES-II codec.
  Covers frame headers and checksums, Manchester wire format, UART on-air
  encoding/decoding, and truncated-sync stream scanning.
- Test locally: \`cargo test --manifest-path rust/orcon-protocol/Cargo.toml\`.
- Current production code, OTA slots, RF transmission limits and flash
  instructions are **unchanged**.

## Migration target

- **Only hardware:** Heltec WiFi LoRa 32 V2, original ESP32 (Xtensa LX6),
  SX1276 (868 MHz), OLED.
- **Runtime candidate:** Rust + ESP-IDF, using the maintained
  \`esp-rs/esp-idf\` monorepo (not the archived standalone
  \`esp-rs/esp-idf-svc\` repository). Exact ESP-IDF / Rust versions and
  component APIs must be pinned after the first on-target build.
- Preserve the existing proven SX1276 **FSK direct-mode receive** behavior:
  DIO2 demodulated NRZ stream, UART at 38400 baud, bounded RX queue,
  task ownership and recovery.
- Preserve the RF transmit restrictions and explicit authorization flow.
- Preserve WiFi fallback, captive provisioning, web authentication,
  UI, profile semantics, Siri / Shortcuts API, protected credentials,
  signed updates, A/B rollback, recovery and partition handling.

## Implementation order (not a benchmarking exercise)

1. Port hardware-independent protocols, frame decoding, command state
   machines, RF budgets, profile validation and web API models.
2. Establish a compileable Rust firmware target for original ESP32 and
   the exact Heltec V2 pin assignments, boot / storage layout and watchdog.
3. Port SX1276 SPI + DIO2 / UART receive, maintain observable receive
   behavior; make RF TX unreachable by default.
4. Port WiFi, HTTP API, UI serving, auth, network recovery and Siri API.
5. Port provisioning, persistent profiles, signed OTA, A/B rollback,
   physical recovery and safe fallback.
6. Port RF pairing and TX with the existing limiting and physical
   authorization gates. Run hardware checks before ever enabling RF TX.
7. Complete integration tests and device acceptance. Only then offer
   Rust as a candidate for installation on the Heltec.

**Release gate:** a Rust build is not a firmware release until its
bootability, flash layout, credentials, rollback, RF reception and
safety behavior are verified on actual V2 hardware. Do not replace a
working image or bypass physical/RF approvals as a shortcut.

## Why rust-anything is not used as a converter

The original application is C++/Arduino with direct hardware side effects.
At the time of starting this migration, rust-anything lists C but not
C++ as a supported source language, and its record/replay workflow does
not validate embedded RF behavior. The Rust replacement is implemented
directly using the original code and tests as specifications.
