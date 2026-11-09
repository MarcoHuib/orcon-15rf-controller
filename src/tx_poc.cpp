#include "tx_poc.h"
#include <string.h>
#include <limits.h>
namespace txpoc {
bool due(uint32_t now,uint32_t deadline) { return (int32_t)(now-deadline)>=0; }
static bool validId(const ramses::DeviceId& id) {
    return id.cls>0 && id.cls<63 && id.serial>0 && id.serial<=0x3FFFF;
}
bool validIds(const orcon::Ids& ids) { return validId(ids.us)&&validId(ids.fan)&&!(ids.us==ids.fan); }
bool makeMode04(const orcon::Ids& ids,uint8_t mode,ramses::Frame& out) {
    if (!validIds(ids)||mode<1||mode>3) return false;
    ramses::Frame f;
    f.type=ramses::MsgType::I;
    f.has_addr[0]=f.has_addr[1]=true; f.has_addr[2]=false;
    f.addr[0]=ids.us; f.addr[1]=ids.fan; f.cmd=0x22F1; f.len=3;
    f.payload[0]=0; f.payload[1]=mode; f.payload[2]=4;
    out=f; return true;
}
// Exact two-key JSON schema, without duplicate keys, escapes, arrays, coercion,
// floats, signs, leading zeros, overflow, trailing data or unbounded allocation.
bool parseInput(const char* json,size_t len,Input& out) {
    if (!json||len==0||len>96) return false;
    size_t p=0;
    auto ws=[&]() { while(p<len&&(json[p]==' '||json[p]=='\t'||json[p]=='\r'||json[p]=='\n')) ++p; };
    auto take=[&](char c) { ws(); if(p>=len||json[p]!=c) return false; ++p; return true; };
    if(!take('{')) return false;
    Input parsed; bool mode=false,request=false;
    for(unsigned n=0;n<2;++n) {
        if(n&&!take(',')) return false;
        if(!take('"')) return false;
        size_t key=p;
        while(p<len&&json[p]!='"') { if(json[p]<'a'||json[p]>'z') return false; ++p; }
        size_t keyLen=p-key;
        if(!take('"')||!take(':')) return false;
        ws(); size_t start=p; uint32_t value=0;
        while(p<len&&json[p]>='0'&&json[p]<='9') {
            unsigned digit=(unsigned)(json[p++]-'0');
            if(value>(UINT32_MAX-digit)/10) return false;
            value=value*10+digit;
        }
        if(p==start||(p-start>1&&json[start]=='0')) return false;
        if(keyLen==4&&!memcmp(json+key,"mode",4)&&!mode) {
            if(value<1||value>3) return false;
            parsed.mode=(uint8_t)value; mode=true;
        } else if(keyLen==7&&!memcmp(json+key,"request",7)&&!request) {
            if(!value) return false;
            parsed.request=value; request=true;
        } else return false;
    }
    if(!take('}')) return false;
    ws(); if(p!=len||!mode||!request) return false;
    out=parsed; return true;
}
const char* errorName(Error e) {
    switch(e) {
#define E(a,b) case Error::a:return b
        E(None,"none"); E(Expired,"queue-expired"); E(RadioUnavailable,"radio-unavailable");
        E(Prepare,"prepare-failed"); E(Start,"start-failed"); E(Send,"send-failed");
        E(SendTimeout,"send-timeout"); E(Cleanup,"cleanup-failed"); E(RxRestore,"rx-restore-failed");
        E(OwnerTimeout,"owner-timeout"); E(UnitTimeout,"unit-timeout"); E(Ambiguous,"remote-overlap");
#undef E
    }
    return "invalid";
}
const char* phaseName(Phase p) {
    switch(p) {
        case Phase::Idle:return "idle";
        case Phase::Queued:return "queued";
        case Phase::DryRunComplete:return "dry-run-complete";
        case Phase::AwaitUnit:return "await-unit-status";
        case Phase::UnitReported:return "unit-reported-mode";
        case Phase::Failed:return "failed";
    }
    return "invalid";
}
bool DryRunBackend::prepare(const Request& r,size_t& bytes) {
    ramses::Frame f; uint8_t packed[64]; size_t bits=0;
    if(!makeMode04(r.ids,r.input.mode,f)) return false;
    bytes=ramses::encodeFrame(f,packed,sizeof(packed),6,&bits);
    return bytes==50&&bits==400;
}
void Owner::fail(Error e) { if(result_.error==Error::None) result_.error=e; step_=Step::Cleanup; }
void Owner::tick(uint32_t now,bool rxHealthy) {
    switch(step_) {
        case Step::Idle: {
            Request request; if(!ex_.requests.pop(request)) return;
            result_=Result{}; result_.request=request; retries_=0; deadline_=now+OWNER_MS;
            if(due(now,request.expires)) fail(Error::Expired);
            else if(!rxHealthy) fail(Error::RadioUnavailable);
            else step_=Step::Prepare;
            break;
        }
        case Step::Prepare:
            if(due(now,result_.request.expires)) { fail(Error::Expired); break; }
            if(due(now,deadline_)) fail(Error::SendTimeout);
            else if(!port_.prepare(result_.request,result_.bytes)) fail(Error::Prepare);
            else if(!port_.start(now)) fail(Error::Start);
            else step_=Step::Simulate;
            break;
        case Step::Simulate: {
            if(due(now,deadline_)) { fail(Error::SendTimeout); break; }
            Completion c=port_.poll(now);
            if(c==Completion::Failed) fail(Error::Send);
            else if(c==Completion::Complete) { result_.sentMs=now; step_=Step::Cleanup; }
            break;
        }
        case Step::Cleanup:
            if(!port_.cleanup()&&result_.error==Error::None) result_.error=Error::Cleanup;
            step_=Step::Restore; break;
        case Step::Restore:
            if(port_.restoreRx()) { result_.rxRestored=true; result_.restoredMs=now; step_=Step::Publish; }
            else { retryAt_=now+RECOVERY_MS; step_=Step::Recover; }
            break;
        case Step::Recover:
            if(!due(now,retryAt_)) break;
            ++retries_;
            if(port_.reinitializeRx()) { result_.rxRestored=true; result_.restoredMs=now; step_=Step::Publish; }
            else if(retries_>=RECOVERY_MAX) { result_.error=Error::RxRestore; step_=Step::Publish; }
            else retryAt_=now+RECOVERY_MS;
            break;
        case Step::Publish:
            // Retain ownership on backpressure, never block or drop completion.
            if(ex_.results.push(result_)) step_=Step::Idle;
            break;
    }
}
static bool sameIds(const orcon::Ids& a,const orcon::Ids& b) { return a.us==b.us&&a.fan==b.fan; }
Admission Controller::submit(Input input,const orcon::Ids& ids,bool known,bool ready,uint32_t now) {
    if(!input.request||input.mode<1||input.mode>3) return Admission::Invalid;
    if(!known||!validIds(ids)) return Admission::UnknownUnit;
    const Request& last=status_.result.request;
    if(haveAccepted_&&input.request==last.input.request)
        return input.mode==last.input.mode&&sameIds(ids,last.ids)?Admission::Duplicate:Admission::Conflict;
    if(input.request!=status_.nextRequest) return Admission::Replay;
    if(status_.busy) return Admission::Busy;
    if(!ready||status_.locked) return Admission::Unavailable;
    if(haveAccepted_&&(uint32_t)(now-lastAcceptedMs_)<RATE_MS) return Admission::Limited;
    if(input.request==UINT32_MAX) return Admission::Replay; // never wrap/replay IDs
    Request request; request.input=input; request.ids=ids; request.expires=now+QUEUE_MS;
    if(!ex_.requests.push(request)) return Admission::Busy;
    status_.result=Result{}; status_.result.request=request;
    status_.phase=Phase::Queued; status_.busy=true; ++status_.nextRequest;
    acceptedMs_=lastAcceptedMs_=now; haveAccepted_=true; ownerTimedOut_=false;
    return Admission::Accepted;
}
void Controller::consumeResult() {
    Result result;
    if(ex_.results.pop(result)&&status_.busy&&result.request.input.request==status_.result.request.input.request) {
        status_.result=result;
        if(ownerTimedOut_) status_.result.error=Error::OwnerTimeout;
        if(!result.rxRestored||result.error==Error::Cleanup||ownerTimedOut_) status_.locked=true;
        if(status_.result.error!=Error::None||!result.rxRestored) {
            if(status_.result.error==Error::None) status_.result.error=Error::RxRestore;
            status_.phase=Phase::Failed; status_.busy=false;
        } else if(result.rfSent) status_.phase=Phase::AwaitUnit;
        else { status_.phase=Phase::DryRunComplete; status_.busy=false; }
    }
}
void Controller::tick(uint32_t now) {
    consumeResult();
    if(status_.phase==Phase::Queued&&!ownerTimedOut_&&due(now,acceptedMs_+TRANSACTION_MS)) {
        status_.phase=Phase::Failed; status_.result.error=Error::OwnerTimeout;
        ownerTimedOut_=true; // hold reservation until owner returns cleanup result
    }
    if(status_.phase==Phase::AwaitUnit&&due(now,status_.result.sentMs+CONFIRM_MS)) {
        status_.phase=Phase::Failed; status_.result.error=Error::UnitTimeout; status_.busy=false;
    }
}
void Controller::onFrame(const ramses::Frame& f,uint32_t receivedMs) {
    if(status_.phase!=Phase::AwaitUnit||!status_.result.rfSent||!f.has_addr[0]) return;
    const Result& r=status_.result;
    if(!due(receivedMs,r.restoredMs)||!due(receivedMs,r.sentMs+1)||due(receivedMs,r.sentMs+CONFIRM_MS)) return;
    bool toUnit=f.has_addr[1]&&f.addr[1]==r.request.ids.fan;
    if(f.cmd==0x22F1&&toUnit&&!(f.addr[0]==r.request.ids.us)) {
        status_.phase=Phase::Failed; status_.result.error=Error::Ambiguous; status_.busy=false; return;
    }
    if(!(f.addr[0]==r.request.ids.fan)||(f.type!=ramses::MsgType::I&&f.type!=ramses::MsgType::RP)||
        f.cmd!=0x31D9||f.len<3||f.payload[0]!=0||f.payload[2]!=r.request.input.mode) return;
    status_.phase=Phase::UnitReported; status_.busy=false;
}
static bool tokenValid(const char* s) {
    if(!s||strlen(s)!=32) return false;
    for(size_t i=0;i<32;++i) if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f'))) return false;
    return true;
}
static bool tokenEqual(const char* a,const char* b) {
    if(!a||strlen(a)!=32) return false;
    unsigned diff=0; for(size_t i=0;i<32;++i) diff|=(unsigned char)a[i]^(unsigned char)b[i];
    return diff==0;
}
bool Security::begin(const char* token,const char* csrf,uint32_t now) {
    configured_=false; memset(token_,0,sizeof(token_)); memset(csrf_,0,sizeof(csrf_)); failures_=0; window_=createdMs_=now;
    if(!tokenValid(token)||!tokenValid(csrf)||tokenEqual(token,csrf)) return false;
    memcpy(token_,token,33); memcpy(csrf_,csrf,33); configured_=true; return true;
}
void Security::expire(uint32_t now) {
    if(configured_&&(uint32_t)(now-createdMs_)>=600000) configured_=false;
}
AccessResult Security::check(const char* token,const char* csrf,Access a,bool mutation,uint32_t now) {
    expire(now);
    if((uint32_t)(now-window_)>=1000) { window_=now; failures_=0; }
    if(failures_>=8) return AccessResult::Limited;
    if(!configured_||!tokenEqual(token,token_)) { ++failures_; return AccessResult::Unauthorized; }
    if(!a.station||!a.host||(mutation&&(!a.origin||!a.json||!tokenEqual(csrf,csrf_)))) return AccessResult::Forbidden;
    return AccessResult::Allowed;
}
} // namespace txpoc
