// test_rx.cpp — async-RX decode chain (CC1101 GDO2 / SX1276 DIO2 -> UART -> codec).
// Build: g++ -std=c++17 -I src test/test_rx.cpp src/rx_stream.cpp src/ramses_codec.cpp -o /tmp/test_rx
#include "ramses_codec.h"
#include "rx_stream.h"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace ramses;
static int fails = 0;
#define CHECK(c,m) do{ if(!(c)){printf("  [FAIL] %s\n",m);fails++;} else printf("  [ ok ] %s\n",m);}while(0)

static Frame mk(MsgType t, uint16_t cmd, const uint8_t* pl, uint8_t len) {
    Frame f; f.type=t;
    f.has_addr[0]=true; f.addr[0]=DeviceId(0x1D,0x012345);
    f.has_addr[1]=true; f.addr[1]=DeviceId(0x20,0x02BCDE);
    f.has_addr[2]=false;
    f.cmd=cmd; f.len=len; for(uint8_t i=0;i<len;i++) f.payload[i]=pl[i];
    return f;
}
static bool same(const Frame& a, const Frame& b) {
    return a.type==b.type && a.cmd==b.cmd && a.len==b.len && memcmp(a.payload,b.payload,a.len)==0 &&
           a.addr[0]==b.addr[0] && a.has_addr[1]==b.has_addr[1] && (!a.has_addr[1] || a.addr[1]==b.addr[1]);
}
// frame -> wire bytes (what the UART would recover on RX)
static size_t toWire(const Frame& f, uint8_t* wire, size_t cap) {
    uint8_t logical[MAX_LOGICAL];
    size_t ln = buildLogical(f, logical, sizeof(logical));
    return wireFromLogical(logical, ln, wire, cap);
}

// The firmware's own decoder (rx_stream.cpp), fed `chunk` bytes per RX-task pass.
// Each sync is tagged with rssi = -seq so tests can check frame -> sync attribution.
struct Stream {
    rxs::Decoder d;
    std::vector<Frame> out;
    std::vector<rxs::Item> items;
    void feed(const uint8_t* in, size_t n, size_t chunk) {
        size_t pos = 0;
        while (pos < n) {
            size_t lim = pos + chunk < n ? pos + chunk : n;
            while (pos < lim && d.room()) if (d.put(in[pos++])) d.noteSync((int16_t)-(int)d.syncs, (uint32_t)pos);
            rxs::Item it;
            while (d.next(it)) { out.push_back(it.f); items.push_back(it); }
        }
    }
};

// ---- bit-level model of the radio data pin + ESP32 UART (38400 8N1) ---------
static uint32_t rng = 1;
static int rbit() { rng = rng * 1103515245u + 12345u; return (rng >> 16) & 1; }
static void pushOnAir(std::vector<int>& bits, const Frame& f, int preamble) {
    uint8_t air[MAX_ONAIR]; size_t nb = 0;
    encodeFrame(f, air, sizeof(air), preamble, &nb);
    for (size_t i = 0; i < nb; i++) bits.push_back((air[i >> 3] >> (7 - (i & 7))) & 1);
}
// Edge-triggered UART: start on 1->0, sample 8 data bits LSB-first + stop.
// Bytes with a framing error are still delivered (as the ESP32 FIFO does).
static std::vector<uint8_t> uart(const std::vector<int>& bits) {
    std::vector<uint8_t> out; size_t i = 1;
    while (i + 10 <= bits.size()) {
        if (!(bits[i-1] == 1 && bits[i] == 0)) { i++; continue; }
        uint8_t v = 0; for (int k = 0; k < 8; k++) if (bits[i+1+k]) v |= (uint8_t)(1 << k);
        out.push_back(v);
        i += 10;
    }
    return out;
}

static void test_one(const char* nm, const Frame& f) {
    uint8_t wire[MAX_WIRE]; size_t wn = toWire(f, wire, sizeof(wire));
    // build a noisy stream: junk + preamble + wire + trailing junk
    uint8_t s[400]; size_t n=0;
    uint8_t junk[]={0x12,0xC4,0xAB,0x07,0x55,0x55,0x55};
    for(unsigned i=0;i<sizeof(junk);i++) s[n++]=junk[i];
    memcpy(s+n, wire, wn); n+=wn;
    s[n++]=0x88; s[n++]=0x31; s[n++]=0x44;
    Frame g; size_t consumed=0;
    bool ok = extractWireFrame(s, n, &consumed, g);
    char m[96];
    snprintf(m,sizeof(m),"%s: extracted cmd=%04X len=%u (consumed=%zu)", nm, g.cmd, g.len, consumed);
    CHECK(ok && same(f, g), m);
}

// H1: the 0xFF sync byte is lost for most UART lock phases. Sweep random noise
// prefixes (random UART phase entering the preamble) and require every frame.
static void test_uart_phase_sweep() {
    printf("\n== UART phase sweep (noise -> preamble -> frame) ==\n");
    uint8_t pl[]={0x00,0x01,0x04};
    Frame f = mk(MsgType::I, 0x22F1, pl, 3);
    const int TRIALS = 400;
    int decoded = 0, withoutFF = 0;
    for (int t = 0; t < TRIALS; t++) {
        rng = 1000u + (uint32_t)t;
        std::vector<int> bits;
        int noise = 20 + (int)(rng % 200);
        for (int k = 0; k < noise; k++) bits.push_back(rbit());
        pushOnAir(bits, f, 5);
        for (int k = 0; k < 300; k++) bits.push_back(rbit());
        std::vector<uint8_t> bytes = uart(bits);
        for (size_t k = 1; k + 4 <= bytes.size(); k++)
            if (!memcmp(&bytes[k], &SYNC[1], 4)) { if (bytes[k-1] != 0xFF) withoutFF++; break; }
        Stream st; st.feed(bytes.data(), bytes.size(), 16);
        if (st.out.size() == 1 && same(st.out[0], f)) decoded++;
    }
    char m[120];
    snprintf(m, sizeof(m), "%d/%d trials arrived WITHOUT the 0xFF sync byte (exercises the old failure)", withoutFF, TRIALS);
    CHECK(withoutFF > TRIALS / 2, m);
    snprintf(m, sizeof(m), "%d/%d trials decoded exactly once", decoded, TRIALS);
    CHECK(decoded == TRIALS, m);
}

