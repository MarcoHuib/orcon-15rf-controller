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
static Input input(uint32_t request=1,uint8_t mode=1) { Input v;v.request=request;v.mode=mode;return v; }
static Request req(uint32_t now=1000) { Request v;v.ids=ids();v.input=input();v.expires=now+QUEUE_MS;return v; }
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
    bool pending=false,fast=true,full=true;
    int prepares=0,starts=0,cleanups=0,restores=0,reinits=0;
    bool prepare(const Request&,size_t& n) override {++prepares;n=50;return fault!=Error::Prepare;}
    bool start(uint32_t) override {++starts;return fault!=Error::Start;}
    Completion poll(uint32_t) override {return pending?Completion::Pending:fault==Error::Send?Completion::Failed:Completion::Complete;}
    bool cleanup() override {++cleanups;return fault!=Error::Cleanup;}
    bool restoreRx() override {++restores;return fast;}
    bool reinitializeRx() override {++reinits;return full;}
};
static Result runFault(Fake& fake,Error expected,bool healthy=true,uint32_t start=1000) {
    Exchange ex; Owner owner(ex,fake); CHECK(ex.requests.push(req(start)));
    for(uint32_t n=0;n<250;n+=5)owner.tick(start+n,healthy);
    Result result;CHECK(ex.results.pop(result));CHECK(result.error==expected);CHECK(!result.rfSent);
    CHECK(fake.cleanups==1&&fake.restores==1&&fake.starts<=1);
    for(int n=0;n<1000;++n)owner.tick(start+300+n,true);
    CHECK(fake.cleanups==1&&fake.starts<=1); // no retries or unsolicited work
    return result;
}
static void faults() {
    for(Error e:{Error::None,Error::Prepare,Error::Start,Error::Send,Error::Cleanup}) {
        Fake f;f.fault=e;Result r=runFault(f,e);CHECK(r.rxRestored);
    }
    Fake timeout;timeout.pending=true;CHECK(runFault(timeout,Error::SendTimeout).rxRestored);
    Fake fallback;fallback.fast=false;CHECK(runFault(fallback,Error::None).rxRestored);CHECK(fallback.reinits==1);
    Fake failed;failed.fast=failed.full=false;CHECK(!runFault(failed,Error::RxRestore).rxRestored);CHECK(failed.reinits==RECOVERY_MAX);
    Fake unavailable;runFault(unavailable,Error::RadioUnavailable,false);CHECK(unavailable.starts==0);
    Fake wrap;runFault(wrap,Error::None,true,0xFFFFFFD0u);
    Exchange ex; Fake f;Owner owner(ex,f);
    CHECK(ex.requests.push(req(1000)));owner.tick(1500,true);
    for(int t=1505;t<1700;t+=5)owner.tick(t,true);
    Result r;CHECK(ex.results.pop(r)&&r.error==Error::Expired&&f.starts==0);
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
    Result result;CHECK(ex.results.pop(result)&&result.rxRestored&&!result.rfSent);
    CHECK(f.prepares==1&&f.starts==1&&f.cleanups==1&&f.restores==1&&f.reinits==0);
    // Expiration between dequeue and preparation still forbids starting work.
    Exchange late;Fake lf;Owner lo(late,lf);CHECK(late.requests.push(req()));
    lo.tick(1499,true);lo.tick(1500,true);
    for(int t=1505;t<1600;t+=5)lo.tick(t,true);
    CHECK(late.results.pop(result)&&result.error==Error::Expired&&lf.starts==0);
    // Exhausted restore is a latched admission fault even if a caller says ready.
    Exchange failed;Fake ff;ff.fast=ff.full=false;Owner fo(failed,ff);Controller c(failed);
    CHECK(c.submit(input(),ids(),true,true,1000)==Admission::Accepted);
    for(int t=1000;t<1200;t+=5){fo.tick(t,true);c.tick(t);}
    CHECK(c.status().locked&&c.status().result.error==Error::RxRestore);
    CHECK(c.submit(input(2),ids(),true,true,12000)==Admission::Unavailable);
    CHECK(c.submit(input(),ids(),true,true,12000)==Admission::Duplicate);
}
static void transactions() {
    Exchange ex;Controller c(ex);DryRunBackend dry;dry.setRxHealthy(true);Owner owner(ex,dry);
    CHECK(c.submit(input(),ids(),false,true,1000)==Admission::UnknownUnit);
    CHECK(c.submit(input(),ids(),true,false,1000)==Admission::Unavailable);
    CHECK(c.submit(input(),ids(),true,true,1000)==Admission::Accepted);
    CHECK(c.submit(input(),ids(),true,true,1001)==Admission::Duplicate);
    CHECK(c.submit(input(1,2),ids(),true,true,1001)==Admission::Conflict);
    CHECK(c.submit(input(2),ids(),true,true,1001)==Admission::Busy);
    orcon::Ids changed=ids();changed.fan.serial=303;
    CHECK(c.submit(input(),changed,true,true,1001)==Admission::Conflict);
    for(int t=1000;t<1100;t+=5){owner.tick(t,true);c.tick(t);}
    CHECK(c.status().phase==Phase::DryRunComplete&&!c.status().busy);
    CHECK(!c.status().result.rfSent&&c.status().result.rxRestored&&c.status().result.bytes==50);
    CHECK(c.status().result.request.ids.fan==ids().fan);
    CHECK(c.submit(input(2),ids(),true,true,1100)==Admission::Limited);
    CHECK(c.submit(input(3),ids(),true,true,11000)==Admission::Replay);
    CHECK(c.submit(input(2),ids(),true,true,11000)==Admission::Accepted);
    for(int t=11000;t<11100;t+=5){owner.tick(t,true);c.tick(t);}
    CHECK(c.submit(input(1),ids(),true,true,22000)==Admission::Replay);
    // Boot and 25 simulated idle hours never manufacture a request or result.
    Exchange quiet;Controller qc(quiet);Fake qf;Owner qo(quiet,qf);
    for(uint32_t t=0;t<25u*3600000u;t+=1000){qo.tick(t,true);qc.tick(t);}
    CHECK(qf.starts==0&&qc.status().phase==Phase::Idle);
    // Owner timeout retains admission reservation until expired request is cleaned.
    Exchange late;Controller lc(late);Fake lf;Owner lo(late,lf);
    CHECK(lc.submit(input(),ids(),true,true,1000)==Admission::Accepted);lc.tick(2000);
    CHECK(lc.status().phase==Phase::Failed&&lc.status().busy);
    CHECK(lc.submit(input(2),ids(),true,true,12000)==Admission::Busy);
    for(int t=12000;t<12100;t+=5){lo.tick(t,true);lc.tick(t);}
    CHECK(lf.starts==0&&!lc.status().busy&&lc.status().result.error==Error::OwnerTimeout);
    // Queue is strictly bounded, never overwritten.
    Exchange full;CHECK(full.requests.push(req()));Controller fc(full);
    CHECK(fc.submit(input(),ids(),true,true,1000)==Admission::Busy);
    Request r;CHECK(full.requests.pop(r)&&r.ids.fan==ids().fan);
}
static Frame statusFrame(DeviceId from,uint8_t mode) {
    Frame f;f.type=MsgType::I;f.addr[0]=from;f.has_addr[1]=false;f.has_addr[2]=true;f.addr[2]=from;
    f.cmd=0x31D9;f.len=3;f.payload[2]=mode;return f;
}
static void correlation() {
    Exchange ex;Controller c(ex);CHECK(c.submit(input(),ids(),true,true,1000)==Admission::Accepted);
    Request r;CHECK(ex.requests.pop(r));Result sent;sent.request=r;sent.rfSent=true;sent.rxRestored=true;sent.sentMs=1010;sent.restoredMs=1012;
    CHECK(ex.results.push(sent));c.tick(1030);CHECK(c.status().phase==Phase::AwaitUnit);
    c.onFrame(statusFrame(ids().fan,1),1000);CHECK(c.status().phase==Phase::AwaitUnit); // old queued frame
    c.onFrame(statusFrame(DeviceId(29,303),1),1040);CHECK(c.status().phase==Phase::AwaitUnit);
    c.onFrame(statusFrame(ids().fan,3),1040);CHECK(c.status().phase==Phase::AwaitUnit);
    Frame wrong=statusFrame(ids().fan,1);wrong.type=MsgType::W;c.onFrame(wrong,1040);CHECK(c.status().phase==Phase::AwaitUnit);
    c.onFrame(statusFrame(ids().fan,1),11010);CHECK(c.status().phase==Phase::AwaitUnit); // deadline exclusive
    c.onFrame(statusFrame(ids().fan,1),1050);CHECK(c.status().phase==Phase::UnitReported);
    // Consume owner result before draining RX; only then evaluate loop-time expiry.
    // A timely received frame remains evidence even after a long main-loop stall.
    Exchange delayed;Controller delayedController(delayed);
    CHECK(delayedController.submit(input(),ids(),true,true,1000)==Admission::Accepted);
    CHECK(delayed.requests.pop(r));sent.request=r;sent.rfSent=true;CHECK(delayed.results.push(sent));
    delayedController.consumeResult();delayedController.onFrame(statusFrame(ids().fan,1),1050);
    delayedController.tick(20000);CHECK(delayedController.status().phase==Phase::UnitReported);
    // Dry-run completion must never open a confirmation window.
    Exchange dry;Controller dc(dry);CHECK(dc.submit(input(),ids(),true,true,1000)==Admission::Accepted);
    CHECK(dry.requests.pop(r));sent.request=r;sent.rfSent=false;CHECK(dry.results.push(sent));dc.tick(1030);
    dc.onFrame(statusFrame(ids().fan,1),1050);CHECK(dc.status().phase==Phase::DryRunComplete);
    for(bool overlap:{false,true}) {
        Exchange e;Controller t(e);CHECK(t.submit(input(),ids(),true,true,1000)==Admission::Accepted);
        CHECK(e.requests.pop(r));sent.request=r;sent.rfSent=true;CHECK(e.results.push(sent));t.tick(1030);
        if(overlap) {Frame f;CHECK(makeMode04(ids(),2,f));f.addr[0]=DeviceId(29,404);t.onFrame(f,1040);}
        else t.tick(11010);
        CHECK(t.status().phase==Phase::Failed&&t.status().result.error==(overlap?Error::Ambiguous:Error::UnitTimeout));
    }
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
    for(int k=0;k<4;++k){Access bad=a;if(k==0)bad.station=false;if(k==1)bad.host=false;if(k==2)bad.origin=false;if(k==3)bad.json=false;
        CHECK(s.check(token,csrf,bad,true,1000)==AccessResult::Forbidden);}
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
}
static void concurrency() {
    struct Value {uint32_t n=0, inverse=0;}; Mailbox<Value> box;
    const uint32_t count=100000;std::atomic<bool> bad{false};
    std::thread producer([&](){for(uint32_t i=1;i<=count;++i){Value v;v.n=i;v.inverse=~i;while(!box.push(v))std::this_thread::yield();}});
    std::thread consumer([&](){for(uint32_t i=1;i<=count;++i){Value v;while(!box.pop(v))std::this_thread::yield();if(v.n!=i||v.inverse!=~i)bad=true;}});
    producer.join();consumer.join();CHECK(!bad&&!box.full());
    // Actual controller and owner in separate threads, with copied identities/results.
    Exchange ex;Controller c(ex);Fake f;Owner owner(ex,f);std::atomic<bool> stop{false};
    std::thread radio([&](){uint32_t t=0;while(!stop.load()){owner.tick(++t,true);std::this_thread::yield();}});
    CHECK(c.submit(input(),ids(),true,true,0)==Admission::Accepted);
    for(unsigned i=0;i<1000000&&c.status().busy;++i){c.tick(0);std::this_thread::yield();}
    CHECK(!c.status().busy);stop=true;radio.join();
    CHECK(c.status().phase==Phase::DryRunComplete||c.status().result.error==Error::Expired);
}
int main(){golden();schema();faults();ownerTrace();transactions();correlation();security();concurrency();
    printf("TX-PoC: %d checks, %d failures\n",checks,fails);return fails?1:0;}
