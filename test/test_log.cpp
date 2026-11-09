#include "ramses_codec.h"
#include "orcon_frames.h"
#include "orcon_log.h"
#include <cstdio>
#include <cstring>
unsigned long g_ms = 12345; unsigned long millis() { return g_ms; }
using namespace ramses; using namespace orcon;
static int failures=0;
#define CHECK(c,m) do{ if(!(c)){printf("  [FAIL] %s\n",m);failures++;} else printf("  [ ok ] %s\n",m);}while(0)
int main(){
    printf("RF log formatter tests\n");
    Ids id; id.us=DeviceId(0x1D,0x012345); id.fan=DeviceId(0x20,0x02BCDE);
    char buf[96];
    Frame f = make22F1(id, Mode::High);
    rlog::format(f, buf, sizeof(buf));
    printf("  fmt: %s\n", buf);
    CHECK(strstr(buf,"I  --- 29:074565 32:179422 --:------ 22F1 003 000307")!=nullptr,
          "22F1 formats with 2-address + payload");
    // self-broadcast 1298
    Frame c = make1298(id, 639);
    rlog::format(c, buf, sizeof(buf));
    printf("  fmt: %s\n", buf);
    CHECK(strstr(buf,"--:------")!=nullptr && strstr(buf,"1298 003 00027F")!=nullptr,
          "1298 formats self-broadcast with empty addr1");
    // log add + json + clone detect (simulate hearing a real remote->fan 22F1)
    Frame heard = make22F1(id, Mode::Low);   // addr0=29(REM), addr1=32(FAN)
    rlog::add('R', heard, -73);
    rlog::add('T', make31E0(id,50), 0);
    char j[2048]; rlog::toJson(j,sizeof(j));
    CHECK(strstr(j,"\"d\":\"R\"")&&strstr(j,"\"d\":\"T\"")&&strstr(j,"\"r\":-73"), "toJson has RX+TX entries with rssi");
    CHECK(rlog::seen().haveRemote && rlog::seen().remote.cls==0x1D, "clone-detect found the remote id");
    CHECK(rlog::seen().haveFan && rlog::seen().fan.cls==0x20, "clone-detect found the fan id");

    // MVS-15RHBP: remote AND fan are class 29. Roles must follow the messages
    // actually received: I 29:000101 -> 29:000202 22F1 000104, I 29:000202 31D9 000003.
    DeviceId rem(0x1D,101), fan(0x1D,202), ctl(0x25,4242);
    Frame m1; m1.type=MsgType::I; m1.addr[0]=rem; m1.addr[1]=fan; m1.has_addr[2]=false;
    m1.cmd=0x22F1; m1.len=3; m1.payload[0]=0x00; m1.payload[1]=0x01; m1.payload[2]=0x04;
    Frame m2; m2.type=MsgType::I; m2.addr[0]=fan; m2.has_addr[1]=false; m2.addr[2]=fan;
    m2.cmd=0x31D9; m2.len=3; m2.payload[0]=0x00; m2.payload[1]=0x00; m2.payload[2]=0x03;
    Frame m3 = m2; m3.type=MsgType::RQ; m3.addr[0]=ctl; m3.has_addr[1]=true; m3.addr[1]=fan; m3.has_addr[2]=false; m3.cmd=0x31DA; m3.len=1;
    rlog::add('R', m2, -70); rlog::add('R', m1, -60); rlog::add('R', m2, -70); rlog::add('R', m3, -80);
    rlog::format(m1, buf, sizeof(buf)); printf("  fmt: %s\n", buf);
    CHECK(strstr(buf,"29:000101 29:000202 --:------ 22F1 003 000104")!=nullptr, "MVS 22F1 formats as received");
    CHECK(rlog::seen().haveRemote && rlog::seen().remote==rem, "MVS: 22F1 sender 29:000101 is the remote");
    CHECK(rlog::seen().haveFan && rlog::seen().fan==fan, "MVS: 31D9 sender 29:000202 is the fan (not the remote)");

    // Discovery table: every sender with its role; bounded; least recently heard evicted.
    DeviceId nb(20,303), rem2(0x1D,111111);
    Frame n1 = m2; n1.addr[0] = nb; n1.addr[2] = nb;                           // neighbour unit 31D9
    Frame r2 = m1; r2.addr[0] = rem2; r2.addr[1] = nb;                         // neighbour's remote
    g_ms += 1000; rlog::add('R', n1, -75); rlog::add('R', r2, -80);
    const rlog::Device* dv; size_t nd = rlog::devices(&dv);
    auto role = [&](DeviceId id) { for (size_t i = 0; i < nd; i++) if (dv[i].id == id) return (int)dv[i].roles; return -1; };
    CHECK(role(fan) == rlog::ROLE_UNIT && role(nb) == rlog::ROLE_UNIT && role(rem) == rlog::ROLE_REMOTE &&
          role(rem2) == rlog::ROLE_REMOTE && role(ctl) == 0, "devices: two units, two remotes, one RQ sender, roles by message");
    char dj[2048]; rlog::devicesJson(dj, sizeof(dj));
    printf("  devices: %.160s...\n", dj);
    CHECK(dj[0] == '[' && strstr(dj, "\"id\":\"29:000202\",\"role\":\"unit\"") && strstr(dj, "\"id\":\"20:000303\""),
          "devicesJson lists the units and remotes");
    for (uint32_t i = 0; i < 20; i++) { g_ms += 10; Frame x = n1; x.addr[0] = DeviceId(0x14, 1000 + i); x.addr[2] = x.addr[0]; rlog::add('R', x, -90); }
    nd = rlog::devices(&dv);
    bool newest = false, oldest = false;
    for (size_t i = 0; i < nd; i++) { if (dv[i].id == DeviceId(0x14, 1019)) newest = true; if (dv[i].id == DeviceId(0x14, 1000)) oldest = true; }
    CHECK(nd == rlog::MAX_DEVICES && newest && !oldest, "table stays bounded at 12, least recently heard evicted");
    // toJson must never write past cap: sweep every cap with long (31DA) entries in the log
    Frame big = m2; big.cmd = 0x31DA; big.len = 30; for (int i = 0; i < 30; i++) big.payload[i] = (uint8_t)i;
    g_ms = 4000000000UL;                                     // 10-digit timestamps: longest entries
    for (size_t i = 0; i < rlog::CAP; i++) rlog::add('R', big, -127);   // whole ring: long entries
    static char sweep[2400]; int badCap = -1;
    for (size_t cap = 160; cap <= 1600 && badCap < 0; cap++) {
        memset(sweep, 0x5A, sizeof(sweep));
        size_t jn = rlog::toJson(sweep, cap);
        bool ok = jn < cap && sweep[jn] == 0 && sweep[jn - 1] == ']';
        for (size_t i = cap; i < sizeof(sweep); i++) if (sweep[i] != 0x5A) ok = false;
        if (!ok) badCap = (int)cap;
    }
    char m2s[96]; snprintf(m2s, sizeof(m2s), "toJson stays inside cap for every cap 160..1600 (first bad: %d)", badCap);
    CHECK(badCap < 0, m2s);
    const char* v=failures?"FAILED":"ALL PASSED"; const char* pl=failures==1?"":"s";
    printf("\n%s (%d failure%s)\n",v,failures,pl);
    return failures?1:0;
}
