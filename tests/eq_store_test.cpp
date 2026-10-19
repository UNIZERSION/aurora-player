#ifdef NDEBUG
#undef NDEBUG
#endif
#include "eq_store.h"
#include "player_store.h"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <utility>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#endif

// Inject failures at actual C stdio boundaries of the production store. The
// production file is compiled unchanged; no runtime test hooks enter firmware.
namespace fault {
enum Mode { None, OpenWrite, ShortWrite, Flush, Sync, CloseWrite, VerifyOpen,
            ScanOpen, ScanRead, CorruptAfterClose };
static Mode mode=None;
static FILE *writer=nullptr,*read_failure=nullptr;
static std::string path;
static bool wrote=false,failed=false;
static void set(Mode m){mode=m;writer=read_failure=nullptr;path.clear();wrote=failed=false;}
static FILE *open(const char *p,const char *m){
    if((mode==OpenWrite&&m[0]=='w') ||
       (mode==VerifyOpen&&wrote&&m[0]=='r') ||
       (mode==ScanOpen&&!wrote&&m[0]=='r')){failed=true;errno=EIO;return nullptr;}
    FILE *f=std::fopen(p,m);
    if(f&&m[0]=='w'){writer=f;path=p;wrote=true;}
    if(f&&mode==ScanRead&&!wrote&&m[0]=='r')read_failure=f;
    return f;
}
static size_t read(void *p,size_t s,size_t n,FILE *f){if(f==read_failure){failed=true;return 0;}return std::fread(p,s,n,f);}
static int error(FILE *f){return f==read_failure?1:std::ferror(f);}
static size_t write(const void *p,size_t s,size_t n,FILE *f){
    if(mode==ShortWrite&&f==writer){failed=true;return std::fwrite(p,s,n/2,f);}
    return std::fwrite(p,s,n,f);
}
static int flush(FILE *f){const int result=std::fflush(f);if(mode==Flush&&f==writer){failed=true;return EOF;}return result;}
static int sync(int descriptor){
    if(mode==Sync){failed=true;errno=EIO;return -1;}
#ifdef _WIN32
    return _commit(descriptor);
#else
    return fsync(descriptor);
#endif
}
static int close(FILE *f){
    const bool writing=f==writer;
    if(f==read_failure)read_failure=nullptr;
    const int result=std::fclose(f);
    if(writing){
        writer=nullptr;
        if(mode==CorruptAfterClose){FILE *edit=std::fopen(path.c_str(),"r+b");assert(edit);int c=std::fgetc(edit);assert(c!=EOF);assert(std::fseek(edit,0,SEEK_SET)==0);assert(std::fputc(c^1,edit)!=EOF);assert(std::fclose(edit)==0);failed=true;}
        if(mode==CloseWrite){failed=true;return EOF;}
    }
    return result;
}
}
#define fopen fault::open
#define fread fault::read
#define fwrite fault::write
#define ferror fault::error
#define fflush fault::flush
#define fclose fault::close
#ifdef _WIN32
#define _commit fault::sync
#else
#define fsync fault::sync
#endif
#include "../main/eq_store.cpp"
#undef fopen
#undef fread
#undef fwrite
#undef ferror
#undef fflush
#undef fclose
#ifdef _WIN32
#undef _commit
#else
#undef fsync
#endif

