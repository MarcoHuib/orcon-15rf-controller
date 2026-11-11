#include "remote_profiles.h"
#include <cstring>
#include <limits.h>
namespace profiles {
static_assert(BLOB_BYTES>=18+127*LIMIT,"profile blob bound");
static bool validId(const ramses::DeviceId& id) { return id.cls>0&&id.cls<63&&id.serial>0&&id.serial<=0x3FFFF; }
bool validName(const char* s) {
    if(!s) return false;
    const size_t n=strlen(s); if(!n||n>NAME_BYTES||s[0]==' '||s[n-1]==' ') return false;
    for(size_t i=0;i<n;) {
        unsigned c=(unsigned char)s[i++];
        if(c<0x20||c==0x7F) return false;
        if(c<0x80) continue;
        unsigned more=0,cp=0,min=0;
        if(c>=0xC2&&c<=0xDF){more=1;cp=c&31;min=0x80;}
        else if(c>=0xE0&&c<=0xEF){more=2;cp=c&15;min=0x800;}
        else if(c>=0xF0&&c<=0xF4){more=3;cp=c&7;min=0x10000;}
        else return false;
        while(more--){if(i>=n) return false;c=(unsigned char)s[i++];if((c&0xC0)!=0x80)return false;cp=(cp<<6)|(c&63);}
        if(cp<min||cp>0x10FFFF||(cp>=0xD800&&cp<=0xDFFF)) return false;
    }
    return true;
}
uint8_t Profile::controlTail() const {
    if(!validId(unit)||remote==unit) return 0;
    uint8_t tail=0;
    for(uint8_t k=0;k<setupflow::K_N;++k) {
        if(k==setupflow::K_TIMER) continue;
        const Mapping& m=keys[k];
        if(m.cmd!=0x22F1||m.len!=3||m.payload[0]!=0||m.payload[1]!=setupflow::modeOfKey(k)||
           (m.payload[2]!=4&&m.payload[2]!=7)||(tail&&tail!=m.payload[2])) return 0;
        tail=m.payload[2];
    }
    return tail;
}
const char* errorName(Error e) {
    switch(e) {
        case Error::None:return "none";case Error::Unavailable:return "profile-storage-unavailable";
        case Error::InvalidName:return "bad-profile-name";case Error::NameTaken:return "profile-name-taken";case Error::Incomplete:return "profile-incomplete";
        case Error::Full:return "profile-limit";case Error::Missing:return "profile-not-found";
        case Error::Exhausted:return "profile-id-exhausted";case Error::Write:return "profile-write-failed";
    }
    return "profile-error";
}
static uint32_t crc(const uint8_t* b,size_t n) {
    uint32_t v=0xFFFFFFFF;
    for(size_t i=0;i<n;++i){v^=b[i];for(unsigned j=0;j<8;++j)v=(v>>1)^(0xEDB88320u&uint32_t(-int32_t(v&1)));}
    return ~v;
}
static bool validProfile(const Profile& p) {
    if(!p.id||!memchr(p.name,0,sizeof(p.name))||!validName(p.name)||!validId(p.remote)) return false;
    if((p.unit.cls||p.unit.serial)&&(!validId(p.unit)||p.unit==p.remote)) return false;
    for(uint8_t k=0;k<setupflow::K_N;++k) {
        const Mapping& m=p.keys[k];
        if(!m.len||m.len>sizeof(m.payload))return false;
        if(k==setupflow::K_TIMER){if(m.cmd!=0x22F3)return false;}
        else if(m.cmd!=0x22F1||m.len<2||m.payload[0]!=0||m.payload[1]!=setupflow::modeOfKey(k))return false;
    }
    return true;
}
// Names pick the RF target in the UI: two identical names would make the choice ambiguous.
static bool nameTaken(const Snapshot& s,const char* name,uint32_t self) {
    for(uint8_t i=0;i<s.count;++i)if(s.items[i].id!=self&&!strcmp(s.items[i].name,name))return true;
    return false;
}
// Explicit little-endian format, independent of C++ padding / ESP32 ABI.
static size_t encode(const Snapshot& s,uint8_t* out) {
    size_t n=0; auto byte=[&](uint8_t v){out[n++]=v;};
    auto word=[&](uint32_t v){for(unsigned k=0;k<4;++k)byte(uint8_t(v>>(8*k)));};
    word(0x5052434F);byte(1);byte(s.count);word(s.next);word(s.active);
    for(uint8_t i=0;i<s.count;++i){const Profile& p=s.items[i];word(p.id);
        for(char c:p.name)byte(uint8_t(c));
        word(p.remote.raw());word(p.unit.raw());
        for(const Mapping& m:p.keys){byte(uint8_t(m.cmd));byte(uint8_t(m.cmd>>8));byte(m.len);for(uint8_t b:m.payload)byte(b);}
    }
    const uint32_t sum=crc(out,n);word(sum);return n;
}
static bool decode(const uint8_t* in,size_t size,Snapshot& s) {
    if(size<18||size>BLOB_BYTES)return false;
    size_t n=0; auto byte=[&](){return in[n++];};
    auto word=[&](){uint32_t v=0;for(unsigned k=0;k<4;++k)v|=uint32_t(byte())<<(8*k);return v;};
    if(word()!=0x5052434F||byte()!=1)return false;
    s.count=byte(); if(s.count>LIMIT||size!=18+size_t(s.count)*127)return false;
    s.next=word();s.active=word();bool active=s.active==0;
    bool badRaw=false;
    auto id=[&](){uint32_t raw=word();if(raw>0xFFFFFF)badRaw=true;return ramses::DeviceId(uint8_t(raw>>18),raw&0x3FFFF);};
    for(uint8_t i=0;i<s.count;++i){Profile& p=s.items[i];p.id=word();
        for(char& c:p.name)c=char(byte());
        p.remote=id();p.unit=id();
        for(Mapping& m:p.keys){m.cmd=byte();m.cmd|=uint16_t(byte())<<8;m.len=byte();for(uint8_t& b:m.payload)b=byte();}
        if(!validProfile(p)||p.id>=s.next)return false;
        for(uint8_t j=0;j<i;++j)if(s.items[j].id==p.id||s.items[j].remote==p.remote)return false;
        if(p.id==s.active)active=true;
    }
    const uint32_t expected=crc(in,n);return word()==expected&&!badRaw&&s.next&&active;
}
Store& store(){static Store value;return value;}
bool Store::begin(Storage& port) {
    storage_=&port;ready_=false;data_=Snapshot{};
    uint8_t blob[BLOB_BYTES];size_t len=0;
    if(!port.read(blob,sizeof(blob),len)||len>sizeof(blob))return false;
    Snapshot candidate;
    if(len&&!decode(blob,len,candidate))return false;
    data_=candidate;ready_=true;return true;
}
const Profile* Store::find(uint32_t id) const {for(uint8_t i=0;i<data_.count;++i)if(data_.items[i].id==id)return &data_.items[i];return nullptr;}
Error Store::commit(const Snapshot& next) {
    if(!ready_)return Error::Unavailable;
    uint8_t blob[BLOB_BYTES];const size_t len=encode(next,blob);
    if(!storage_->write(blob,len))return Error::Write;
    data_=next;return Error::None;
}
Error Store::save(const setupflow::Read& r,const char* name,uint32_t& id,bool activate) {
    if(!ready_)return Error::Unavailable;
    if(!validName(name))return Error::InvalidName;
    if(r.st!=setupflow::St::Done||r.step!=setupflow::K_N)return Error::Incomplete;
    Profile p;p.remote=r.remote;
    if(r.targetKnown&&!r.targetConflict)p.unit=r.target;
    memcpy(p.name,name,strlen(name)+1);
    for(uint8_t k=0;k<setupflow::K_N;++k){p.keys[k].cmd=r.got[k].cmd;p.keys[k].len=r.got[k].len;memcpy(p.keys[k].payload,r.got[k].pl,8);}
    p.id=1;if(!validProfile(p))return Error::Incomplete;
    Snapshot next=data_;uint8_t slot=0;while(slot<next.count&&!(next.items[slot].remote==p.remote))++slot;
    if(slot==next.count){if(next.count==LIMIT)return Error::Full;if(next.next==UINT32_MAX)return Error::Exhausted;p.id=next.next++;++next.count;}
    else p.id=next.items[slot].id; // one profile per observed remote; reread updates it
    if(nameTaken(data_,name,slot<data_.count?p.id:0))return Error::NameTaken;
    next.items[slot]=p;
    if(activate)next.active=p.id;
    uint8_t oldBlob[BLOB_BYTES],newBlob[BLOB_BYTES];const size_t a=encode(data_,oldBlob),b=encode(next,newBlob);
    const Error e=a==b&&!memcmp(oldBlob,newBlob,a)?Error::None:commit(next);
    if(e==Error::None)id=p.id;
    return e;
}
Error Store::select(uint32_t id) {
    if(!ready_)return Error::Unavailable;
    if(id&&!find(id))return Error::Missing;
    if(id==data_.active)return Error::None;
    Snapshot next=data_;next.active=id;
    return commit(next);
}
Error Store::rename(uint32_t id,const char* name) {
    if(!ready_)return Error::Unavailable;
    if(!validName(name))return Error::InvalidName;
    if(find(id)&&nameTaken(data_,name,id))return Error::NameTaken;
    Snapshot next=data_;
    for(uint8_t i=0;i<next.count;++i){
        Profile& p=next.items[i];
        if(p.id!=id)continue;
        if(!strcmp(p.name,name))return Error::None;
        memset(p.name,0,sizeof(p.name));memcpy(p.name,name,strlen(name)+1);
        return commit(next);
    }
    return Error::Missing;
}
Error Store::remove(uint32_t id) {
    if(!ready_)return Error::Unavailable;
    Snapshot next=data_;
    for(uint8_t i=0;i<next.count;++i){
        if(next.items[i].id!=id)continue;
        for(uint8_t j=i+1;j<next.count;++j)next.items[j-1]=next.items[j];
        --next.count;next.items[next.count]=Profile{};
        if(next.active==id)next.active=0;
        return commit(next);
    }
    return Error::Missing;
}
}
