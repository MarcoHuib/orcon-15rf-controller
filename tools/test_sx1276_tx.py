"""Exercise the actual packet-TX block with a fault-injected register/radio port.
No SPI, serial connection or radio hardware is opened. Constants come from pinned RadioLib.
"""
from pathlib import Path
import os, shlex, subprocess, tempfile

radio = Path('src/ramses_radio.cpp').read_text()
block = radio.split('static bool g_txActive = false, g_savedRx = false;', 1)[1].split('} // namespace sx1276', 1)[0]
block = 'static bool g_txActive = false, g_savedRx = false;' + block
header = Path('.pio/libdeps/heltec_v2_sx1276/RadioLib/src/modules/SX127x/SX127x.h').read_text()
constants = '\n'.join(line for line in header.splitlines() if line.startswith('#define RADIOLIB_SX127X_'))
shim = r'''
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <array>
#include <atomic>
#include <vector>
#define RADIOLIB_ERR_NONE 0
#define RADIOLIB_ENCODING_NRZ 0
#define RADIOLIB_SHAPING_0_5 2
namespace radio { namespace sx1276 { static const size_t TX_MAX_BYTES = 128; } }
using radio::sx1276::TX_MAX_BYTES;
static int checks=0, fails=0, failAt=0, calls=0, starts=0, badWrite=-1, badOnRestore=-1, rxAfter=0, waits=0;
static void delayMicroseconds(int) { ++waits; }
#define CHECK(x) do { ++checks; if(!(x)) { ++fails; printf("FAIL line %d: %s\n",__LINE__,#x); } } while(0)
static std::array<uint8_t,256> regs;
// FIFO model: 64 bytes; while transmitting, every IRQ_FLAGS_2 read drains `drain` bytes to air.
static int fifo=0, maxFifo=0, drain=1, every=1, reads=0, fullSeen=0, sentAir=0; static bool tx=false, overrun=false, stuck=false;
static std::vector<uint8_t> loaded;                      // bytes in the order they entered the FIFO
static uint32_t us=0; static uint32_t micros() { return us += 7; }
struct Mod {
    // rxAfter > 0: the chip reports FSRx (0x04) for that many op-mode reads before RX (0x05).
    uint8_t SPIreadRegister(uint8_t a) {
        if(a==1&&regs[1]==4&&rxAfter>0&&--rxAfter==0) regs[1]=5;
        if(a==0x3f) {
            if(tx&&++reads%every==0) for(int k=0;k<drain&&fifo>0;++k) { --fifo; ++sentAir; }
            if(fifo>=64) ++fullSeen;
            const bool sent=tx&&!stuck&&fifo==0&&sentAir==regs[0x32];
            return uint8_t((fifo>=64?0x80:0)|(fifo==0?0x40:0)|(overrun?0x10:0)|(sent?0x08:0));
        }
        return regs[a];
    }
    void SPIwriteRegister(uint8_t a,uint8_t v) {
        if(a==0x00) { if(fifo>=64) overrun=true; else { ++fifo; loaded.push_back(v); if(fifo>maxFifo) maxFifo=fifo; } return; }
        if(a==0x3f) { if(v&0x10) { fifo=0; overrun=false; } return; }   // FifoOverrun write clears the FIFO
        if(a!=badWrite) regs[a]=v;
    }
} g_mod;
static bool fault() { return ++calls==failAt; }
struct Radio {
    int standby() { if(tx&&badOnRestore>=0) badWrite=badOnRestore; tx=false; if(fault()) return -1; regs[1]=1; return 0; }
    int packetMode() { if(fault()) return -1; regs[0x31]=0x40; return 0; }
    int fixedPacketLengthMode(uint8_t n) { CHECK(n<=64); if(fault()) return -1; regs[0x30]&=0x7f; regs[0x32]=n; return 0; }
    int setEncoding(int) { if(fault()) return -1; regs[0x30]&=0x9f; return 0; }
    int setCRC(bool) { if(fault()) return -1; regs[0x30]&=0xef; return 0; }
    int setDataShaping(int) { if(fault()) return -1; regs[0xa]&=0x9f; return 0; }
    int setOutputPower(int n) { CHECK(n==10); if(fault()) return -1; regs[9]=0x88; regs[0x4d]=0x84; return 0; }
    int startTransmit(const uint8_t* d,size_t n) {   // RadioLib 7.x FSK: <=64 B whole, else 30 B, TX starts
        ++starts; CHECK(n>0&&n<=TX_MAX_BYTES&&n==regs[0x32]); CHECK(regs[0x31]==0x40); CHECK(regs[0x30]==0);
        CHECK(regs[0x25]==0&&regs[0x26]==2&&regs[0x27]==0);
        if(fault()) return -1;
        fifo=0; overrun=false;
        for(size_t i=0;i<(n>64?30:n);++i) g_mod.SPIwriteRegister(0,d[i]);
        tx=true; regs[1]=3; regs[0x40]=0; return 0;
    }
    int receiveDirect() { if(fault()) return -1; regs[0x31]&=0xbf; regs[0x40]=0x0c; regs[1]=rxAfter?4:5; return 0; }
} g_radio;
static bool g_rxReady=true;
static uint8_t g_op=5;
struct Uart { int available() { return 0; } void read() {} } rxUart;
struct Decoder { int resets=0; void reset() { ++resets; } } g_dec;
'''
tests = r'''
static uint8_t data[200];
static void reset() {
    regs.fill(0); regs[1]=5; regs[0x40]=0x0c; regs[0x20]=0x12; regs[0x21]=0x34;
    regs[0x25]=0; regs[0x26]=2; regs[0x27]=0x90; regs[0x30]=0x80; regs[0x32]=0x40;
    calls=starts=failAt=waits=rxAfter=0; badWrite=badOnRestore=-1; g_rxReady=true;
    fifo=maxFifo=sentAir=reads=fullSeen=0; drain=every=1; tx=overrun=stuck=false; loaded.clear(); us=0;
    for(int i=0;i<200;++i) data[i]=uint8_t(i*37+11);
}
static bool airIs(size_t n) { return loaded.size()==n && !memcmp(loaded.data(),data,n) && sentAir==(int)n; }
int main() {
    reset(); const auto before=regs;
    CHECK(!txBegin(nullptr,50)); CHECK(!txBegin(data,0)); CHECK(!txBegin(data,TX_MAX_BYTES+1)); CHECK(starts==0);
    // Short packet (a 53-byte mode command): sent whole, RX restored inside txBegin.
    CHECK(txBegin(data,53)); CHECK(airIs(53)); CHECK(!txActive()&&g_rxReady&&g_op==5);
    CHECK(regs[0x20]==0x12&&regs[0x21]==0x34); // LoRa aliases must not corrupt FSK timeout registers
    CHECK(txDone()&&!txFailed()); CHECK(txRestoreRx()); CHECK(!txDone());   // the outcome is reported once
    for(auto a:TX_REGS) CHECK(regs[a]==before[a]);
    // Long packets (the ~106-byte 1FC9 offer, and the cap): refilled in order, FIFO never over 64.
    for(size_t n: {65u,106u,118u,128u}) {
        reset(); CHECK(txBegin(data,n)); CHECK(airIs(n)); CHECK(maxFifo<=64&&!overrun);
        CHECK(txDone()&&txRestoreRx()&&g_rxReady&&!txActive()); CHECK(regs[0x32]==0x40);   // length register restored
    }
    reset(); every=6; CHECK(txBegin(data,106)); CHECK(airIs(106)&&txDone()&&maxFifo==64&&fullSeen>0&&!overrun);   // realistic: 208 us/byte vs ~35 us/poll, FIFO often full
    // Underrun (the FIFO ran dry mid-packet): failed, never claimed sent; RX still restored.
    reset(); drain=40; CHECK(txBegin(data,106)); CHECK(txFailed()&&!txDone()); CHECK(txRestoreRx()&&g_rxReady&&!txActive());
    // No PacketSent: bounded by airtime, then failed and restored.
    reset(); stuck=true; CHECK(txBegin(data,106)); CHECK(txFailed()); CHECK(us<=106*260+10000+2000); CHECK(g_rxReady&&!txActive());
    // FIFO overrun reported by the chip: failed.
    reset(); overrun=true; regs[0x32]=0; { uint8_t f=g_mod.SPIreadRegister(0x3f); CHECK(f&0x10); } overrun=false;
    // Every config/start failure aborts before subsequent operations; the owner's restore then runs.
    for(int f=1;f<=8;++f) {
        reset(); failAt=f; CHECK(!txBegin(data,106)); CHECK(txActive()); CHECK(loaded.size()==0);
        CHECK(starts==(f==8?1:0)); failAt=0; CHECK(!txDone()&&!txFailed()); CHECK(txRestoreRx()); CHECK(!txActive()&&g_rxReady);
    }
    reset(); badWrite=0x27; CHECK(!txBegin(data,50)); CHECK(starts==0); badWrite=-1; CHECK(txRestoreRx());
    reset(); badWrite=0x32; CHECK(!txBegin(data,106)); CHECK(starts==0); badWrite=-1; CHECK(txRestoreRx());   // long length not accepted
    // Restore failures inside txBegin are reported by txRestoreRx, with the reason.
    reset(); failAt=9; CHECK(txBegin(data,50)); CHECK(txDone()); CHECK(!txRestoreRx()); CHECK(!g_rxReady&&!txActive()&&restoreDiag()==0x01000000u);
    reset(); failAt=10; CHECK(txBegin(data,50)); CHECK(!txRestoreRx()); CHECK(restoreDiag()==0x03000000u);
    reset(); badOnRestore=0x30; CHECK(txBegin(data,50)); CHECK(!txRestoreRx()); CHECK(!g_rxReady&&!txActive());
    CHECK((restoreDiag()>>24)==2 && ((restoreDiag()>>16)&0xff)==0x30);          // which register, for the web layer
    // FSK RX passes through FSRx: a short transition is waited for, a stuck one fails closed.
    reset(); rxAfter=4; CHECK(txBegin(data,50)); CHECK(txRestoreRx()); CHECK(g_op==5&&g_rxReady&&restoreDiag()==0&&waits>0&&waits<=20);
    reset(); rxAfter=1000; CHECK(txBegin(data,50)); CHECK(!txRestoreRx()); CHECK(!g_rxReady&&restoreDiag()==0x04000004u&&waits==20);
    reset(); CHECK(txBegin(data,50)); CHECK(txRestoreRx()); CHECK(restoreDiag()==0&&waits==0);              // no wait when already RX
    printf("SX1276 register/FIFO/restore model: %d checks, %d failures\n",checks,fails); return fails?1:0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp)/'sx.cpp'; binary=Path(tmp)/'sx'
    source.write_text(constants+'\n'+shim+'\n'+block+'\n'+tests)
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++17','-Wall','-Wextra','-Werror'] + shlex.split(os.environ.get('CXXFLAGS','')) + [str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