namespace fs=std::filesystem;
using Bytes=std::vector<uint8_t>;
static Bytes read_bytes(const fs::path &path){std::ifstream f(path,std::ios::binary);assert(f);return Bytes(std::istreambuf_iterator<char>(f),{});}
static void write_bytes(const fs::path &path,const Bytes &bytes){std::ofstream f(path,std::ios::binary|std::ios::trunc);assert(f);f.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());f.close();assert(f.good());}
static void le(Bytes &b,size_t at,uint64_t value,size_t n){for(size_t i=0;i<n;++i){b.at(at+i)=uint8_t(value);value>>=8;}}
static void seal(Bytes &b){le(b,b.size()-4,player::crc32(b.data(),b.size()-4),4);}
static bool equal_config(const audio_eq_config_t &a,const audio_eq_config_t &b){return a.enabled==b.enabled&&a.preamp_db==b.preamp_db&&!memcmp(a.gain_db,b.gain_db,AUDIO_EQ_BANDS);}
static bool equal(const player::EqBank &a,const player::EqBank &b){
    if(a.selected!=b.selected||a.saved_mask!=b.saved_mask||!equal_config(a.current,b.current))return false;
    for(unsigned i=0;i<player::EQ_PRESET_COUNT;++i)if(!equal_config(a.presets[i],b.presets[i]))return false;
    return true;
}
static player::EqBank bank(unsigned value){
    player::EqBank b;b.selected=value%4;b.saved_mask=uint8_t(value%16);
    for(unsigned c=0;c<=4;++c){auto &config=c?b.presets[c-1]:b.current;config.enabled=(value+c)%2;config.preamp_db=-int8_t((value+c)%25);for(unsigned i=0;i<AUDIO_EQ_BANDS;++i)config.gain_db[i]=int8_t((value+c+i)%25)-12;}
    return b;
}
static void recovered(const fs::path &path,const player::EqBank &expected,uint64_t sequence){
    player::EqStore reader(path.string());player::EqBank out=bank(88);
    assert(reader.load(out)&&reader.sequence()==sequence&&equal(out,expected));
}
int main(int argc,char **argv){
    assert(argc==2);const fs::path root=argv[1];fs::create_directories(root);
    static_assert(sizeof(player::Settings)==1312,"Released Settings ABI must remain unchanged");
    const auto first=bank(1),second=bank(2),third=bank(3);
    player::EqBank defaults;assert(defaults.selected==0&&defaults.saved_mask==0);
    for(unsigned c=0;c<=4;++c){const auto &v=c?defaults.presets[c-1]:defaults.current;assert(v.enabled==1&&v.preamp_db==AUDIO_MASTER_GAIN_DB);for(int gain:v.gain_db)assert(gain==0);}
    player::EqStore missing((root/"missing").string());auto unchanged=first;assert(!missing.load(unchanged)&&equal(unchanged,first));
    auto new_bank=defaults;assert(!missing.load(new_bank)&&equal(new_bank,defaults));
    assert(missing.save(defaults));recovered(root/"missing",defaults,1);

    // Runtime policy must not migrate any saved field in the storage layer.
    // Test the old 0/-12/-24 dB records, including all four occupied slots.
    for(int legacy_db:{0,-12,-24}){
        const auto legacy_path=root/("legacy"+std::to_string(legacy_db));player::EqStore legacy(legacy_path.string());
        auto legacy_bank=first;legacy_bank.saved_mask=15;legacy_bank.current.preamp_db=int8_t(legacy_db);
        for(auto &preset:legacy_bank.presets)preset.preamp_db=int8_t(legacy_db);
        assert(legacy.save(legacy_bank));const auto legacy_bytes=read_bytes(legacy_path/"eq-a.bin");
        assert(legacy_bytes.size()==97);for(unsigned i=0;i<=4;++i)assert(legacy_bytes[39+13*i]==uint8_t(legacy_db));
        player::EqBank legacy_loaded;assert(legacy_loaded.current.preamp_db==AUDIO_MASTER_GAIN_DB);
        assert(legacy.load(legacy_loaded)&&equal(legacy_loaded,legacy_bank));
        assert(read_bytes(legacy_path/"eq-a.bin")==legacy_bytes);
    }

    const auto basic=root/"basic";player::EqStore store(basic.string());assert(store.save(first)&&store.sequence()==1);assert(store.save(second)&&store.sequence()==2);
    const Bytes old=read_bytes(basic/"eq-a.bin"),latest=read_bytes(basic/"eq-b.bin");
    assert(old.size()==97&&latest.size()==97&&latest[0]=='A'&&latest[1]=='R'&&latest[2]=='E'&&latest[3]=='Q'&&latest[4]==1&&latest[8]==2&&latest[16]==69&&latest[26]==11&&latest[27]==4);
    recovered(basic,second,2);
    player::EqStore fresh(basic.string());assert(fresh.save(third)&&fresh.sequence()==3);assert(read_bytes(basic/"eq-b.bin")==latest);recovered(basic,third,3);
    write_bytes(basic/"eq-a.bin",old);
    puts("PASS EQ store: flat enabled defaults use player gain policy, legacy 0/-12/-24 dB working curves+4presets loaded unchanged, separate Settings ABI, canonical97-byte LE format, working curve+4slots round trip, fresh saver preserves newest slot.");

    // Every interrupted-write length and one-bit corruption position must fall
    // back to the untouched old bank, never a mix of new/old slots.
    for(size_t cut=0;cut<latest.size();++cut){write_bytes(basic/"eq-b.bin",Bytes(latest.begin(),latest.begin()+cut));recovered(basic,first,1);}
    for(size_t pos=0;pos<latest.size();++pos){auto damaged=latest;damaged[pos]^=1;write_bytes(basic/"eq-b.bin",damaged);recovered(basic,first,1);}
    auto appended=latest;appended.push_back(0);write_bytes(basic/"eq-b.bin",appended);recovered(basic,first,1);
    // Invalid semantic values stay invalid even with a correctly forged CRC.
    const std::pair<size_t,uint8_t> corrupt[]={{0,0},{4,2},{8,0},{16,68},{20,1},{24,4},{25,16},{26,10},{27,3},{28,13},{28,243},{39,1},{39,231},{40,2},{41,13},{92,2}};
    for(auto item:corrupt){auto damaged=latest;damaged[item.first]=item.second;seal(damaged);write_bytes(basic/"eq-b.bin",damaged);recovered(basic,first,1);}
    write_bytes(basic/"eq-a.bin",{});write_bytes(basic/"eq-b.bin",{});player::EqStore broken(basic.string());unchanged=third;assert(!broken.load(unchanged)&&equal(unchanged,third));
    puts("PASS EQ recovery: all97 truncation lengths,97 byte corruption positions, trailing data and CRC-valid invalid schema/ranges rejected; failed load preserves RAM bank.");

    // Input validation is checked before even creating/opening any slot.
    write_bytes(basic/"eq-a.bin",old);write_bytes(basic/"eq-b.bin",latest);
    std::vector<player::EqBank> invalid;
    auto b=first;b.selected=4;invalid.push_back(b);b=first;b.saved_mask=16;invalid.push_back(b);
    b=first;b.current.gain_db[0]=13;invalid.push_back(b);b=first;b.current.gain_db[10]=-13;invalid.push_back(b);
    b=first;b.current.preamp_db=1;invalid.push_back(b);b=first;b.current.preamp_db=-25;invalid.push_back(b);b=first;b.current.enabled=2;invalid.push_back(b);
    for(unsigned i=0;i<4;i++){b=first;b.presets[i].gain_db[5]=127;invalid.push_back(b);}
    for(const auto &bad:invalid){assert(!store.save(bad));assert(read_bytes(basic/"eq-a.bin")==old&&read_bytes(basic/"eq-b.bin")==latest);}
    auto overflow=latest;le(overflow,8,UINT64_MAX,8);seal(overflow);write_bytes(basic/"eq-b.bin",overflow);player::EqStore full(basic.string());unchanged=first;assert(full.load(unchanged)&&equal(unchanged,second)&&full.sequence()==UINT64_MAX);assert(!full.save(third));assert(read_bytes(basic/"eq-b.bin")==overflow&&read_bytes(basic/"eq-a.bin")==old);
    auto tie=latest;le(tie,8,1,8);seal(tie);write_bytes(basic/"eq-b.bin",tie);recovered(basic,first,1);
    puts("PASS EQ validation: invalid working/preset configurations cause no writes, uint64 sequence exhaustion refuses overwrite, equal sequences select deterministic A slot.");

    for(int m=fault::OpenWrite;m<=fault::CorruptAfterClose;++m){
        const auto path=root/("fault-"+std::to_string(m));player::EqStore subject(path.string());assert(subject.save(first));const Bytes protected_before=read_bytes(path/"eq-a.bin");
        fault::set(static_cast<fault::Mode>(m));assert(!subject.save(second)&&fault::failed&&subject.sequence()==1);fault::set(fault::None);
        assert(read_bytes(path/"eq-a.bin")==protected_before);
        player::EqStore reader(path.string());player::EqBank loaded;assert(reader.load(loaded));assert(equal(loaded,first)||equal(loaded,second));
        const uint64_t valid_sequence=reader.sequence();const auto protected_path=path/(valid_sequence==1?"eq-a.bin":"eq-b.bin");const auto newest_before=read_bytes(protected_path);
        assert(subject.save(third)&&subject.sequence()==valid_sequence+1);assert(read_bytes(protected_path)==newest_before);recovered(path,third,valid_sequence+1);
    }
    const auto nested=root/"absent"/"parent";player::EqStore no_parent(nested.string());assert(!no_parent.save(first));
    const auto not_dir=root/"not-a-dir";write_bytes(not_dir,{1,2,3});player::EqStore file_as_dir(not_dir.string());assert(!file_as_dir.save(first));assert(read_bytes(not_dir)==Bytes({1,2,3}));
    puts("PASS EQ failure atomicity: open/short-write/flush/sync/close/verify-read/scan-open/scan-read/corrupt-readback faults preserve prior bank; retry rescans and preserves newest valid candidate; missing parent and non-directory fail safely.");
    puts("Scope: production persistence with real host filesystem and injected C I/O faults; not physical SD power-cut guarantees.");
}
