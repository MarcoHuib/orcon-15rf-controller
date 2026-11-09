// Phase 1 invariants, golden vectors and fault/concurrency tests. No RF dependencies.
#include "tx_poc.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <thread>
#include <atomic>
using namespace txpoc;
using namespace ramses;
static int fails=0,checks=0;
#define CHECK(c) do { ++checks; if(!(c)) { ++fails; printf("FAIL line %d: %s\n",__LINE__,#c); } } while(0)
static orcon::Ids ids() { orcon::Ids v; v.us=DeviceId(29,101); v.fan=DeviceId(29,202); return v; }
static const DeviceId REMOTE(29,404), NEIGHBOUR_UNIT(29,303);
static Input input(uint32_t request=1,uint8_t mode=1) { Input v;v.request=request;v.mode=mode;return v; }
static Request req(uint32_t now=1000) { Request v;v.ids=ids();v.input=input();v.expires=now+QUEUE_MS;return v; }
static Frame statusFrame(DeviceId from,uint8_t mode) {
    Frame f;f.type=MsgType::I;f.addr[0]=from;f.has_addr[1]=false;f.has_addr[2]=true;f.addr[2]=from;
    f.cmd=0x31D9;f.len=3;f.payload[2]=mode;return f;
}
// Physical remote -> unit. viaAddr2: destination in the third address slot (orcon::toUnit).
static Frame remoteFrame(uint8_t mode,bool viaAddr2=false,uint16_t cmd=0x22F1,DeviceId to=ids().fan) {
    Frame f;f.type=MsgType::I;f.addr[0]=REMOTE;
    if(viaAddr2){f.has_addr[1]=false;f.has_addr[2]=true;f.addr[2]=to;} else {f.has_addr[1]=true;f.addr[1]=to;f.has_addr[2]=false;}
    f.cmd=cmd;f.len=3;f.payload[1]=mode;f.payload[2]=4;return f;
}
static void see(Controller& c,const Frame& f,uint32_t at,uint32_t now=0) { c.onFrame(f,at,now?now:at,ids()); }
// Remote press at t, unit status `mode` 40 ms later: what a real press looks like on air.
static void prime(Controller& c,uint32_t t,uint8_t mode=1) { see(c,remoteFrame(mode),t); see(c,statusFrame(ids().fan,mode),t+40); }
static Admission sub(Controller& c,Input in,uint32_t now,bool ready=true,bool clear=true) {
    return c.submit(in,ids(),true,ready,clear,now);
}
static const uint32_t T0=1000, ADMIT=T0+REMOTE_GUARD_MS+100;   // first admissible time after prime(T0)

