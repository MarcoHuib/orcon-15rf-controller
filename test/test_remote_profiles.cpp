#include "remote_profiles.h"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace profiles;using namespace setupflow;using namespace ramses;
static int checks=0,fails=0;
#define CHECK(x) do{++checks;if(!(x)){++fails;printf("FAIL %d: %s\n",__LINE__,#x);}}while(0)
struct Memory : Storage {
    std::vector<uint8_t> blob;bool readOk=true,writeOk=true;int writes=0;
    bool read(uint8_t* out,size_t cap,size_t& len)override{if(!readOk||blob.size()>cap)return false;len=blob.size();if(len)memcpy(out,blob.data(),len);return true;}
    bool write(const uint8_t* in,size_t len)override{++writes;if(!writeOk)return false;blob.assign(in,in+len);return true;}
};
static Read capture(uint32_t serial=101) {
    Read r;r.start(7,DeviceId(29,serial),1000,1);
    for(uint8_t k=0;k<K_N;++k){Frame f;f.type=MsgType::I;f.addr[0]=r.remote;f.addr[1]=DeviceId(29,202);f.has_addr[2]=false;
        f.cmd=k==K_TIMER?0x22F3:0x22F1;f.len=k==K_TIMER?7:3;
        f.payload[1]=k==K_TIMER?0x12:modeOfKey(k);f.payload[2]=k==K_TIMER?30:4;
        r.onFrame(f,k+1,k+2,1400+k*400,-60);
    }return r;
}
int main(){
    Memory mem;Store s;CHECK(s.begin(mem)&&s.ready()&&s.data().count==0);uint32_t id=0;
    Read r=capture();CHECK(r.st==St::Done&&r.targetKnown&&!r.targetConflict);
    CHECK(s.save(r,"Ventilatie woonkamer",id)==Error::None&&id==1);
    CHECK(s.data().count==1&&s.data().active==0&&s.active()==nullptr); // save never selects/pairs/sends
    const Profile* p=s.find(id);CHECK(p&&p->remote==r.remote&&p->unit==r.target&&p->controlTail()==4);
    CHECK(p->keys[K_TIMER].cmd==0x22F3&&p->keys[K_TIMER].len==7&&p->keys[K_TIMER].payload[2]==30);
    CHECK(s.save(r,"Ventilatie woonkamer",id)==Error::None&&mem.writes==1); // duplicate save idempotent
    CHECK(s.select(id)==Error::None&&s.active()->id==id);
    Store reboot;CHECK(reboot.begin(mem)&&reboot.active()&&reboot.active()->id==id);
    CHECK(!strcmp(reboot.active()->name,"Ventilatie woonkamer")&&reboot.active()->controlTail()==4);
    CHECK(reboot.rename(id,"Ventilatie keuken \"A\"")==Error::None);
    Store again;CHECK(again.begin(mem)&&!strcmp(again.active()->name,"Ventilatie keuken \"A\""));
    const int before=mem.writes;CHECK(again.select(id)==Error::None&&mem.writes==before);
    mem.writeOk=false;CHECK(again.rename(id,"Niet opgeslagen")==Error::Write);
    CHECK(!strcmp(again.active()->name,"Ventilatie keuken \"A\""));
    CHECK(again.select(0)==Error::Write&&again.data().active==id);
    CHECK(again.remove(id)==Error::Write&&again.find(id));mem.writeOk=true;
    {Read other=capture(102);uint32_t added=0;const int w=mem.writes;   // duplicate names are ambiguous targets
     CHECK(again.save(other,"Ventilatie keuken \"A\"",added)==Error::NameTaken&&again.data().count==1&&mem.writes==w);}
    for(uint32_t i=1;i<LIMIT;++i){Read next=capture(101+i);uint32_t added=0;char name[24];snprintf(name,sizeof(name),"Andere ventilatie %u",unsigned(i));
        CHECK(again.save(next,name,added)==Error::None&&added==i+1);}
    {const int w=mem.writes;CHECK(again.rename(2,"Andere ventilatie 2")==Error::NameTaken&&mem.writes==w&&!strcmp(again.find(2)->name,"Andere ventilatie 1"));
     CHECK(again.rename(2,"Andere ventilatie 1")==Error::None&&mem.writes==w);            // own name: unchanged, no write
     uint32_t same=0;CHECK(again.save(capture(102),"Andere ventilatie 1",same)==Error::None&&same==2); // reread keeps its own name
     CHECK(again.rename(999,"Andere ventilatie 2")==Error::Missing);}
    Read fifth=capture(109);CHECK(again.save(fifth,"Vijfde",id)==Error::Full);
    CHECK(again.rename(999,"Ontbreekt")==Error::Missing&&again.remove(999)==Error::Missing&&again.select(999)==Error::Missing);
    CHECK(again.remove(1)==Error::None&&!again.active()&&again.data().active==0);
    CHECK(again.save(fifth,"Vijfde",id)==Error::None&&id==5); // deleted IDs never recycled
    Store restored;CHECK(restored.begin(mem)&&restored.data().count==LIMIT&&!restored.find(1));
    for(const char* bad:{""," "," naam","naam ","x\n","\xC0\xAF","\xED\xA0\x80","\xF4\x90\x80\x80"})CHECK(!validName(bad));
    CHECK(validName("Slaapkamer één"));char longName[NAME_BYTES+2];memset(longName,'a',sizeof(longName));longName[sizeof(longName)-1]=0;CHECK(!validName(longName));
    CHECK(restored.rename(2,"")==Error::InvalidName);
    {Memory m;Store a;CHECK(a.begin(m));Read incomplete=r;incomplete.st=St::Active;CHECK(a.save(incomplete,"Naam",id)==Error::Incomplete&&m.writes==0);
     incomplete=r;incomplete.got[2].len=9;CHECK(a.save(incomplete,"Naam",id)==Error::Incomplete); // never store truncated bytes
     incomplete=r;incomplete.remote=DeviceId(63,0x3FFFF);CHECK(a.save(incomplete,"Naam",id)==Error::Incomplete);
     incomplete=r;incomplete.got[4].pl[1]=3;CHECK(a.save(incomplete,"Naam",id)==Error::Incomplete);
     Read unsupported=r;unsupported.got[4].pl[2]=7;CHECK(a.save(unsupported,"Gemengd",id)==Error::None&&a.find(id)->controlTail()==0);
     Read unknown=capture(102);unknown.targetConflict=true;CHECK(a.save(unknown,"Geen doel",id)==Error::None&&a.find(id)->controlTail()==0);
    }
    {Memory broken=mem;broken.blob[20]^=1;Store a;CHECK(!a.begin(broken)&&!a.ready());CHECK(a.select(0)==Error::Unavailable&&a.save(r,"Naam",id)==Error::Unavailable&&broken.writes==mem.writes);}
    {Memory broken=mem;broken.blob.resize(20);Store a;CHECK(!a.begin(broken));}
    {Memory broken=mem;broken.blob[4]=99;Store a;CHECK(!a.begin(broken));}
    {Memory broken;broken.readOk=false;Store a;CHECK(!a.begin(broken)&&!a.ready());}
    {Memory broken;broken.writeOk=false;Store a;CHECK(a.begin(broken));CHECK(a.save(r,"Naam",id)==Error::Write&&a.data().count==0);}
    printf("Remote profiles: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
