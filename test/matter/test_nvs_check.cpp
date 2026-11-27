// Host test of idf/components/orcon_matter/orcon_matter_nvs_check.h (the Matter NVS ownership check, Codex review 2 H4).
//   test/run_host_tests.sh (matter_nvs): the synthetic cases; real regions as extra arguments
//   /tmp/t [ok:<region.bin> | refuse:<region.bin> ...]
// Real regions tie the host CRC and entry parsing to what the device wrote: chipkvs dumps from the QEMU runs
// (written by the firmware's own NVS, esp_rom_crc32_le) must pass, Orcon's own NVS and the two-broken-pages fixture
// must be refused.
#include "orcon_matter_nvs_check.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace orcon_matter;
using Page = std::vector<uint8_t>;

static uint32_t crc32le(uint32_t crc, const uint8_t* b, uint32_t n) {   // esp_rom_crc32_le: ~ in, ~ out
    crc = ~crc;
    while (n--) {
        crc ^= *b++;
        for (int k = 0; k < 8; ++k) crc = crc & 1 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return ~crc;
}

static Page erased() { return Page(4096, 0xFF); }
static Page header(uint32_t state, bool goodCrc, uint8_t version = 0xFE) {
    Page p = erased();
    memcpy(p.data(), &state, 4);
    const uint32_t seq = 7;
    memcpy(p.data() + 4, &seq, 4);
    p[8] = version;
    const uint32_t c = crc32le(0xFFFFFFFFu, p.data() + 4, 24) ^ (goodCrc ? 0u : 1u);
    memcpy(p.data() + 28, &c, 4);
    return p;
}
struct Entry { uint8_t ns, type, span; std::string key; uint8_t d0; bool goodCrc = true; };
static Entry def(const std::string& name, uint8_t idx) { return {0, 0x01, 1, name, idx}; }
static Entry u8(uint8_t ns, const std::string& key) { return {ns, 0x01, 1, key, 1}; }
// A valid page with these entries in slots 0.., each marked written; a span > 1 also marks its raw slots written.
static Page page(const std::vector<Entry>& es, uint32_t state = 0xFFFFFFFEu) {
    Page p = header(state, true);
    size_t slot = 0;
    for (const Entry& e : es) {
        uint8_t* q = p.data() + 64 + 32 * slot;
        q[0] = e.ns; q[1] = e.type; q[2] = e.span; q[3] = 0xFF;
        memset(q + 8, 0, 16);
        memcpy(q + 8, e.key.data(), e.key.size() < 16 ? e.key.size() : 15);
        q[24] = e.d0;
        uint32_t c = crc32le(crc32le(0xFFFFFFFFu, q, 4), q + 8, 24) ^ (e.goodCrc ? 0u : 1u);
        memcpy(q + 4, &c, 4);
        for (size_t k = 0; k < e.span; ++k, ++slot) p[32 + slot / 4] &= uint8_t(~(1u << ((slot % 4) * 2)));   // 3 -> 2
    }
    return p;
}
static bool region(const std::vector<Page>& pages, RegionCheck* out = nullptr) {
    RegionCheck r;
    for (const Page& p : pages) r.addPage(p.data(), crc32le);
    if (out) *out = r;
    return r.ok();
}
static std::vector<Page> blank() { return std::vector<Page>(12, erased()); }
static std::vector<Page> with(std::vector<Page> base, size_t i, const Page& p) { base[i] = p; return base; }

int main(int argc, char** argv) {
    const uint8_t v[] = "123456789";
    assert(crc32le(0, v, 9) == 0xCBF43926u);   // the standard CRC-32 check value

    // names
    for (const char* n : {"chip-factory", "chip-config", "chip-counters", "CHIP_KVS", "esp_matter_kvs", "node", "orcon-id",
                          "endpoint_0", "endpoint_1F", "endpoint_FFFF"})
        assert(matterNamespace(n));
    for (const char* n : {"orcon", "orconown", "orcon-idx", "orcon-i", "nvs.net80211", "chip_kvs", "endpoint_", "endpoint_1g",
                          "endpoint_12345", ""})
        assert(!matterNamespace(n));

    // pages
    Page halfErased = erased();                 // an erase cut short: state word back to 0xFF.., old bytes behind it
    for (int i = 2048; i < 4096; ++i) halfErased[i] = 0x00;
    Page damaged = header(0xFFFFFFFEu, false);  // a header gone bad over stored entries
    for (int i = 64; i < 600; ++i) damaged[i] = uint8_t(i * 7);
    const Page foreign(4096, 0x5A);             // the QEMU n2 fill pattern
    const Page matter = page({def("chip-config", 1), u8(1, "rebootcount"), def("CHIP_KVS", 2), u8(2, "g/fidx")});
    assert(classifyPage(erased().data(), crc32le) == PageKind::Erased);
    assert(classifyPage(matter.data(), crc32le) == PageKind::Nvs);
    assert(classifyPage(page({}, 0xFFFFFFFCu).data(), crc32le) == PageKind::Nvs);          // FULL, no entries
    assert(classifyPage(page({}, 0xFFFFFFF8u).data(), crc32le) == PageKind::Nvs);          // FREEING
    assert(classifyPage(header(0xFFFFFFFEu, false).data(), crc32le) == PageKind::HeaderOnly);        // init cut short
    assert(classifyPage(header(0xFFFFFFFEu, true, 0xFF).data(), crc32le) == PageKind::HeaderOnly);   // version unwritten
    assert(classifyPage(damaged.data(), crc32le) == PageKind::Unknown);
    assert(classifyPage(halfErased.data(), crc32le) == PageKind::Unknown);
    assert(classifyPage(foreign.data(), crc32le) == PageKind::Unknown);
    assert(classifyPage(header(0x12345678u, true).data(), crc32le) == PageKind::Unknown);

    // regions
    RegionCheck r;
    assert(region(blank()));                                                         // a fresh board
    assert(region(with(blank(), 0, matter), &r) && r.matterNs == 2 && r.orphans() == 0);
    assert(region(with(blank(), 0, header(0xFFFFFFFEu, false))));                   // first page init cut short
    assert(region(with(blank(), 0, page({}))));                                     // initialised, nothing stored yet
    assert(region(with(with(blank(), 0, matter), 5, halfErased)));                  // one erase cut short: NVS repairs it
    assert(region(with(with(blank(), 0, matter), 5, damaged)));                     // one page gone bad next to Matter's
    assert(!region(with(with(with(blank(), 0, matter), 3, damaged), 5, halfErased)));   // two: not NVS's own leftovers
    assert(!region(with(blank(), 4, halfErased)));                                  // one unknown page, no Matter data
    assert(!region(with(with(blank(), 0, header(0xFFFFFFFEu, false)), 4, damaged)));
    assert(!region(std::vector<Page>(12, foreign)));                                 // n2: foreign bytes
    assert(!region(with(std::vector<Page>(12, foreign), 0, matter)));
    // another NVS: valid structure, foreign namespaces (Orcon's own NVS written into the region by mistake)
    assert(!region(with(blank(), 0, page({def("orconown", 1), u8(1, "owner"), def("chip-config", 2)})), &r));
    assert(r.foreignNs == 1 && std::string(r.foreign) == "orconown");
    // entries whose namespace no Matter definition names (its definition missing or elsewhere)
    assert(!region(with(blank(), 0, page({def("CHIP_KVS", 1), u8(2, "x")})), &r) && r.orphans() == 1);
    // entries with a broken CRC (a write cut short) are dropped by NVS and count for nothing, also as definitions
    Entry badDef = def("orcon", 3); badDef.goodCrc = false;
    Entry badUse = u8(3, "y"); badUse.goodCrc = false;
    assert(region(with(blank(), 0, page({def("CHIP_KVS", 1), badDef, badUse, u8(1, "z")})), &r) && r.foreignNs == 0);
    // a string's raw slot is not parsed as an entry: here it holds bytes that look like a foreign definition
    {
        Page p = page({def("CHIP_KVS", 1), {1, 0x21, 2, "str", 0}, u8(1, "after")});
        uint8_t* rawSlot = p.data() + 64 + 32 * 2;   // slot 2 = the string's data slot
        const Page fake = page({def("orcon", 9)});
        memcpy(rawSlot, fake.data() + 64, 32);
        assert(region(with(blank(), 0, p), &r) && r.foreignNs == 0 && r.matterNs == 1);
    }

    // real regions
    int files = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool wantOk = a.rfind("ok:", 0) == 0;
        assert(wantOk || a.rfind("refuse:", 0) == 0);
        const std::string path = a.substr(wantOk ? 3 : 7);
        std::ifstream f(path, std::ios::binary);
        const Page img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        assert(!img.empty() && img.size() % 4096 == 0);
        RegionCheck rc;
        for (size_t off = 0; off < img.size(); off += 4096) rc.addPage(img.data() + off, crc32le);
        printf("  %-7s %s: pages %u erased, %u nvs, %u header-only, %u unknown; namespaces %u Matter, %u foreign%s%s; "
               "orphans %u\n", rc.ok() ? "ok" : "refused", path.c_str(), rc.erased, rc.nvs, rc.headerOnly, rc.unknown,
               rc.matterNs, rc.foreignNs, rc.foreignNs ? " e.g. " : "", rc.foreign, rc.orphans());
        assert(rc.ok() == wantOk);
        ++files;
    }
    printf("test_nvs_check: PASS (%d real regions)\n", files);
    return 0;
}
