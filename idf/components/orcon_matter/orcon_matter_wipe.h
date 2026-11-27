#pragma once
// Matter's storage region and Orcon's factory reset step for it, in EVERY build: the Matter component
// (orcon_matter_nvs.cpp reads the region the same way before NVS may touch it) and Orcon itself
// (src/owner_store.cpp eraseMatter(), also in firmware without Matter). After a switch from Matter firmware to one
// without it, the region still holds the fabrics and this device's Matter identity, its DAC private key included: a
// factory reset there removes them too, so the next owner gets a new device with a new passcode.
// Only a region that is blank or provably Matter's own NVS (orcon_matter_nvs_check.h) is erased; foreign bytes stay.
// A read error is "not done" (the reset retries at the next boot), never "nothing there".
// Only calls that Arduino-ESP32 2.0 (ESP-IDF 4.4, the PlatformIO builds) and ESP-IDF 5.5 both have.
#include "orcon_matter_nvs.h"
#include "orcon_matter_nvs_check.h"

#include <esp_log.h>
#include <esp_partition.h>
#include <esp_rom_crc.h>
#include <nvs_flash.h>
#include <stdlib.h>

namespace orcon_matter {
constexpr uint32_t kMatterNvsSize = 0xC000;   // 12 NVS pages at the head of "spiffs" (orcon_matter_nvs.cpp)

inline uint32_t romCrc32(uint32_t crc, const uint8_t* buf, uint32_t len) { return esp_rom_crc32_le(crc, buf, len); }

// The region page by page through the check. readOk false: no buffer, or a flash read error.
inline RegionCheck checkMatterRegion(const esp_partition_t* region, bool& readOk) {
    RegionCheck r;
    uint8_t* buf = static_cast<uint8_t*>(malloc(kNvsPageSize));   // boot and reset only: no static DRAM
    readOk = buf != nullptr;
    for (uint32_t off = 0; readOk && off < kMatterNvsSize; off += kNvsPageSize) {
        readOk = esp_partition_read(region, off, buf, kNvsPageSize) == ESP_OK;
        if (readOk) r.addPage(buf, romCrc32);
    }
    free(buf);
    return r;
}

// provision::resume() at boot, before Matter starts: false only for "not done" (retry at the next boot).
inline bool wipeMatterStorage() {
    const esp_partition_t* sp = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    if (!sp || sp->size < kMatterNvsSize) return true;   // no Matter storage on this layout
    esp_partition_t region = *sp;
    region.size = kMatterNvsSize;
    bool readOk = false;
    const RegionCheck r = checkMatterRegion(&region, readOk);
    if (!readOk) return false;
    if (!r.ok() || r.erased * kNvsPageSize == kMatterNvsSize) return true;   // foreign data, or already blank
    nvs_flash_deinit_partition(kMatterNvsLabel);   // when this boot opened it (else a harmless error)
    const esp_err_t err = esp_partition_erase_range(sp, 0, kMatterNvsSize);
    ESP_LOGW("orcon_mtr_nvs", "factory reset: Matter's storage (fabrics and identity) erased: %s", esp_err_to_name(err));
    return err == ESP_OK;
}
}  // namespace orcon_matter