// H2: a garbled frame (bit error, no valid STOP) right before a good one must
// not swallow the good one; back-to-back frames must all come out, in order.
static void test_consecutive() {
    printf("\n== consecutive / partial frames ==\n");
    uint8_t p1[]={0x00,0x01,0x04}, p2[]={0x00,0x00,0x03};
    Frame a = mk(MsgType::I, 0x22F1, p1, 3), b = mk(MsgType::I, 0x31D9, p2, 3);
    uint8_t wa[MAX_WIRE], wb[MAX_WIRE];
    size_t na = toWire(a, wa, sizeof(wa)), nb = toWire(b, wb, sizeof(wb));

    std::vector<uint8_t> s;
    auto put = [&](const uint8_t* p, size_t n){ s.insert(s.end(), p, p + n); };
    const uint8_t pre[] = {0x55,0x55,0xD5};
    // garbled copy of A: one Manchester byte hit by noise, STOP lost
    std::vector<uint8_t> bad(wa, wa + na); bad[9] = 0x13; bad.pop_back();
    put(pre, 3); put(bad.data(), bad.size());
    put(pre, 3); put(wb, nb);                  // good B
    put(pre, 3); put(wa, na);                  // good A
    put(pre, 3); put(wb, nb);                  // good B again, back to back

    for (size_t chunk : {1, 7, 64, 320}) {
        Stream st; st.feed(s.data(), s.size(), chunk);
        char m[96];
        bool ok = st.out.size() == 3 && same(st.out[0], b) && same(st.out[1], a) && same(st.out[2], b);
        snprintf(m, sizeof(m), "garbled+B+A+B, chunk=%zu -> %zu frames (want B,A,B), resets=%u",
                 chunk, st.out.size(), st.d.resets);
        CHECK(ok && st.d.resets == 0, m);
        // sync 1 is the garbled A: frames must carry syncs 2,3,4 and those syncs' RSSI
        bool tags = st.items.size() == 3;
        for (size_t k = 0; tags && k < 3; k++) tags = st.items[k].seq == k + 2 && st.items[k].rssi == -(int)(k + 2);
        snprintf(m, sizeof(m), "chunk=%zu: each frame tagged with its own sync (seq 2,3,4) and RSSI", chunk);
        CHECK(tags && st.d.syncs == 4, m);
    }

    // checksum error must be rejected, and the next frame still decoded
    std::vector<uint8_t> s2;
    std::vector<uint8_t> cs(wa, wa + na); cs[7] = (cs[7] == MANCH_ENC[1]) ? MANCH_ENC[2] : MANCH_ENC[1];
    s2.insert(s2.end(), cs.begin(), cs.end()); s2.insert(s2.end(), wb, wb + nb);
    Stream st; st.feed(s2.data(), s2.size(), 5);
    CHECK(st.out.size() == 1 && same(st.out[0], b), "bad checksum dropped, following frame kept");
}

int main(){
    printf("Async RX decode-chain tests\n");
    uint8_t p1[]={0x00,0x01,0x07};                 test_one("22F1", mk(MsgType::I,0x22F1,p1,3));
    uint8_t p2[]={0x00,0xC8,0xEF};                 test_one("22F7", mk(MsgType::W,0x22F7,p2,3));
    uint8_t p3[]={0x00,0x00,0x00,0x00,0x01,0x00,0x64,0x00}; test_one("31E0", mk(MsgType::I,0x31E0,p3,8));
    uint8_t p4[]={0x00,0x10,0x40,0x07,0x12,0x34,0x56,0x78,0x07,0xD0,0x07,0xD0,0x09,0xC4}; test_one("31DA-ish", mk(MsgType::I,0x31DA,p4,14));

    // no-frame stream (pure noise) must not false-decode
    uint8_t noise[60]; for(int i=0;i<60;i++) noise[i]=(uint8_t)(i*37+11);
    Frame g; size_t cc=0; CHECK(!extractWireFrame(noise,sizeof(noise),&cc,g), "pure noise -> no frame");

    // long random byte noise through the stream: no false decodes, buffer bounded
    { rng = 7; std::vector<uint8_t> nz(200000); for (auto& b : nz) { b = 0; for (int k = 0; k < 8; k++) b |= (uint8_t)(rbit() << k); }
      Stream st; st.feed(nz.data(), nz.size(), 50);
      char m[96]; snprintf(m, sizeof(m), "200k random bytes -> %zu false frames, %u resets", st.out.size(), st.d.resets);
      CHECK(st.out.empty() && st.d.resets == 0, m); }

    test_uart_phase_sweep();
    test_consecutive();

    printf("\n%s (%d failures)\n", fails?"FAILED":"ALL PASSED", fails);
    return fails?1:0;
}
