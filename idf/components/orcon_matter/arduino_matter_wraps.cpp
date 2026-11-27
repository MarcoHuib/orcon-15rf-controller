// Pass-through definitions for the link-time wraps that arduino-esp32 3.3.12's CMakeLists.txt adds
// whenever ESP-Matter's data model is in the build (lines 520-535: --wrap + -u for
// esp_matter_mem_calloc/realloc/free and ESP32Utils::InitWiFiStack). The wrapper bodies live in
// Arduino's Matter library, which this build leaves out (CONFIG_ARDUINO_SELECTIVE_Matter=n), so
// without these the link fails with undefined __wrap_ symbols.
//
// Behaviour: exactly ESP-Matter's own functions (__real_). Arduino's versions prefer PSRAM and are
// IRAM_ATTR; this board has no PSRAM and none of these is called from an ISR, so plain flash
// code that keeps CONFIG_ESP_MATTER_MEM_ALLOC_MODE_INTERNAL is the equivalent.
#include <stddef.h>
#include <sdkconfig.h>
#include <lib/core/CHIPError.h>

extern "C" {
void* __real__Z21esp_matter_mem_callocjj(size_t n, size_t size);
void* __real__Z22esp_matter_mem_reallocPvj(void* ptr, size_t size);
void __real__Z19esp_matter_mem_freePv(void* ptr);

void* __wrap__Z21esp_matter_mem_callocjj(size_t n, size_t size) { return __real__Z21esp_matter_mem_callocjj(n, size); }
void* __wrap__Z22esp_matter_mem_reallocPvj(void* ptr, size_t size) { return __real__Z22esp_matter_mem_reallocPvj(ptr, size); }
void __wrap__Z19esp_matter_mem_freePv(void* ptr) { __real__Z19esp_matter_mem_freePv(ptr); }

#if CONFIG_ENABLE_WIFI_STATION   // Arduino adds this wrap only then (CMakeLists.txt:522)
CHIP_ERROR __real__ZN4chip11DeviceLayer8Internal10ESP32Utils13InitWiFiStackEv(void);
// CHIP's InitWiFiStack() already tolerates Wi-Fi that Orcon's net layer initialised first
// (reuses the WIFI_STA_DEF netif, skips esp_wifi_init when the driver runs).
CHIP_ERROR __wrap__ZN4chip11DeviceLayer8Internal10ESP32Utils13InitWiFiStackEv(void) {
    return __real__ZN4chip11DeviceLayer8Internal10ESP32Utils13InitWiFiStackEv();
}
#endif
}