static void golden() {
    for(uint8_t mode=1;mode<=3;++mode) {
        Frame f; CHECK(makeMode04(ids(),mode,f));
        uint8_t expected[]={0x1C,0x74,0,0x65,0x74,0,0xCA,0x22,0xF1,3,0,mode,4,(uint8_t)(0xB3-mode)};
        uint8_t logical[96];size_t n=buildLogical(f,logical,sizeof(logical));
        CHECK(n==sizeof(expected)&&!memcmp(logical,expected,n));
        uint8_t wireExpected[]={0xFF,0,0x33,0x55,0x53,0xA9,0x5A,0x95,0x9A,0xAA,0xAA,0x96,0x99,
            0x95,0x9A,0xAA,0xAA,0x5A,0x66,0xA6,0xA6,0x55,0xA9,0xAA,0xA5,0xAA,0xAA,
            0xAA,(uint8_t)(mode==1?0xA9:mode==2?0xA6:0xA5),0xAA,0x9A,0x65,
            (uint8_t)(mode==1?0xA6:mode==2?0xA9:0xAA),0x35};
        uint8_t wire[64];CHECK(wireFromLogical(logical,n,wire,sizeof(wire))==sizeof(wireExpected));
        CHECK(!memcmp(wire,wireExpected,sizeof(wireExpected)));
        // Independent UART bit assembly from literal wire vector, not our codec/table.
        std::vector<int> bits;
        auto uart=[&](uint8_t b){bits.push_back(0);for(int k=0;k<8;++k)bits.push_back((b>>k)&1);bits.push_back(1);};
        for(int j=0;j<6;++j)uart(0x55);
        for(uint8_t b:wireExpected)uart(b);
        CHECK(bits.size()==400);
        uint8_t expectedPacked[50]={};
        for(size_t j=0;j<bits.size();++j) expectedPacked[j/8]|=(uint8_t)(bits[j]<<(7-j%8));
        uint8_t packed[64];size_t bitlen=0;
        CHECK(encodeFrame(f,packed,sizeof(packed),6,&bitlen)==50&&bitlen==400);
        CHECK(!memcmp(expectedPacked,packed,50));
        Frame legacy=orcon::make22F1(ids(),(orcon::Mode)mode);CHECK(legacy.payload[2]==7);
    }
    Frame f;CHECK(!makeMode04(ids(),0,f)&&!makeMode04(ids(),4,f));
    orcon::Ids bad=ids();bad.fan=DeviceId(63,0x3FFFF);CHECK(!makeMode04(bad,1,f));
    bad=ids();bad.fan.serial=0;CHECK(!validIds(bad));
    bad=ids();bad.us.serial=0x40000;CHECK(!validIds(bad));
    bad=ids();bad.us.cls=64;CHECK(!validIds(bad));
    bad=ids();bad.us=bad.fan;CHECK(!validIds(bad));
}
static void schema() {
    Input v;
    const char* good[]={"{\"mode\":1,\"request\":1}"," { \"request\" : 42,\"mode\":3 }\n"};
    for(auto s:good)CHECK(parseInput(s,strlen(s),v));
    const char* bad[]={"", "{}", "[]", "null", "{\"mode\":1}","{\"mode\":0,\"request\":1}",
        "{\"mode\":4,\"request\":1}","{\"mode\":true,\"request\":1}","{\"mode\":\"1\",\"request\":1}",
        "{\"mode\":1.0,\"request\":1}","{\"mode\":-1,\"request\":1}","{\"mode\":01,\"request\":1}",
        "{\"mode\":1,\"request\":0}","{\"mode\":1,\"request\":4294967296}",
        "{\"mode\":1,\"mode\":2}","{\"mode\":1,\"request\":1,\"raw\":1}",
        "{\"mode\":1,\"request\":1}x","{\"m\\u006fde\":1,\"request\":1}",
        "{\"mode\":1,\"request\":1,}","{\"mode\":1,\"request\":1"};
    for(auto s:bad)CHECK(!parseInput(s,strlen(s),v));
    char longInput[98];memset(longInput,' ',sizeof(longInput));CHECK(!parseInput(longInput,sizeof(longInput),v));
    CHECK(!parseInput(nullptr,1,v));
    const char embedded[]="{\"mode\":1,\"request\":1}\0x";CHECK(!parseInput(embedded,sizeof(embedded)-1,v));
}
struct Fake : Backend {
    Error fault=Error::None;
    bool pending=false,fast=true;
    int prepares=0,starts=0,cleanups=0,restores=0;
    bool prepare(const Request&,size_t& n) override {++prepares;n=50;return fault!=Error::Prepare;}
    bool start(uint32_t) override {++starts;return fault!=Error::Start;}
    Completion poll(uint32_t) override {return pending?Completion::Pending:fault==Error::Send?Completion::Failed:Completion::Complete;}
    bool cleanup() override {++cleanups;return fault!=Error::Cleanup;}
    bool restoreRx() override {++restores;return fast;}
};
static Result runFault(Fake& fake,Error expected,bool healthy=true,uint32_t start=1000) {
    Exchange ex; Owner owner(ex,fake); CHECK(ex.requests.push(req(start)));
    for(uint32_t n=0;n<250;n+=5)owner.tick(start+n,healthy);
    Result result;CHECK(ex.results.pop(result));CHECK(result.error==expected);CHECK(!result.rfSent);
    const int work=result.touched?1:0;               // cleanup/restore only after radio work began
    CHECK(fake.cleanups==work&&fake.restores==work&&fake.starts<=1);
    for(int n=0;n<1000;++n)owner.tick(start+300+n,true);
    CHECK(fake.cleanups==work&&fake.starts<=1);      // no retries or unsolicited work
    return result;
}
static void faults() {
    for(Error e:{Error::None,Error::Prepare,Error::Start,Error::Send,Error::Cleanup}) {
        Fake f;f.fault=e;Result r=runFault(f,e);CHECK(r.rxRestored&&r.touched);
    }
    Fake timeout;timeout.pending=true;CHECK(runFault(timeout,Error::SendTimeout).rxRestored);
    // Failed fast restore: no second recovery loop in the owner; published, not retried.
    Fake failed;failed.fast=false;Result r=runFault(failed,Error::RxRestore);CHECK(!r.rxRestored&&failed.restores==1);
    // First error wins: a send failure followed by a failed restore stays a send failure.
    Fake both;both.fault=Error::Send;both.fast=false;r=runFault(both,Error::Send);CHECK(!r.rxRestored&&r.touched);
    Fake unavailable;r=runFault(unavailable,Error::RadioUnavailable,false);CHECK(unavailable.starts==0&&!r.touched);
    Fake wrap;runFault(wrap,Error::None,true,0xFFFFFFD0u);
    Exchange ex; Fake f;Owner owner(ex,f);
    CHECK(ex.requests.push(req(1000)));owner.tick(1500,true);
    for(int t=1505;t<1700;t+=5)owner.tick(t,true);
    CHECK(ex.results.pop(r)&&r.error==Error::Expired&&f.starts==0&&f.cleanups==0&&!r.touched);
    // A stalled consumer must not block the radio task or lose its completion.
    Exchange held;Fake hf;Owner ho(held,hf);Result sentinel;sentinel.bytes=17;
    CHECK(held.results.push(sentinel)&&held.requests.push(req()));
    for(int t=1000;t<1200;t+=5)ho.tick(t,true);
    CHECK(ho.step()==Step::Publish&&hf.starts==1);
    CHECK(held.results.pop(r)&&r.bytes==17);ho.tick(1200,true);
    CHECK(held.results.pop(r)&&r.bytes==50&&ho.step()==Step::Idle);
}
static void ownerTrace() {
    Exchange ex;Fake f;Owner owner(ex,f);CHECK(ex.requests.push(req()));
    CHECK(owner.step()==Step::Idle);owner.tick(1000,true);CHECK(owner.step()==Step::Prepare);
    owner.tick(1005,true);CHECK(owner.step()==Step::Simulate);
    owner.tick(1010,true);CHECK(owner.step()==Step::Cleanup);
    owner.tick(1015,true);CHECK(owner.step()==Step::Restore);
    owner.tick(1020,true);CHECK(owner.step()==Step::Publish);
    owner.tick(1025,true);CHECK(owner.step()==Step::Idle);
    Result result;CHECK(ex.results.pop(result)&&result.rxRestored&&!result.rfSent&&result.touched);
    CHECK(f.prepares==1&&f.starts==1&&f.cleanups==1&&f.restores==1);
    // Expiration between dequeue and preparation still forbids starting work.
    Exchange late;Fake lf;Owner lo(late,lf);CHECK(late.requests.push(req()));
    lo.tick(1499,true);lo.tick(1500,true);
    for(int t=1505;t<1600;t+=5)lo.tick(t,true);
    CHECK(late.results.pop(result)&&result.error==Error::Expired&&lf.starts==0&&lf.cleanups==0);
    // Health lost between dequeue and preparation: untouched, nothing to clean up.
    Exchange gone;Fake gf;Owner go(gone,gf);CHECK(gone.requests.push(req()));
    go.tick(1000,true);go.tick(1005,false);go.tick(1010,false);
    CHECK(gone.results.pop(result)&&result.error==Error::RadioUnavailable&&!result.touched&&gf.prepares==0&&gf.restores==0);
    // Failed restore after our radio work is a latched admission fault even if a caller says ready.
    Exchange failed;Fake ff;ff.fast=false;Owner fo(failed,ff);Controller c(failed);prime(c,T0);
    CHECK(sub(c,input(1,2),ADMIT)==Admission::Accepted);
    for(uint32_t t=ADMIT;t<ADMIT+200;t+=5){fo.tick(t,true);c.tick(t);}
    CHECK(c.status().locked&&c.status().result.error==Error::RxRestore);
    prime(c,ADMIT+20000);
    CHECK(sub(c,input(2,2),ADMIT+30000)==Admission::Unavailable);
    CHECK(sub(c,input(1,2),ADMIT+30000)==Admission::Duplicate);
}
// The real dry-run backend through the real owner and controller (not only the Fake).
static void dryRunFaults() {
    {   // RX unhealthy when the owner takes the request: untouched, first error kept, no lock.
        Exchange ex;Controller c(ex);DryRunBackend dry;Owner o(ex,dry);prime(c,T0);
        CHECK(sub(c,input(1,2),ADMIT)==Admission::Accepted);
        for(uint32_t t=ADMIT;t<ADMIT+200;t+=5){dry.setRxHealthy(false);o.tick(t,false);c.tick(t);}
        const Status& s=c.status();
        CHECK(s.phase==Phase::Failed&&s.result.error==Error::RadioUnavailable&&!s.result.touched);
        CHECK(!s.locked&&!s.busy&&!s.result.rxRestored);
        CHECK(sub(c,input(2,2),ADMIT+RATE_MS,false)==Admission::Unavailable); // still blocked while unhealthy
    }
    {   // Health lost after the dry-run "transmission": touched, rx-restore-failed, locked.
        Exchange ex;Controller c(ex);DryRunBackend dry;Owner o(ex,dry);prime(c,T0);
        CHECK(sub(c,input(1,2),ADMIT)==Admission::Accepted);
        uint32_t t=ADMIT;
        for(;o.step()!=Step::Cleanup&&t<ADMIT+200;t+=5){dry.setRxHealthy(true);o.tick(t,true);c.tick(t);}
        CHECK(o.step()==Step::Cleanup);
        for(;t<ADMIT+300;t+=5){dry.setRxHealthy(false);o.tick(t,false);c.tick(t);}
        const Status& s=c.status();
        CHECK(s.phase==Phase::Failed&&s.result.error==Error::RxRestore&&s.result.touched&&s.locked&&!s.busy);
    }
    {   // Healthy: dry-run completes, RX observed, nothing sent, no confirmation window.
        Exchange ex;Controller c(ex);DryRunBackend dry;dry.setRxHealthy(true);Owner o(ex,dry);prime(c,T0);
        CHECK(sub(c,input(1,2),ADMIT)==Admission::Accepted);
        for(uint32_t t=ADMIT;t<ADMIT+200;t+=5){o.tick(t,true);c.tick(t);}
        const Status& s=c.status();
        CHECK(s.phase==Phase::DryRunComplete&&s.result.touched&&s.result.rxRestored&&!s.result.rfSent&&s.result.bytes==50);
        see(c,statusFrame(ids().fan,2),ADMIT+300);CHECK(c.status().phase==Phase::DryRunComplete);
    }
}
static void transactions() {
    Exchange ex;Controller c(ex);DryRunBackend dry;dry.setRxHealthy(true);Owner owner(ex,dry);
    CHECK(c.submit(input(),ids(),false,true,true,1000)==Admission::UnknownUnit);
    CHECK(sub(c,input(),1000,false)==Admission::Unavailable);
    CHECK(sub(c,input(1,2),1000)==Admission::IdentityUnverified);      // no physical remote heard yet
    see(c,remoteFrame(1),T0);
    CHECK(sub(c,input(1,2),ADMIT)==Admission::UnitStateUnknown);        // no unit status yet
    see(c,statusFrame(ids().fan,1),T0+40);
    CHECK(sub(c,input(1,1),ADMIT)==Admission::Unchanged);               // unit already reports stand 1
    CHECK(sub(c,input(1,2),T0+REMOTE_GUARD_MS-1)==Admission::RemoteActive);
    CHECK(sub(c,input(1,2),T0+REMOTE_GUARD_MS)==Admission::Accepted);
    const uint32_t A=T0+REMOTE_GUARD_MS;
    CHECK(c.status().result.request.prior==1);
    CHECK(sub(c,input(1,2),A+1)==Admission::Duplicate);
    CHECK(sub(c,input(1,3),A+1)==Admission::Conflict);
    CHECK(sub(c,input(2,3),A+1)==Admission::Busy);
    orcon::Ids changed=ids();changed.fan.serial=303;
    CHECK(c.submit(input(1,2),changed,true,true,true,A+1)==Admission::Conflict);
    for(uint32_t t=A;t<A+100;t+=5){owner.tick(t,true);c.tick(t);}
    CHECK(c.status().phase==Phase::DryRunComplete&&!c.status().busy);
    CHECK(!c.status().result.rfSent&&c.status().result.rxRestored&&c.status().result.bytes==50);
    CHECK(c.status().result.request.ids.fan==ids().fan);
    CHECK(sub(c,input(2,3),A+100)==Admission::Limited);
    see(c,statusFrame(ids().fan,1),A+RATE_MS-10);                      // keep the unit state fresh
    CHECK(sub(c,input(3,3),A+RATE_MS)==Admission::Replay);
    CHECK(sub(c,input(2,3),A+RATE_MS)==Admission::Accepted);
    for(uint32_t t=A+RATE_MS;t<A+RATE_MS+100;t+=5){owner.tick(t,true);c.tick(t);}
    CHECK(sub(c,input(1,3),A+2*RATE_MS)==Admission::Replay);
    // Stale unit state: older than UNIT_STATE_MAX_AGE_MS refuses admission.
    Exchange se;Controller sc(se);prime(sc,T0);
    CHECK(sub(sc,input(1,2),T0+40+UNIT_STATE_MAX_AGE_MS-1)==Admission::Accepted);
    Exchange so;Controller oc(so);prime(oc,T0);
    CHECK(sub(oc,input(1,2),T0+40+UNIT_STATE_MAX_AGE_MS)==Admission::UnitStateUnknown);
    // Boot and 25 simulated idle hours never manufacture a request or result.
    Exchange quiet;Controller qc(quiet);Fake qf;Owner qo(quiet,qf);
    for(uint32_t t=0;t<25u*3600000u;t+=1000){qo.tick(t,true);qc.tick(t);}
    CHECK(qf.starts==0&&qc.status().phase==Phase::Idle);
    // Owner timeout retains admission reservation until expired request is cleaned.
    Exchange late;Controller lc(late);Fake lf;Owner lo(late,lf);prime(lc,T0);
    CHECK(sub(lc,input(1,2),ADMIT)==Admission::Accepted);lc.tick(ADMIT+TRANSACTION_MS);
    CHECK(lc.status().phase==Phase::Failed&&lc.status().busy);
    prime(lc,ADMIT+20000);
    CHECK(sub(lc,input(2,2),ADMIT+30000)==Admission::Busy);
    for(uint32_t t=ADMIT+30000;t<ADMIT+30100;t+=5){lo.tick(t,true);lc.tick(t);}
    CHECK(lf.starts==0&&!lc.status().busy&&lc.status().result.error==Error::Expired&&lc.status().locked);
    // Queue is strictly bounded, never overwritten.
    Exchange full;CHECK(full.requests.push(req()));Controller fc(full);prime(fc,T0);
    CHECK(sub(fc,input(1,2),ADMIT)==Admission::Busy);
    Request r;CHECK(full.requests.pop(r)&&r.ids.fan==ids().fan);
}
// A future RF result as the owner would publish it: sent at ADMIT+10, RX back at ADMIT+30.
static const uint32_t SENT=ADMIT+10, RESTORED=ADMIT+30, LAST=SENT+CONFIRM_MS-1;
static Request admit(Exchange& ex,Controller& c,uint8_t prior=1,uint8_t mode=2) {
    prime(c,T0,prior);
    CHECK(sub(c,input(1,mode),ADMIT)==Admission::Accepted);
    Request r;CHECK(ex.requests.pop(r));CHECK(r.prior==prior);return r;
}
static void publishSent(Exchange& ex,Controller& c,const Request& r) {
    Result s;s.request=r;s.touched=s.rfSent=s.rxRestored=true;s.sentMs=SENT;s.restoredMs=RESTORED;
    CHECK(ex.results.push(s));c.tick(RESTORED+10);
}
static void armSent(Exchange& ex,Controller& c,uint8_t prior=1,uint8_t mode=2) {
    publishSent(ex,c,admit(ex,c,prior,mode));CHECK(c.status().phase==Phase::AwaitUnit);
}
static Phase outcome(void (*scenario)(Controller&)) {
    Exchange ex;Controller c(ex);armSent(ex,c);scenario(c);return c.status().phase;
}
static void correlation() {
    // Baseline: the unit reports the requested stand inside the window.
    CHECK(outcome([](Controller& c){see(c,statusFrame(ids().fan,2),RESTORED+100);})==Phase::UnitReported);
    // A: remote pressed 300 ms before our TX; its frame is drained after the result.
    CHECK(outcome([](Controller& c){see(c,remoteFrame(2),SENT-300,RESTORED+50);
        see(c,statusFrame(ids().fan,2),RESTORED+100);})==Phase::Ambiguous);
    {   // A': the same remote frame is drained while the transaction is still queued.
        Exchange ex;Controller c(ex);Request r=admit(ex,c);see(c,remoteFrame(2),SENT-300,SENT);
        publishSent(ex,c,r);CHECK(c.status().phase==Phase::Ambiguous);
    }
    // Remote exactly at the guard boundary counts; one ms earlier does not.
    CHECK(outcome([](Controller& c){see(c,remoteFrame(2),SENT-REMOTE_GUARD_MS,RESTORED+50);
        see(c,statusFrame(ids().fan,2),RESTORED+100);})==Phase::Ambiguous);
    {   // Older than the guard: the press that set the prior state, not interference.
        Exchange ex;Controller c(ex);prime(c,T0-1,1);
        CHECK(sub(c,input(1,2),ADMIT)==Admission::Accepted);Request r;CHECK(ex.requests.pop(r));
        Result s;s.request=r;s.touched=s.rfSent=s.rxRestored=true;s.sentMs=T0-1+REMOTE_GUARD_MS+1;s.restoredMs=s.sentMs+20;
        CHECK(ex.results.push(s));c.tick(s.restoredMs+10);CHECK(c.status().phase==Phase::AwaitUnit);
        see(c,statusFrame(ids().fan,2),s.restoredMs+50);CHECK(c.status().phase==Phase::UnitReported);
    }
    // B: remote addressed to the unit through the third address slot.
    CHECK(outcome([](Controller& c){see(c,remoteFrame(2,true),RESTORED+50);
        see(c,statusFrame(ids().fan,2),RESTORED+100);})==Phase::Ambiguous);
    // C: timed boost 22F3 from a remote during the window.
    CHECK(outcome([](Controller& c){see(c,remoteFrame(2,false,0x22F3),RESTORED+50);
        see(c,statusFrame(ids().fan,2),RESTORED+100);})==Phase::Ambiguous);
    // Remote of another unit (neighbour) is not interference; an RP 22F1 is not a command.
    CHECK(outcome([](Controller& c){see(c,remoteFrame(2,false,0x22F1,NEIGHBOUR_UNIT),RESTORED+50);
        Frame rp=remoteFrame(2);rp.type=MsgType::RP;see(c,rp,RESTORED+60);
        see(c,statusFrame(ids().fan,2),RESTORED+100);})==Phase::UnitReported);
    // Untimed remote action cannot be placed in time: counted as now, so ambiguous.
    CHECK(outcome([](Controller& c){c.onFrame(remoteFrame(2),0,RESTORED+50,ids());
        see(c,statusFrame(ids().fan,2),RESTORED+100);})==Phase::Ambiguous);
    // D: unit already in the requested stand is refused before any transmission...
    {   Exchange ex;Controller c(ex);prime(c,T0,2);CHECK(sub(c,input(1,2),ADMIT)==Admission::Unchanged);
        CHECK(!ex.requests.full()&&!c.status().busy);
    }
    // ...and a late (+9.9 s) status is outside the 3 s window: not confirmed.
    {   Exchange ex;Controller c(ex);armSent(ex,c);see(c,statusFrame(ids().fan,2),SENT+9900);
        CHECK(c.status().phase==Phase::AwaitUnit);c.tick(SENT+9900);CHECK(c.status().phase==Phase::NotConfirmed);
    }
    // Unit keeps reporting its prior stand: unchanged. Any third stand: ambiguous.
    CHECK(outcome([](Controller& c){see(c,statusFrame(ids().fan,1),RESTORED+100);})==Phase::Unchanged);
    CHECK(outcome([](Controller& c){see(c,statusFrame(ids().fan,3),RESTORED+100);})==Phase::Ambiguous);
    // Window edges: before send/restore and at the deadline are ignored; LAST still counts.
    CHECK(outcome([](Controller& c){see(c,statusFrame(ids().fan,2),SENT-1,RESTORED+50);
        see(c,statusFrame(ids().fan,2),SENT+5,RESTORED+50);see(c,statusFrame(ids().fan,2),SENT+CONFIRM_MS);})==Phase::AwaitUnit);
    CHECK(outcome([](Controller& c){see(c,statusFrame(ids().fan,2),LAST);})==Phase::UnitReported);
    // Missing or future receive time is never evidence.
    CHECK(outcome([](Controller& c){c.onFrame(statusFrame(ids().fan,2),0,RESTORED+100,ids());
        c.onFrame(statusFrame(ids().fan,2),RESTORED+200,RESTORED+100,ids());})==Phase::AwaitUnit);
    // Neighbour unit, wrong verb, nonzero domain byte: ignored.
    CHECK(outcome([](Controller& c){see(c,statusFrame(NEIGHBOUR_UNIT,2),RESTORED+100);
        Frame w=statusFrame(ids().fan,2);w.type=MsgType::W;see(c,w,RESTORED+110);
        Frame d=statusFrame(ids().fan,2);d.payload[0]=1;see(c,d,RESTORED+120);})==Phase::AwaitUnit);
    {   // Expiry: only after deadline + grace, so a timely frame drained late still counts.
        Exchange ex;Controller c(ex);armSent(ex,c);c.tick(SENT+CONFIRM_MS);CHECK(c.status().phase==Phase::AwaitUnit);
        see(c,statusFrame(ids().fan,2),LAST,SENT+CONFIRM_MS+100);CHECK(c.status().phase==Phase::UnitReported);
        Exchange e2;Controller c2(e2);armSent(e2,c2);c2.tick(SENT+CONFIRM_MS+EXPIRY_GRACE_MS-1);
        CHECK(c2.status().phase==Phase::AwaitUnit);
        c2.tick(SENT+CONFIRM_MS+EXPIRY_GRACE_MS);CHECK(c2.status().phase==Phase::NotConfirmed&&!c2.status().busy);
    }
    {   // A timely received frame remains evidence even after a long main-loop stall.
        Exchange ex;Controller c(ex);Request r=admit(ex,c);Result s;s.request=r;s.touched=s.rfSent=s.rxRestored=true;
        s.sentMs=SENT;s.restoredMs=RESTORED;CHECK(ex.results.push(s));
        c.consumeResult();see(c,statusFrame(ids().fan,2),RESTORED+100,SENT+20000);
        c.tick(SENT+20000);CHECK(c.status().phase==Phase::UnitReported);
    }
    {   // After a terminal outcome later frames change nothing.
        Exchange ex;Controller c(ex);armSent(ex,c);see(c,statusFrame(ids().fan,2),RESTORED+100);
        see(c,remoteFrame(3),RESTORED+200);see(c,statusFrame(ids().fan,3),RESTORED+300);
        CHECK(c.status().phase==Phase::UnitReported);
    }
    {   // Dry-run completion must never open a confirmation window.
        Exchange ex;Controller c(ex);Request r=admit(ex,c);Result s;s.request=r;s.touched=s.rxRestored=true;
        CHECK(ex.results.push(s));c.tick(RESTORED);see(c,statusFrame(ids().fan,2),RESTORED+50);
        CHECK(c.status().phase==Phase::DryRunComplete);
    }
    {   // Prior-state tracking keeps the newest receive time, even if frames arrive out of order.
        Exchange ex;Controller c(ex);prime(c,T0,1);see(c,statusFrame(ids().fan,3),T0+20,T0+100);
        CHECK(c.status().unitMode==1&&c.status().unitMs==T0+40);
    }
}
static void identity() {
    rlog::Seen seen; rlog::Device devs[2]; devs[0].id=REMOTE; devs[1].id=NEIGHBOUR_UNIT;
    orcon::Ids v=ids();
    CHECK(!identityConflict(v,seen,devs,2));
    orcon::Ids clone=ids();clone.us=REMOTE;CHECK(identityConflict(clone,seen,devs,2));          // heard sender
    clone.us=NEIGHBOUR_UNIT;CHECK(identityConflict(clone,seen,devs,2));
    seen.haveRemote=true;seen.remote=DeviceId(29,505);clone.us=seen.remote;CHECK(identityConflict(clone,seen,devs,0));
    seen.haveFan=true;seen.fan=DeviceId(29,606);clone.us=seen.fan;CHECK(identityConflict(clone,seen,nullptr,0));
    // Admission: a conflict refuses; the controller also refuses a remote that uses our source.
    Exchange ex;Controller c(ex);prime(c,T0);
    CHECK(sub(c,input(1,2),ADMIT,true,false)==Admission::IdentityConflict);
    Exchange e2;Controller c2(e2);Frame f=remoteFrame(1);f.addr[0]=ids().us;see(c2,f,T0);
    see(c2,statusFrame(ids().fan,1),T0+40);CHECK(sub(c2,input(1,2),ADMIT)==Admission::IdentityConflict);
    // Unknown identity: no remote for this unit heard this boot (only a neighbour's).
    Exchange e3;Controller c3(e3);see(c3,remoteFrame(1,false,0x22F1,NEIGHBOUR_UNIT),T0);
    see(c3,statusFrame(ids().fan,1),T0+40);CHECK(sub(c3,input(1,2),ADMIT)==Admission::IdentityUnverified);
    CHECK(!e3.requests.full());
}
static void health() {
    CHECK(!healthReady(0,0)&&!healthReady(0,100));                    // never published
    CHECK(healthReady(packHealth(1000,true),1000));
    CHECK(healthReady(packHealth(1000,true),1000+HEALTH_MAX_AGE_MS-1));
    CHECK(!healthReady(packHealth(1000,true),1000+HEALTH_MAX_AGE_MS)); // stale heartbeat
    CHECK(!healthReady(packHealth(1000,false),1000));                 // published unhealthy
    CHECK(healthReady(packHealth(0xFFFFFFF0u,true),20));               // millis wrap
    CHECK(!healthReady(packHealth(1002,true),1000));                  // "future" beat: fail closed
    // One publisher, one reader: the bit always belongs to its own heartbeat (no tearing).
    std::atomic<uint32_t> word{0};std::atomic<bool> bad{false},done{false};
    std::thread pub([&](){for(uint32_t t=2;t<400000;t+=2)word.store(packHealth(t,(t/2)%3!=0),std::memory_order_release);done=true;});
    uint32_t last=0;
    while(!done.load()){uint32_t w=word.load(std::memory_order_acquire);if(!w)continue;uint32_t t=w&~1u;
        if(t<last||(bool)(w&1u)!=((t/2)%3!=0))bad=true;last=t;}
    pub.join();CHECK(!bad);
}
static void security() {
    const char* token="0123456789abcdef0123456789abcdef";
    const char* csrf="fedcba9876543210fedcba9876543210";
    Access a;a.station=a.host=a.origin=a.json=true;
    Security s;CHECK(s.check(token,csrf,a,true,0)==AccessResult::Unauthorized);
    CHECK(!s.begin("short",csrf)&&!s.begin(token,token)&&s.begin(token,csrf));
    CHECK(s.check(token,csrf,a,true,1000)==AccessResult::Allowed);
    CHECK(s.check(token,"",a,true,1000)==AccessResult::Forbidden);
    CHECK(s.check(token,"",a,false,1000)==AccessResult::Allowed);
    CHECK(s.check("",csrf,a,false,1000)==AccessResult::Unauthorized);  // missing Authorization
    CHECK(s.check(nullptr,nullptr,a,true,1000)==AccessResult::Unauthorized);
    CHECK(s.check(token,"fedcba9876543210fedcba987654321F",a,true,1000)==AccessResult::Forbidden);
    for(int k=0;k<4;++k){Access bad=a;if(k==0)bad.station=false;if(k==1)bad.host=false;if(k==2)bad.origin=false;if(k==3)bad.json=false;
        CHECK(s.check(token,csrf,bad,true,1000)==AccessResult::Forbidden);}
    Access getOnly=a;getOnly.origin=getOnly.json=false;CHECK(s.check(token,"",getOnly,false,1000)==AccessResult::Allowed);
    getOnly.host=false;CHECK(s.check(token,"",getOnly,false,1000)==AccessResult::Forbidden);
    for(int k=0;k<8;++k)CHECK(s.check("invalid",csrf,a,true,2000)==AccessResult::Unauthorized);
    CHECK(s.check(token,csrf,a,true,2001)==AccessResult::Limited);
    CHECK(s.check(token,csrf,a,true,3000)==AccessResult::Allowed);
    CHECK(s.begin(token,csrf,1000));
    CHECK(s.check(token,csrf,a,true,600999)==AccessResult::Allowed);
    CHECK(s.check(token,csrf,a,true,601000)==AccessResult::Unauthorized);
    CHECK(s.begin(token,csrf,0xFFFFF000u));
    CHECK(s.check(token,csrf,a,true,0xFFFFF100u)==AccessResult::Allowed);
    CHECK(s.check(token,csrf,a,true,uint32_t(0xFFFFF000u+600000u))==AccessResult::Unauthorized);
    // Expiry remains latched even when the 32-bit clock later wraps.
    CHECK(s.check(token,csrf,a,true,0xFFFFF001u)==AccessResult::Unauthorized);
    CHECK(!s.begin("bad",csrf));CHECK(s.check(token,csrf,a,true,4000)==AccessResult::Unauthorized);
    // Header validation as net.cpp feeds it (missing header = "").
    const char* ip="192.168.1.20";
    Access g=accessFrom(true,ip,ip,"http://192.168.1.20","application/json");
    CHECK(g.station&&g.host&&g.origin&&g.json);
    CHECK(accessFrom(true,"192.168.1.20:80",ip,"http://192.168.1.20:80","application/json").host);
    struct { const char *host,*local,*origin,*type; bool host_ok,origin_ok,json_ok; } cases[]={
        {"192.168.1.20:8080",ip,"http://192.168.1.20:8080","application/json",false,false,true},
        {"192.168.1.200",ip,"http://192.168.1.200","application/json",false,false,true},   // prefix trick
        {"orcon.local",ip,"http://orcon.local","application/json",false,false,true},      // DNS name / rebinding
        {"",ip,"","",false,false,false},                                                     // all headers missing
        {ip,"0.0.0.0","http://192.168.1.20","application/json",true&&false,false,true},    // no station IP
        {ip,"",ip,"application/json",false,false,true},
        {ip,ip,"","application/json",true,false,true},                                       // Origin missing
        {ip,ip,"null","application/json",true,false,true},
        {ip,ip,"https://192.168.1.20","application/json",true,false,true},
        {ip,ip,"http://192.168.1.21","application/json",true,false,true},
        {ip,ip,"http://192.168.1.20/","application/json",true,false,true},
        {ip,ip,"http://192.168.1.20","application/json; charset=utf-8",true,true,false},
        {ip,ip,"http://192.168.1.20","text/plain",true,true,false},
        {ip,ip,"http://192.168.1.20","Application/JSON",true,true,false},
    };
    for(auto& k:cases){Access x=accessFrom(true,k.host,k.local,k.origin,k.type);
        CHECK(x.host==k.host_ok&&x.origin==k.origin_ok&&x.json==k.json_ok);
        if(!(x.host&&x.origin&&x.json))CHECK(s.check(token,csrf,x,true,700000)!=AccessResult::Allowed);}
    Access n=accessFrom(false,nullptr,nullptr,nullptr,nullptr);CHECK(!n.station&&!n.host&&!n.origin&&!n.json);
}
static void concurrency() {
    struct Value {uint32_t n=0, inverse=0;}; Mailbox<Value> box;
    const uint32_t count=100000;std::atomic<bool> bad{false};
    std::thread producer([&](){for(uint32_t i=1;i<=count;++i){Value v;v.n=i;v.inverse=~i;while(!box.push(v))std::this_thread::yield();}});
    std::thread consumer([&](){for(uint32_t i=1;i<=count;++i){Value v;while(!box.pop(v))std::this_thread::yield();if(v.n!=i||v.inverse!=~i)bad=true;}});
    producer.join();consumer.join();CHECK(!bad&&!box.full());
    // Real controller (main thread) and real owner + dry-run backend (radio thread) on a shared
    // clock that only advances slowly: every transaction must COMPLETE. Expired is a failure.
    Exchange ex;Controller c(ex);DryRunBackend dry;Owner owner(ex,dry);
    std::atomic<uint32_t> clock{T0};std::atomic<bool> stop{false};
    std::thread radio([&](){while(!stop.load()){dry.setRxHealthy(true);owner.tick(clock.load(),true);std::this_thread::yield();}});
    see(c,remoteFrame(1),T0);
    const unsigned transactions=200;unsigned completed=0,other=0;
    for(unsigned k=0;k<transactions;++k){
        const uint32_t t=clock.load()+RATE_MS+REMOTE_GUARD_MS;clock=t;
        see(c,statusFrame(ids().fan,1),t-10,t);
        if(sub(c,input(k+1,(uint8_t)(2+k%2)),t)!=Admission::Accepted){++other;continue;}
        for(unsigned spin=0;c.status().busy&&spin<50000000;++spin){
            if(spin%100==99)clock.fetch_add(1);
            c.tick(clock.load());std::this_thread::yield();}
        if(c.status().phase==Phase::DryRunComplete)++completed;else{++other;printf("  transaction %u: %s/%s\n",k,phaseName(c.status().phase),errorName(c.status().result.error));}
    }
    stop=true;radio.join();
    CHECK(completed==transactions&&other==0);
}
int main(){golden();schema();faults();ownerTrace();dryRunFaults();transactions();correlation();identity();health();security();concurrency();
    printf("TX-PoC: %d checks, %d failures\n",checks,fails);return fails?1:0;}
