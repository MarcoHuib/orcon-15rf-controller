// Bounded, versioned remote profiles. Observations are never transmitter identities
// or pairing credentials. Persistence is injected so the same code is host-tested.
#pragma once
#include "setup_flow.h"
namespace profiles {
constexpr size_t LIMIT=4, NAME_BYTES=48, BLOB_BYTES=640;
struct Mapping { uint16_t cmd=0; uint8_t len=0, payload[8]={}; };
struct Profile {
    uint32_t id=0;
    char name[NAME_BYTES+1]={};
    ramses::DeviceId remote{0,0}, unit{0,0};
    Mapping keys[setupflow::K_N];
    uint8_t controlTail() const; // 0 = incomplete/unsupported; timer is never replayed
};
struct Snapshot { uint32_t next=1, active=0; uint8_t count=0; Profile items[LIMIT]; };
class Storage {
public:
    virtual ~Storage()=default;
    // Missing key is success with len=0. Errors must return false, never empty data.
    virtual bool read(uint8_t*,size_t cap,size_t& len)=0;
    virtual bool write(const uint8_t*,size_t len)=0; // one atomic NVS blob replacement
};
enum class Error { None, Unavailable, InvalidName, NameTaken, Incomplete, Full, Missing, Exhausted, Write };
const char* errorName(Error);
bool validName(const char*);
class Store {
public:
    bool begin(Storage&);
    bool ready() const { return ready_; }
    const Snapshot& data() const { return data_; }
    const Profile* find(uint32_t) const;
    const Profile* active() const { return find(data_.active); }
    // activate: the saved profile also becomes the active one, in the SAME NVS write
    // (never "saved but not active"). Selecting never pairs or transmits.
    Error save(const setupflow::Read&,const char* name,uint32_t& id,bool activate=false);
    Error select(uint32_t);
    Error rename(uint32_t,const char*);
    Error remove(uint32_t);
private:
    Error commit(const Snapshot&);
    Storage* storage_=nullptr; bool ready_=false; Snapshot data_;
};
Store& store();
}
