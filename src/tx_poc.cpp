#include "tx_poc.h"
#include <string.h>
#include <limits.h>
namespace txpoc {
bool due(uint32_t now,uint32_t deadline) { return (int32_t)(now-deadline)>=0; }
bool validIds(const orcon::Ids& ids) { return validId(ids.us)&&validId(ids.fan)&&!(ids.us==ids.fan); }
bool makeModeCmd(const orcon::Ids& ids,uint8_t mode,uint8_t tail,ramses::Frame& out) {
    if(!validIds(ids)||mode>4||(tail!=4&&tail!=7)) return false;
    ramses::Frame f;
    f.type=ramses::MsgType::I;
    f.has_addr[0]=f.has_addr[1]=true; f.has_addr[2]=false;
    f.addr[0]=ids.us; f.addr[1]=ids.fan; f.cmd=0x22F1; f.len=3;
    f.payload[0]=0; f.payload[1]=mode; f.payload[2]=tail;
    out=f; return true;
}
bool makeBindOffer(const orcon::Ids& ids,ramses::Frame& out) {
    if(!validId(ids.us)) return false;
    ramses::Frame f;
    f.type=ramses::MsgType::I;
    f.has_addr[0]=f.has_addr[1]=true; f.has_addr[2]=false;
    f.addr[0]=ids.us; f.addr[1]=ramses::DeviceId(63,0x3FFFE); f.cmd=0x1FC9;
    static const uint8_t tuple[4][3]={{0x00,0x1F,0xC9},{0x00,0x22,0xF1},{0x00,0x22,0xF3},{0x67,0x10,0xE0}};
    const uint32_t raw=ids.us.raw();
    for(int i=0;i<4;++i) {
        uint8_t* t=f.payload+6*i;
        t[0]=tuple[i][0]; t[1]=tuple[i][1]; t[2]=tuple[i][2];
        t[3]=uint8_t(raw>>16); t[4]=uint8_t(raw>>8); t[5]=uint8_t(raw);
    }
    f.len=24;
    out=f; return true;
}
bool makeBindAck(const orcon::Ids& ids,ramses::Frame& out) {
    if(!validId(ids.us)||!validId(ids.fan)||ids.us==ids.fan) return false;
    ramses::Frame f;
    f.type=ramses::MsgType::I;
    f.has_addr[0]=f.has_addr[1]=true; f.has_addr[2]=false;
    f.addr[0]=ids.us; f.addr[1]=ids.fan; f.cmd=0x1FC9; f.len=1;
    f.payload[0]=0x00;                              // pairing.log: closing I 1FC9, pl=00
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
            if(value>4) return false;
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
bool identityConflict(const orcon::Ids& ids,const rlog::Seen& seen,const rlog::Device* devs,size_t n) {
    if((seen.haveRemote&&seen.remote==ids.us)||(seen.haveFan&&seen.fan==ids.us)) return true;
    for(size_t i=0;i<n;++i) if(devs[i].id==ids.us) return true;
    return false;
}
const char* errorName(Error e) {
    switch(e) {
#define E(a,b) case Error::a:return b
        E(None,"none"); E(Expired,"queue-expired"); E(RadioUnavailable,"radio-unavailable");
        E(Prepare,"prepare-failed"); E(Start,"start-failed"); E(Send,"send-failed");
        E(SendTimeout,"send-timeout"); E(Cleanup,"cleanup-failed"); E(RxRestore,"rx-restore-failed");
        E(OwnerTimeout,"owner-timeout");
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
        case Phase::Unchanged:return "unchanged";
        case Phase::NotConfirmed:return "not-confirmed";
        case Phase::Ambiguous:return "ambiguous";
        case Phase::Failed:return "failed";
    }
    return "invalid";
}
bool DryRunBackend::prepare(const Request& r,size_t& bytes) {
    ramses::Frame f; uint8_t packed[ramses::MAX_ONAIR]; size_t bits=0;   // 1FC9 offer is ~105 B on air
    bool ok=false;
    if(r.kind==(uint8_t)FrameKind::Mode) ok=makeModeCmd(r.ids,r.input.mode,r.tail,f);
    else if(r.kind==(uint8_t)FrameKind::BindOffer) ok=makeBindOffer(r.ids,f);
    else if(r.kind==(uint8_t)FrameKind::BindAck) ok=makeBindAck(r.ids,f);
    if(!ok) return false;
    bytes=ramses::encodeFrame(f,packed,sizeof(packed),TX_PREAMBLE_BYTES,&bits);
    if(r.kind==(uint8_t)FrameKind::Mode) return bytes==53&&bits==424;
    return bytes>0&&bits>0;
}
void Owner::fail(Error e) { if(result_.error==Error::None) result_.error=e; step_=Step::Cleanup; }
void Owner::finish(Error e) { if(result_.error==Error::None) result_.error=e; step_=Step::Publish; }
void Owner::tick(uint32_t now,bool rxHealthy) {
    // A resumed owner must not report successful work after its hardware deadline.
    // Cleanup/restore still run once even if scheduling was interrupted.
    if((step_==Step::Cleanup||step_==Step::Restore)&&due(now,deadline_)&&result_.error==Error::None)
        result_.error=Error::SendTimeout;
    switch(step_) {
        case Step::Idle: {
            Request request; if(!ex_.requests.pop(request)) return;
            result_=Result{}; result_.request=request; deadline_=now+OWNER_MS;
            if(due(now,request.expires)) finish(Error::Expired);
            else if(!rxHealthy) finish(Error::RadioUnavailable);
            else step_=Step::Prepare;
            break;
        }
        case Step::Prepare:
            if(due(now,result_.request.expires)) { finish(Error::Expired); break; }
            if(!rxHealthy) { finish(Error::RadioUnavailable); break; }
            if(due(now,deadline_)) { finish(Error::SendTimeout); break; }
            result_.touched=true; // from here a hardware backend may have changed radio state
            if(!port_.prepare(result_.request,result_.bytes)) fail(Error::Prepare);
            else if(!port_.start(now)) fail(Error::Start);
            else step_=Step::Simulate;
            break;
        case Step::Simulate: {
            if(due(now,deadline_)) { fail(Error::SendTimeout); break; }
            Completion c=port_.poll(now);
            if(c==Completion::Failed) fail(Error::Send);
            else if(c==Completion::Complete) { result_.rfSent=port_.onAir(); result_.sentMs=now; step_=Step::Cleanup; }
            break;
        }
        case Step::Cleanup:
            result_.cleanupFailed=!port_.cleanup();
            if(result_.cleanupFailed&&result_.error==Error::None) result_.error=Error::Cleanup;
            step_=Step::Restore; break;
        case Step::Restore:
            // One fast restore. Failure is published and latched by the controller; any
            // radio restart belongs to the existing RX recovery, never to this owner.
            if(port_.restoreRx()) { result_.rxRestored=true; result_.restoredMs=now; }
            else if(result_.error==Error::None) result_.error=Error::RxRestore;
            step_=Step::Publish; break;
        case Step::Publish:
            // Retain ownership on backpressure, never block or drop completion.
            if(ex_.results.push(result_)) step_=Step::Idle;
            break;
    }
}
static bool sameIds(const orcon::Ids& a,const orcon::Ids& b) { return a.us==b.us&&a.fan==b.fan; }
void Controller::selectUnit(const ramses::DeviceId& id) {
    if (unitSelected_ && observedUnit_==id) return;
    observedUnit_=id; unitSelected_=true;
    status_.haveUnit=status_.haveRemote=false;
    status_.unitMode=0xFF; status_.unitMs=status_.remoteMs=0;
}
Admission Controller::begin(Request& request,uint32_t now) {
    if(!ex_.requests.push(request)) return Admission::Busy;
    status_.result=Result{}; status_.result.request=request;
    status_.phase=Phase::Queued; status_.busy=true; ++status_.nextRequest;
    status_.acceptedMs=now;
    acceptedMs_=lastAcceptedMs_=now; haveAccepted_=true; ownerTimedOut_=false;
    priorChanged_=false; observed_=Phase::NotConfirmed; observedMs_=0;
    return Admission::Accepted;
}
Admission Controller::submit(Input input,const orcon::Ids& ids,bool known,bool ready,bool identityClear,uint32_t now) {
    if (!status_.busy) selectUnit(ids.fan);
    if(!input.request||input.mode>4) return Admission::Invalid;
    if(!known||!validIds(ids)) return Admission::UnknownUnit;
    const Request& last=status_.result.request;
    if(haveAccepted_&&input.request==last.input.request)
        return input.mode==last.input.mode&&sameIds(ids,last.ids)?Admission::Duplicate:Admission::Conflict;
    if(input.request!=status_.nextRequest) return Admission::Replay;
    if(status_.busy) return Admission::Busy;
    if(!ready||status_.locked) return Admission::Unavailable;
    if(identityCollided_||!identityClear||(status_.haveRemote&&status_.remoteId==ids.us)) return Admission::IdentityConflict;
    if(!status_.haveRemote) return Admission::IdentityUnverified; // physical remote not heard this boot
    if(haveAccepted_&&(uint32_t)(now-lastAcceptedMs_)<RATE_MS) return Admission::Limited;
    if(!status_.haveUnit||(uint32_t)(now-status_.unitMs)>=UNIT_STATE_MAX_AGE_MS) return Admission::UnitStateUnknown;
    if(status_.unitMode==input.mode) return Admission::Unchanged;
    if(!due(now,status_.remoteMs+REMOTE_GUARD_MS)) return Admission::RemoteActive;
    if(input.request==UINT32_MAX) return Admission::Replay; // never wrap/replay IDs
    Request request; request.input=input; request.ids=ids; request.expires=now+QUEUE_MS;
    request.prior=status_.unitMode; request.tail=0x04;
    return begin(request,now);
}
Admission Controller::submitCtl(uint8_t mode,uint8_t tail,const orcon::Ids& ids,bool known,bool ready,bool identityClear,uint32_t now,uint32_t sequence) {
    if (!status_.busy) selectUnit(ids.fan);
    if (sequence) {
        const Request& last=status_.result.request;
        if (haveAccepted_ && sequence==last.input.request)
            return last.kind==(uint8_t)FrameKind::Mode && mode==last.input.mode &&
                   tail==last.tail && sameIds(ids,last.ids) ? Admission::Duplicate : Admission::Conflict;
        if (sequence!=status_.nextRequest) return Admission::Replay;
    }
    if (status_.nextRequest==UINT32_MAX) return Admission::Replay;
    if(mode>4||(tail!=4&&tail!=7)) return Admission::Invalid; // never guess the unit family
    if(!known||!validIds(ids)) return Admission::UnknownUnit;
    if(status_.busy) return Admission::Busy;
    if(!ready||status_.locked) return Admission::Unavailable;
    if(identityCollided_||!identityClear||(status_.haveRemote&&status_.remoteId==ids.us)) return Admission::IdentityConflict;
    if(!status_.haveRemote) return Admission::IdentityUnverified;
    if(haveAccepted_&&(uint32_t)(now-lastAcceptedMs_)<RATE_CTL_MS) return Admission::Limited;
    if(!status_.haveUnit||(uint32_t)(now-status_.unitMs)>=UNIT_STATE_MAX_AGE_MS) return Admission::UnitStateUnknown;
    if(status_.unitMode==mode) return Admission::Unchanged;
    if(!due(now,status_.remoteMs+REMOTE_GUARD_MS)) return Admission::RemoteActive;
    Input input; input.request=status_.nextRequest; input.mode=mode;
    Request request; request.input=input; request.ids=ids; request.expires=now+QUEUE_MS;
    request.prior=status_.unitMode; request.tail=tail;
    return begin(request,now);
}
Admission Controller::submitBind(uint8_t kind,const orcon::Ids& ids,bool ready,bool identityClear,uint32_t now) {
    if(kind!=(uint8_t)FrameKind::BindOffer && kind!=(uint8_t)FrameKind::BindAck) return Admission::Invalid;
    if(!validId(ids.us)) return Admission::Invalid;              // our own id must be real
    if(kind==(uint8_t)FrameKind::BindAck &&                       // the ack answers a specific unit
       (!validId(ids.fan)||ids.us==ids.fan)) return Admission::Invalid;
    // The offer is a self broadcast: ids.fan is ignored by the builder.
    if(status_.busy) return Admission::Busy;
    if(!ready||status_.locked) return Admission::Unavailable;
    if(identityCollided_||!identityClear) return Admission::IdentityConflict;
    if(kind==(uint8_t)FrameKind::BindOffer &&                     // one offer per rate window;
       haveAccepted_&&(uint32_t)(now-lastAcceptedMs_)<RATE_CTL_MS) // the ack is exempt: it is the
        return Admission::Limited;                                // handshake's answer, not a new action
    Input input; input.request=status_.nextRequest; input.mode=0;
    Request request; request.input=input; request.ids=ids; request.expires=now+QUEUE_MS;
    request.prior=0xFF; request.kind=kind;
    return begin(request,now);
}
void Controller::consumeResult(uint32_t now) {
    Result result;
    if(!ex_.results.pop(result)||!status_.busy||result.request.input.request!=status_.result.request.input.request) return;
    // A completion first consumed after the controller deadline cannot bypass
    // timeout simply because consumeResult runs before tick's queued-state check.
    if(due(now,acceptedMs_+TRANSACTION_MS)) ownerTimedOut_=true;
    status_.result=result;
    if(ownerTimedOut_&&result.error==Error::None) status_.result.error=Error::OwnerTimeout;
    // Uncertain radio health after our own radio work blocks TX until a future boot.
    if(ownerTimedOut_||(result.touched&&(!result.rxRestored||result.cleanupFailed||result.error==Error::Cleanup))) status_.locked=true;
    if(status_.result.error!=Error::None) settle(Phase::Failed);
    else if(!result.touched||!result.rxRestored) {   // defensive: "success" without a restore
        status_.result.error=Error::RxRestore; status_.locked=true; settle(Phase::Failed);
    } else if(result.rfSent) {
        if(result.request.kind==(uint8_t)FrameKind::Mode) {   // only mode commands await a unit report
            status_.phase=Phase::AwaitUnit;
            if(interfered()||priorChanged_) settle(Phase::Ambiguous);
        } else settle(Phase::Idle);   // bind offer/ack: send done; setupflow::Pair drives the UI
    }
    else settle(Phase::DryRunComplete);
}
void Controller::tick(uint32_t now) {
    consumeResult(now);
    if(status_.phase==Phase::Queued&&!ownerTimedOut_&&due(now,acceptedMs_+TRANSACTION_MS)) {
        status_.phase=Phase::Failed; status_.result.error=Error::OwnerTimeout;
        ownerTimedOut_=true; // hold reservation until owner returns cleanup result
    }
    if(status_.phase==Phase::AwaitUnit&&due(now,status_.result.sentMs+CONFIRM_MS+EXPIRY_GRACE_MS))
        settle(interfered()||priorChanged_?Phase::Ambiguous:observed_);
}
// A relevant remote action shortly before, or after, our transmission.
bool Controller::interfered() const {
    return status_.haveRemote&&due(status_.remoteMs,status_.result.sentMs-REMOTE_GUARD_MS);
}
static bool toUnit(const ramses::Frame& f,const ramses::DeviceId& unit) {   // orcon::toUnit semantics
    return (f.has_addr[1]&&f.addr[1]==unit)||(f.has_addr[2]&&!(f.addr[2]==f.addr[0])&&f.addr[2]==unit);
}
void Controller::onFrame(const ramses::Frame& f,uint32_t receivedMs,uint32_t now,const orcon::Ids& current) {
    if(!f.has_addr[0]) return;
    // The PoC identity is immutable this boot. Never forget a collision when
    // rlog's bounded discovery table or its last-seen slots are overwritten.
    if(f.addr[0]==current.us) identityCollided_=true;
    if(status_.phase==Phase::AwaitUnit&&due(now,status_.result.sentMs+CONFIRM_MS+EXPIRY_GRACE_MS))
        tick(now); // exact grace boundary: delayed dequeue is no longer evidence
    const bool timed=receivedMs!=0&&due(now,receivedMs);
    const ramses::DeviceId unit=status_.busy?status_.result.request.ids.fan:current.fan; // pinned while open
    selectUnit(unit);
    if((f.cmd==0x22F1||f.cmd==0x22F3)&&f.type!=ramses::MsgType::RP&&toUnit(f,unit)) {
        const uint32_t at=timed?receivedMs:now;  // untimed: cannot be placed, so count it as now
        if(!status_.haveRemote||due(at,status_.remoteMs)) status_.remoteMs=at;
        status_.haveRemote=true; status_.remoteId=f.addr[0];
        if(status_.phase==Phase::AwaitUnit&&interfered()) settle(Phase::Ambiguous);
        return;
    }
    if(!timed||!(f.addr[0]==unit)||(f.type!=ramses::MsgType::I&&f.type!=ramses::MsgType::RP)||
       f.cmd!=0x31D9||f.len<3||f.payload[0]!=0||f.payload[2]>7) return;
    const uint8_t mode=f.payload[2];
    if(!status_.haveUnit||due(receivedMs,status_.unitMs)) { status_.unitMode=mode; status_.unitMs=receivedMs; status_.haveUnit=true; }
    const Result& r=status_.result;
    // Fail closed when the admission-time prior state changed while queued, or
    // a delayed pre-send report proves it had already changed before our send.
    if((status_.phase==Phase::Queued&&due(receivedMs,acceptedMs_))||
       (status_.phase==Phase::AwaitUnit&&!due(receivedMs,r.sentMs+1))) {
        if(mode!=r.request.prior) priorChanged_=true;
    }
    if(status_.phase!=Phase::AwaitUnit) return;
    if(!due(receivedMs,r.restoredMs)||!due(receivedMs,r.sentMs+1)||due(receivedMs,r.sentMs+CONFIRM_MS)) return; // old or late
    if(interfered()||priorChanged_) settle(Phase::Ambiguous);
    else if(mode!=r.request.input.mode&&mode!=r.request.prior) observed_=Phase::Ambiguous;
    else if(observed_!=Phase::Ambiguous&&(!observedMs_||due(receivedMs,observedMs_))) {
        observedMs_=receivedMs;
        observed_=mode==r.request.input.mode?Phase::UnitReported:
                  mode==r.request.prior?Phase::Unchanged:Phase::Ambiguous;
        // Retain the reservation through the whole window: later interference
        // must not be hidden by an early matching report.
    }
}
Access accessFrom(bool station,const char* host,const char* localIp,const char* origin,const char* contentType) {
    host=host?host:""; localIp=localIp?localIp:""; origin=origin?origin:""; contentType=contentType?contentType:"";
    Access a; a.station=station;
    const size_t n=strlen(localIp);
    a.host=n&&strcmp(localIp,"0.0.0.0")&&!strncmp(host,localIp,n)&&(!host[n]||!strcmp(host+n,":80"));
    a.origin=a.host&&!strncmp(origin,"http://",7)&&!strcmp(origin+7,host);
    a.json=!strcmp(contentType,"application/json");
    return a;
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
