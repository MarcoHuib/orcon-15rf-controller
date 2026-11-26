// This controller's identity in Matter (CONFIG_CHIP_PROJECT_CONFIG, included by CHIP's CHIPConfig.h before its own
// defaults). Honest names: a self-built controller for an Orcon unit, not an Orcon product and not certified (the
// vendor ID stays the test ID 0xFFF1 of the DAC; Apple Home keeps its "niet-gecertificeerd" warning). The same macros
// name the bridged fan (orcon_matter.cpp createModel); a static_assert there checks that it sees these values.
#pragma once

#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME "Zelfbouw (niet van Orcon)"
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME "15RF-controller voor Orcon"
#define CHIP_DEVICE_CONFIG_DEFAULT_DEVICE_HARDWARE_VERSION_STRING "Heltec WiFi LoRa 32 V2"

// The commissionable DNS-SD advertisement (_matterc._udp, only while the owner's pairing window is open) carries
// DN=<name>. A generic product name, nothing personal.
#define CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONABLE_DEVICE_NAME 1
#define CHIP_DEVICE_CONFIG_DEVICE_NAME "Orcon ventilator"
