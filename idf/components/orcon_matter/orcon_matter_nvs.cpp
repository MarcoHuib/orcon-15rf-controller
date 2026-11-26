// Dedicated NVS storage for Matter (CHIP + ESP-Matter) WITHOUT changing the partition table:
// a 48 KB descriptor over the head of the existing, otherwise unused "spiffs" data partition
// (0x670000 on the board's default_8MB.csv layout). Orcon's own 20 KB "nvs" never sees fabric data.
//
// * Orcon uses only the LAST two 4 KB sectors of "spiffs" for the ownership sentinel
//   (owner_store.cpp: off() = size - (2 - i) * 4096). This region ends far before them, and every
//   esp_partition_read/write/erase on the descriptor is bounds-checked against its 48 KB size.
// * CHIP calls nvs_flash_init_partition(label) for its four namespaces; that returns ESP_OK for a
//   label that is already initialised (NVSPartitionManager::init_partition), so pre-initialising
//   the descriptor here makes CHIP and ESP-Matter use it (CONFIG_CHIP_*_PARTITION_LABEL and
//   CONFIG_ESP_MATTER_NVS_PART_NAME = "chipkvs", config/e18-matter-nvs.defaults).
// * Never erases on its own: NVS only gets a region that is blank or provably Matter's own NVS (only Matter's
//   namespaces, at most one page NVS itself left unfinished by a power cut); anything else (old SPIFFS bytes, a test
//   pattern, another NVS such as Orcon's, two broken pages) is reported and Matter stays off
//   (orcon_matter_nvs_check.h). A test board is prepared with one consented `esptool erase_region 0x670000 0xC000`.
// * A full Matter wipe = erase exactly this region; Orcon's NVS, Wi-Fi settings and keys survive.
#include "orcon_matter_nvs.h"
#include "orcon_matter_nvs_check.h"

#include <esp_log.h>
#include <esp_partition.h>
#include <esp_rom_crc.h>
#include <nvs_flash.h>
#include <stdlib.h>
#include <string.h>

namespace {
const char* TAG = "orcon_mtr_nvs";
constexpr uint32_t kSize = 0xC000;            // 12 NVS pages (11 usable x 126 entries)
constexpr uint32_t kPage = 4096;
constexpr uint32_t kSentinelSectors = 2;      // owner_store.cpp
esp_partition_t g_part;                       // NVS keeps this pointer: static storage

uint32_t romCrc(uint32_t crc, const uint8_t* buf, uint32_t len) { return esp_rom_crc32_le(crc, buf, len); }

orcon_matter::RegionCheck checkRegion(const esp_partition_t* part, bool& readOk) {
    orcon_matter::RegionCheck r;
    uint8_t* buf = static_cast<uint8_t*>(malloc(kPage));   // boot-time only; no static DRAM
    readOk = buf != nullptr;
    for (uint32_t off = 0; readOk && off < kSize; off += kPage) {
        readOk = esp_partition_read(part, off, buf, kPage) == ESP_OK;
        if (readOk) r.addPage(buf, romCrc);
    }
    free(buf);
    return r;
}
}  // namespace

namespace orcon_matter {
esp_err_t matterNvsBegin() {
    if (strcmp(CONFIG_CHIP_KVS_NAMESPACE_PARTITION_LABEL, kMatterNvsLabel) != 0) return ESP_OK;  // shared "nvs"
    const esp_partition_t* sp =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    if (!sp) return ESP_ERR_NOT_FOUND;
    if (sp->encrypted || sp->size < kSize + (kSentinelSectors + 2) * 4096u) return ESP_ERR_INVALID_SIZE;
    g_part = *sp;                             // same chip, address and flags; narrowed below
    g_part.subtype = ESP_PARTITION_SUBTYPE_DATA_NVS;
    g_part.size = kSize;
    strlcpy(g_part.label, kMatterNvsLabel, sizeof g_part.label);
    bool readOk = false;
    const orcon_matter::RegionCheck r = checkRegion(&g_part, readOk);
    if (!readOk) {   // no buffer, or a flash read error: never hand an unchecked region to NVS
        ESP_LOGE(TAG, "Matter NVS region not readable; Matter stays off");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Matter NVS 0x%06lx..0x%06lx: pages %u erased, %u nvs, %u header-only, %u unknown; namespaces %u "
             "Matter, %u foreign%s%s; %u orphaned entries", (unsigned long)g_part.address,
             (unsigned long)(g_part.address + kSize), unsigned(r.erased), unsigned(r.nvs), unsigned(r.headerOnly),
             unsigned(r.unknown), unsigned(r.matterNs), unsigned(r.foreignNs), r.foreignNs ? " e.g. " : "", r.foreign,
             unsigned(r.orphans()));
    if (!r.ok()) {   // nothing is written: the bytes stay as they are for a look, a backup or a consented erase
        ESP_LOGE(TAG, "Matter NVS region is not provably Matter's own (see the line above); Matter stays off until it "
                 "is erased once (consent)");
        return ESP_ERR_INVALID_STATE;
    }
    if (r.unknown || r.headerOnly)
        ESP_LOGW(TAG, "Matter NVS: one unfinished page next to Matter's own: NVS repairs it at init");
    return nvs_flash_init_partition_ptr(&g_part);
}
}  // namespace orcon_matter

// Orcon's factory reset (provision::resume(), at boot before Matter starts): all of Matter's storage goes, its fabrics
// and this device's Matter identity, so the next owner gets a new device with a new passcode. Only a region that is
// blank or provably Matter's own NVS is erased; foreign bytes stay (nothing of Matter to wipe there). A read error is
// "not done" (the reset retries at the next boot), never "nothing there".
extern "C" bool orcon_matter_factory_wipe(void) {
    const esp_partition_t* sp = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    if (!sp || sp->size < kSize) return true;   // no Matter storage on this layout
    esp_partition_t region = *sp;
    region.size = kSize;
    bool readOk = false;
    const orcon_matter::RegionCheck r = checkRegion(&region, readOk);
    if (!readOk) return false;
    if (!r.ok() || r.erased * kPage == kSize) return true;   // foreign data, or already blank
    nvs_flash_deinit_partition(orcon_matter::kMatterNvsLabel);   // when this boot opened it (else a harmless error)
    const esp_err_t err = esp_partition_erase_range(sp, 0, kSize);
    ESP_LOGW(TAG, "factory reset: Matter's storage (fabrics and identity) erased: %s", esp_err_to_name(err));
    return err == ESP_OK;
}
