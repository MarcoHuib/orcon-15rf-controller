// setup_flow.cpp — see setup_flow.h
#include "setup_flow.h"
#include <string.h>

namespace setupflow {

const char* const KEY_NAME[K_N] = { "away", "auto", "timer", "1", "2", "3" };
static const uint8_t KEY_MODE[K_N] = { 0x00, 0x04, 0xFF, 0x01, 0x02, 0x03 };   // 22F1 byte 1

uint8_t keyFromName(const char* s) {
    for (uint8_t k = 0; s && k < K_N; k++) if (!strcmp(s, KEY_NAME[k])) return k;
    return K_NONE;
}
uint8_t modeOfKey(uint8_t k) { return k < K_N ? KEY_MODE[k] : 0xFF; }

uint8_t keyOfFrame(const ramses::Frame& f) {
    if (f.type != ramses::MsgType::I || !f.has_addr[0]) return K_NONE;
    if (f.cmd == 0x22F3 && f.len >= 1) return K_TIMER;
    if (f.cmd != 0x22F1 || f.len < 2) return K_NONE;
    for (uint8_t k = 0; k < K_N; k++) if (KEY_MODE[k] == f.payload[1]) return k;
    return K_NONE;                               // 05 auto2, 06 boost, 07 disable: no button of ours
}

bool parseId(const char* s, ramses::DeviceId& out) {
    if (!s || strlen(s) != 9 || s[2] != ':') return false;
    uint32_t c = 0, n = 0;
    for (int i = 0; i < 9; i++) {
        if (i == 2) continue;
        if (s[i] < '0' || s[i] > '9') return false;
        if (i < 2) c = c * 10 + uint32_t(s[i] - '0'); else n = n * 10 + uint32_t(s[i] - '0');
    }
    if (c > 63 || n > 0x3FFFF) return false;
    out = ramses::DeviceId(uint8_t(c), n);
    return true;
}

bool tokenEqual(const char* a, const char* b) {
    size_t n = strlen(a);
    if (!n || n != strlen(b)) return false;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= uint8_t(a[i] ^ b[i]);
    return d == 0;
}

bool requestAllowed(const char* type, const char* origin, const char* host) {
    if (strncmp(type, "application/json", 16) != 0 || (type[16] && type[16] != ';')) return false;
    if (!origin[0]) return true;                 // same-origin requests from older browsers
    return !strncmp(origin, "http://", 7) && host[0] && !strcmp(origin + 7, host);
}

bool hostAllowed(const char* host, const char* ip) {
    size_t n = strcspn(host, ":");                 // without ":port"
    if (!n || (host[n] && strcmp(host + n, ":80"))) return false;
    if (strlen(ip) == n && !strncmp(host, ip, n)) return true;
    return n == 15 && !strncmp(host, "orcon15rf.local", n);
}

// The JSON library accepts duplicate keys and trailing data. This bounded lexical
// pass runs first, preserving one interpretation of a mutating API request.
bool uniqueObjectKeys(const char* json, const char* const* keys, size_t nkeys) {
    unsigned depth=0; uint32_t seen=0; bool began=false, done=false;
    for (size_t i=0; json[i]; ++i) {
        const char c=json[i];
        if (done) { if (c!=' ' && c!='\t' && c!='\r' && c!='\n') return false; continue; }
        if (!began) { if (c==' '||c=='\t'||c=='\r'||c=='\n') continue; if(c!='{') return false; began=true; }
        if (c=='"') {
            const size_t start=++i; bool escaped=false;
            while (json[i] && json[i]!='"') {
                if (json[i]=='\\') { escaped=true; if(!json[++i]) return false; }
                ++i;
            }
            if (!json[i]) return false;
            size_t after=i+1;
            while(json[after]==' '||json[after]=='\t'||json[after]=='\r'||json[after]=='\n') ++after;
            if(json[after]==':') {
                if(depth!=1||escaped) return false; // no nested API objects/escaped aliases
                size_t k=0;
                for(;k<nkeys;++k) if(strlen(keys[k])==i-start && !memcmp(json+start,keys[k],i-start)) break;
                if(k==nkeys) continue; // handler reports unknown-field
                if(k>=32||(seen&(uint32_t(1)<<k))) return false;
                seen|=uint32_t(1)<<k;
            }
        } else if(c=='{'||c=='[') ++depth;
        else if(c=='}'||c==']') { if(!depth) return false; if(!--depth) done=true; }
    }
    return done;
}

const char* stName(St s) {
    switch (s) {
        case St::Active:    return "active";
        case St::Done:      return "done";
        case St::Timeout:   return "timeout";
        case St::Cancelled: return "cancelled";
        default:            return "idle";
    }
}
const char* whyName(Why w) {
    switch (w) {
        case Why::Accepted:  return "accepted";
        case Why::Duplicate: return "duplicate";
        case Why::WrongKey:  return "wrong-button";
        case Why::Unknown:   return "unknown-command";
        case Why::Stale:     return "before-step";
        default:             return "";
    }
}

// ---- Read ---------------------------------------------------------------------
void Read::start(uint32_t s, const ramses::DeviceId& r, uint32_t now, uint32_t seqNow) {
    *this = Read();
    sid = s; remote = r; st = St::Active;
    armedMs_ = now; armedSeq_ = seqNow;
}

uint32_t Read::leftMs(uint32_t now) const {
    if (st != St::Active) return 0;
    int32_t left = int32_t(STEP_MS) - int32_t(now - armedMs_);
    return left > 0 ? uint32_t(left) : 0;
}

static bool samePacket(const ramses::Frame& f, const Step& s) {
    if (f.cmd != s.cmd || f.len != s.len) return false;
    uint8_t n = f.len < sizeof(s.pl) ? f.len : uint8_t(sizeof(s.pl));
    return !memcmp(f.payload, s.pl, n);
}

Why Read::onFrame(const ramses::Frame& f, uint32_t ev, uint32_t seq, uint32_t now, int16_t rssi) {
    if (st != St::Active || !f.has_addr[0]) return Why::None;
    const uint8_t k = keyOfFrame(f);
    if (!(f.addr[0] == remote)) {                // another device: never a step, count only
        if (k != K_NONE) others++;
        return Why::None;
    }
    if (f.type != ramses::MsgType::I) return Why::None;   // not a button press
    Why w;
    if (step && samePacket(f, got[step - 1])) w = Why::Duplicate;     // remotes repeat each press
    else if ((seq && seq <= armedSeq_) || int32_t(now - armedMs_) < int32_t(GUARD_MS)) w = Why::Stale;
    else if (k == K_NONE) w = Why::Unknown;
    else if (k != step)   w = Why::WrongKey;
    else                  w = Why::Accepted;
    why = w; whyEv = ev; whyCmd = f.cmd;
    whyMode = (f.cmd == 0x22F1 && f.len >= 2) ? f.payload[1] : 0xFF;
    if (w != Why::Accepted) return w;
    ramses::DeviceId dest(0,0); bool haveDest=false;
    for(unsigned i=1;i<3;++i) if(f.has_addr[i]&&!(f.addr[i]==remote)&&f.addr[i].cls>0&&f.addr[i].cls<63&&f.addr[i].serial>0) {
        if(haveDest&&!(dest==f.addr[i]))targetConflict=true;
        dest=f.addr[i];haveDest=true;
    }
    if(!haveDest)targetConflict=true;
    else if(targetKnown&&!(target==dest))targetConflict=true;
    else {target=dest;targetKnown=true;}
    Step& s = got[step];
    s.ev = ev; s.rssi = rssi; s.cmd = f.cmd; s.len = f.len;
    memcpy(s.pl, f.payload, f.len < sizeof(s.pl) ? f.len : sizeof(s.pl));
    if (++step == K_N) st = St::Done;
    armedMs_ = now; armedSeq_ = seq;             // the next button is armed by this packet
    return w;
}

// ---- Pair ---------------------------------------------------------------------
void Pair::onFrame(const ramses::Frame& f, const ramses::DeviceId& us, bool strict) {
    if (st != St::Active || !sent || confirmed || f.cmd != 0x1FC9 || f.type == ramses::MsgType::RQ) return;
    if (!f.has_addr[0] || f.addr[0] == us) return;
    const bool toUs = (f.has_addr[1] && f.addr[1] == us) || (f.has_addr[2] && f.addr[2] == us);
    if (strict) {
        const uint32_t raw = f.addr[0].raw();
        if (f.type != ramses::MsgType::W || f.len != 6 || f.payload[0] != 0 ||
            f.payload[1] != 0x31 || f.payload[2] != 0xD9 ||
            f.payload[3] != uint8_t(raw >> 16) || f.payload[4] != uint8_t(raw >> 8) ||
            f.payload[5] != uint8_t(raw) || f.addr[0].cls == 0 || f.addr[0].cls >= 63 ||
            f.addr[0].serial == 0) return;
    }
    if (toUs) { confirmed = true; from = f.addr[0]; }
}

void Pair::tick(uint32_t now, bool fwPaired, const ramses::DeviceId& fan) {
    if (st != St::Active) return;
    if (paired(fwPaired, fan)) { st = St::Done; return; }
    if (sent && int32_t(now - sentMs_) >= int32_t(WINDOW_MS)) st = St::Timeout;
    else if (!sent && int32_t(now - startMs_) >= int32_t(WINDOW_MS)) st = St::Timeout;   // abandoned
}

const char* Pair::status(bool fwPaired, const ramses::DeviceId& fan) const {
    if (st == St::Done || (st == St::Active && paired(fwPaired, fan))) return "paired";
    if (st != St::Active) return stName(st);
    if (txFailed)  return "tx_failed";
    if (confirmed && ackSent) return "ack_sent";
    if (confirmed) return "rf_confirmed";
    return sent ? "bind_sent" : "ready";
}

uint32_t Pair::leftMs(uint32_t now) const {
    if (st != St::Active) return 0;
    int32_t left = int32_t(WINDOW_MS) - int32_t(now - (sent ? sentMs_ : startMs_));
    return left > 0 ? uint32_t(left) : 0;
}

// ---- Cmd ----------------------------------------------------------------------
void Cmd::record(uint32_t i, uint8_t k, uint16_t m, bool s, uint32_t now, uint32_t seqNow) {
    id = i; key = k; minutes = m; sent = s; atMs_ = now; seq_ = seqNow;
    confirm = !s ? None : (modeOfKey(k) == 0xFF ? NotAvailable : Pending);
}

void Cmd::onUnitMode(uint8_t mode, uint32_t seq) {
    if (confirm != Pending) return;
    if (seq && seq <= seq_) return;              // began before our send (radios with sync numbers)
    if (mode == modeOfKey(key)) confirm = Confirmed;
}

const char* confirmName(Cmd::Confirm c) {
    switch (c) {
        case Cmd::Pending:      return "pending";
        case Cmd::Confirmed:    return "confirmed";
        case Cmd::NotConfirmed: return "not_confirmed";
        case Cmd::NotAvailable: return "not_available";
        default:                return "none";
    }
}

} // namespace setupflow
