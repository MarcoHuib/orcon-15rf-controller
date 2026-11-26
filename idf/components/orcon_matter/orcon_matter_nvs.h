#pragma once
#include <esp_err.h>
namespace orcon_matter {
constexpr const char* kMatterNvsLabel = "chipkvs";
// Pre-initialises the dedicated Matter NVS when CHIP is configured for it (see orcon_matter_nvs.cpp);
// ESP_OK without doing anything when CHIP uses the shared "nvs" partition.
esp_err_t matterNvsBegin();
}  // namespace orcon_matter
