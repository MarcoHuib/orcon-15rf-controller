#pragma once
// Pure check of the dedicated Matter NVS region before NVS may touch it (no ESP-IDF; host test:
// tools/test_nvs_check.cpp). NVS erases every page it does not recognise, so it may only see a region that is
// provably Matter's or provably empty (Codex review 2, H4). The rules, page by page (4 KB, NVS v2 layout):
//   Erased      all 0xFF.
//   Nvs         NVS state word (ACTIVE 0xFFFFFFFE, FULL 0xFFFFFFFC, FREEING 0xFFFFFFF8, CORRUPT 0xFFFFFFF0), version
//               0xFE at byte 8, CRC-32 of bytes 4..27 at byte 28 (nvs_page.hpp Header). Its written entries are read:
//               every namespace definition must carry one of Matter's names, every entry a namespace defined so.
//   HeaderOnly  an NVS state word, the header unfinished (bad CRC or version), everything after it 0xFF: a page
//               initialisation cut short. NVS writes a page header once, before any entry, so nothing is stored there.
//   Unknown     anything else: an erase cut short, a header gone bad over stored entries, foreign bytes.
// A region is accepted when no namespace is foreign and no entry is orphaned, and it holds no Unknown page, or exactly
// one next to Matter's own NVS: NVS initialises or erases one page at a time, so its own power-loss leftovers never
// exceed one page. Everything else (old SPIFFS bytes, a test pattern, another NVS such as Orcon's own, two broken
// pages) stays untouched and Matter stays off until the region is erased once, with consent.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace orcon_matter {
using Crc32Fn = uint32_t (*)(uint32_t crc, const uint8_t* buf, uint32_t len);   // esp_rom_crc32_le semantics

constexpr size_t kNvsPageSize = 4096, kNvsEntries = 126;
enum class PageKind : uint8_t { Erased, Nvs, HeaderOnly, Unknown };

inline bool nvsStateWord(uint32_t s) { return s == 0xFFFFFFFEu || s == 0xFFFFFFFCu || s == 0xFFFFFFF8u || s == 0xFFFFFFF0u; }

// Matter's namespaces: CHIP's ESP32 platform (ESP32Config.cpp: chip-factory, chip-config, chip-counters;
// KeyValueStoreManagerImpl.h: CHIP_KVS) and ESP-Matter (esp_matter_nvs.h: esp_matter_kvs; "node" and
// "endpoint_<hex>" only as the source of its own one-way migration). Checked against the QEMU chipkvs dumps.
inline bool matterNamespace(const char* n) {
    static const char* const kNames[] = {"chip-factory", "chip-config", "chip-counters", "CHIP_KVS", "esp_matter_kvs",
                                         "node"};
    for (const char* k : kNames)
        if (strcmp(n, k) == 0) return true;
    if (strncmp(n, "endpoint_", 9) != 0) return false;
    size_t i = 9;
    for (; n[i]; ++i)
        if (!((n[i] >= '0' && n[i] <= '9') || (n[i] >= 'A' && n[i] <= 'F'))) return false;
    return i > 9 && i <= 13;   // "endpoint_%X" of a uint16_t
}

inline bool allFF(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (p[i] != 0xFF) return false;
    return true;
}

inline PageKind classifyPage(const uint8_t* p, Crc32Fn crc) {
    uint32_t state, c;
    memcpy(&state, p, 4);
    if (state == 0xFFFFFFFFu) return allFF(p, kNvsPageSize) ? PageKind::Erased : PageKind::Unknown;
    if (!nvsStateWord(state)) return PageKind::Unknown;
    memcpy(&c, p + 28, 4);
    if (p[8] == 0xFE && crc(0xFFFFFFFFu, p + 4, 24) == c) return PageKind::Nvs;
    return allFF(p + 32, kNvsPageSize - 32) ? PageKind::HeaderOnly : PageKind::Unknown;
}

struct RegionCheck {
    uint8_t erased = 0, nvs = 0, headerOnly = 0, unknown = 0;
    uint16_t matterNs = 0, foreignNs = 0;
    uint8_t defined[32] = {}, used[32] = {};   // namespace indexes: defined with a Matter name / used by an entry
    char foreign[16] = "";                     // the first foreign namespace name, for the log

    void addPage(const uint8_t* p, Crc32Fn crc) {
        const PageKind k = classifyPage(p, crc);
        ++(k == PageKind::Erased ? erased : k == PageKind::Nvs ? nvs : k == PageKind::HeaderOnly ? headerOnly : unknown);
        if (k == PageKind::Nvs) readEntries(p, crc);
    }
    uint16_t orphans() const {   // entries in a namespace no Matter definition names
        uint16_t n = 0;
        for (int i = 0; i < 32; ++i)
            for (uint8_t b = used[i] & ~defined[i]; b; b &= b - 1) ++n;
        return n;
    }
    bool ok() const {
        if (foreignNs || orphans()) return false;
        return unknown == 0 || (unknown == 1 && matterNs > 0);
    }

private:
    // Entry state bitmap at 32..63 (2 bits per entry: 3 empty, 2 written, 0 erased); entries at 64 + 32 * i:
    // ns, type, span, chunk index, CRC-32 (of bytes 0..3 and 8..31), key[16], data[8] (nvs_types.hpp Item).
    void readEntries(const uint8_t* p, Crc32Fn crc) {
        for (size_t i = 0; i < kNvsEntries;) {
            const uint8_t* e = p + 64 + 32 * i;
            if (((p[32 + i / 4] >> ((i % 4) * 2)) & 3) != 2) { ++i; continue; }
            uint32_t c;
            memcpy(&c, e + 4, 4);
            if (crc(crc(0xFFFFFFFFu, e, 4), e + 8, 24) != c) { ++i; continue; }   // a write cut short: NVS drops it
            if (e[0] == 0 && e[1] == 0x01) {                                       // namespace definition: key -> index
                char name[17];
                memcpy(name, e + 8, 16);
                name[16] = 0;
                if (matterNamespace(name)) { defined[e[24] / 8] |= uint8_t(1u << (e[24] % 8)); ++matterNs; }
                else if (!foreignNs++) memcpy(foreign, name, sizeof foreign - 1);
            } else if (e[0] != 0) {
                used[e[0] / 8] |= uint8_t(1u << (e[0] % 8));
            }
            i += e[2] ? e[2] : 1;   // span: a string or blob chunk occupies e[2] slots
        }
    }
};
}  // namespace orcon_matter
