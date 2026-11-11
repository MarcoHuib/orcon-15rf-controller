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
#define RADIOLIB_ERR_NONE 0
#define RADIOLIB_ENCODING_NRZ 0
#define RADIOLIB_SHAPING_0_5 2
static int checks=0, fails=0, failAt=0, calls=0, starts=0, badWrite=-1, rxAfter=0, waits=0;
static void delayMicroseconds(int) { ++waits; }
#define CHECK(x) do { ++checks; if(!(x)) { ++fails; printf("FAIL line %d: %s\n",__LINE__,#x); } } while(0)
static std::array<uint8_t,256> regs;
struct Mod {
    // rxAfter > 0: the chip reports FSRx (0x04) for that many op-mode reads before RX (0x05).
    uint8_t SPIreadRegister(uint8_t a) { if(a==1&&regs[1]==4&&rxAfter>0&&--rxAfter==0) regs[1]=5; return regs[a]; }
    void SPIwriteRegister(uint8_t a,uint8_t v) { if(a!=badWrite) regs[a]=v; }
} g_mod;
static bool fault() { return ++calls==failAt; }
struct Radio {
    int standby() { if(fault()) return -1; regs[1]=1; return 0; }
    int packetMode() { if(fault()) return -1; regs[0x31]=0x40; return 0; }
    int fixedPacketLengthMode(uint8_t n) { if(fault()) return -1; regs[0x30]&=0x7f; regs[0x32]=n; return 0; }
    int setEncoding(int) { if(fault()) return -1; regs[0x30]&=0x9f; return 0; }
    int setCRC(bool) { if(fault()) return -1; regs[0x30]&=0xef; return 0; }
    int setDataShaping(int) { if(fault()) return -1; regs[0xa]&=0x9f; return 0; }
    int setOutputPower(int n) { CHECK(n==10); if(fault()) return -1; regs[9]=0x88; regs[0x4d]=0x84; return 0; }
    int startTransmit(const uint8_t*,size_t n) {
        ++starts; CHECK(n>0&&n<=64); CHECK(regs[0x31]==0x40); CHECK(regs[0x30]==0);
        CHECK(regs[0x25]==0&&regs[0x26]==2&&regs[0x27]==0);
        if(fault()) return -1; regs[1]=3; regs[0x40]=0; return 0;
    }
    int receiveDirect() { if(fault()) return -1; regs[0x31]&=0xbf; regs[0x40]=0x0c; regs[1]=rxAfter?4:5; return 0; }
} g_radio;
static bool g_rxReady=true;
static uint8_t g_op=5;
struct Uart { int available() { return 0; } void read() {} } rxUart;
struct Decoder { int resets=0; void reset() { ++resets; } } g_dec;
'''
tests = r'''
static void reset() {
    regs.fill(0); regs[1]=5; regs[0x40]=0x0c; regs[0x20]=0x12; regs[0x21]=0x34;
    regs[0x25]=0; regs[0x26]=2; regs[0x27]=0x90; regs[0x30]=0x80;
    calls=starts=failAt=waits=rxAfter=0; badWrite=-1; g_rxReady=true;
}
int main() {
    uint8_t data[106]={};
    reset(); const auto before=regs;
    CHECK(!txBegin(nullptr,50)); CHECK(!txBegin(data,0)); CHECK(!txBegin(data,65)); CHECK(!txBegin(data,105)); CHECK(starts==0);
    CHECK(txBegin(data,50)); CHECK(txActive()); CHECK(!txBegin(data,50));
    CHECK(regs[0x20]==0x12&&regs[0x21]==0x34); // LoRa aliases must not corrupt FSK timeout registers
    CHECK(!txDone()); regs[0x3f]=0xff; CHECK(!txDone()); regs[0x3f]=8; CHECK(txDone()); CHECK(txRestoreRx());
    CHECK(!txActive()&&g_rxReady&&g_op==5);
    for(auto a:TX_REGS) CHECK(regs[a]==before[a]);
    // Every config/start failure aborts before subsequent operations; fast restore always runs.
    for(int f=1;f<=8;++f) {
        reset(); failAt=f; CHECK(!txBegin(data,50)); CHECK(txActive());
        CHECK(starts==(f==8?1:0)); failAt=0; CHECK(txRestoreRx()); CHECK(!txActive()&&g_rxReady);
    }
    reset(); badWrite=0x27; CHECK(!txBegin(data,50)); CHECK(starts==0); badWrite=-1; CHECK(txRestoreRx());
    reset(); CHECK(txBegin(data,50)); failAt=calls+2; CHECK(!txRestoreRx()); CHECK(!g_rxReady&&!txActive());
    reset(); CHECK(txBegin(data,50)); badWrite=0x30; CHECK(!txRestoreRx()); CHECK(!g_rxReady&&!txActive());
    CHECK((restoreDiag()>>24)==2 && ((restoreDiag()>>16)&0xff)==0x30);          // which register, for the web layer
    reset(); CHECK(txBegin(data,50)); failAt=calls+1; CHECK(!txRestoreRx()); CHECK(restoreDiag()==0x01000000u);
    reset(); CHECK(txBegin(data,50)); failAt=calls+2; CHECK(!txRestoreRx()); CHECK(restoreDiag()==0x03000000u);
    // FSK RX passes through FSRx: a short transition is waited for, a stuck one fails closed.
    reset(); CHECK(txBegin(data,50)); rxAfter=4; CHECK(txRestoreRx()); CHECK(g_op==5&&g_rxReady&&restoreDiag()==0&&waits>0&&waits<=20);
    reset(); CHECK(txBegin(data,50)); rxAfter=1000; CHECK(!txRestoreRx()); CHECK(!g_rxReady&&restoreDiag()==0x04000004u&&waits==20);
    reset(); CHECK(txBegin(data,50)); CHECK(txRestoreRx()); CHECK(restoreDiag()==0&&waits==0);              // no wait when already RX
    printf("SX1276 register/restore model: %d checks, %d failures\n",checks,fails); return fails?1:0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp)/'sx.cpp'; binary=Path(tmp)/'sx'
    source.write_text(constants+'\n'+shim+'\n'+block+'\n'+tests)
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++17','-Wall','-Wextra','-Werror'] + shlex.split(os.environ.get('CXXFLAGS','')) + [str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
