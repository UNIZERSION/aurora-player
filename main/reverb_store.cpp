#include "reverb_store.h"
#include "player_store.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace player {
namespace {
constexpr uint32_t MAGIC=0x56525241; // ARRV
constexpr size_t RECORD_BYTES=32;
struct Record { audio_reverb_config_t config{}; uint64_t sequence=0; };
void put(uint8_t *p,uint64_t value,unsigned count){for(unsigned i=0;i<count;++i){p[i]=uint8_t(value);value>>=8;}}
uint64_t get(const uint8_t *p,unsigned count){uint64_t v=0;while(count)v=(v<<8)|p[--count];return v;}
void encode(const Record &r,uint8_t (&b)[RECORD_BYTES]){
    memset(b,0,sizeof b);put(b,MAGIC,4);put(b+4,2,4);put(b+8,r.sequence,8);
    put(b+16,r.config.decay_ms,2);put(b+18,r.config.tone_hz,2);b[20]=r.config.amount;
    put(b+21,r.config.low_cut_hz,2);
    put(b+28,crc32(b,28),4);
}
bool decode(const uint8_t (&b)[RECORD_BYTES],Record &out){
    const auto version=get(b+4,4);
    if(get(b,4)!=MAGIC||(version!=1&&version!=2)||!get(b+8,8)||get(b+28,4)!=crc32(b,28))return false;
    for(unsigned i=version==1?21:23;i<28;++i)if(b[i])return false;
    Record r;r.sequence=get(b+8,8);r.config.decay_ms=uint16_t(get(b+16,2));r.config.tone_hz=uint16_t(get(b+18,2));r.config.amount=b[20];
    if(version==1 && (r.config.tone_hz<600 || r.config.tone_hz>5000))return false;
    r.config.low_cut_hz=version==1?0:uint16_t(get(b+21,2));
    if(!audio_reverb_valid_config(&r.config))return false;
    out=r;return true;
}
bool read(const std::string &path,Record &out,bool &io_error,uint8_t *raw=nullptr){
    io_error=false;FILE *f=fopen(path.c_str(),"rb");if(!f){io_error=errno!=ENOENT;return false;}
    uint8_t bytes[RECORD_BYTES];bool ok=fread(bytes,1,sizeof bytes,f)==sizeof bytes&&fgetc(f)==EOF;
    if(ferror(f)){ok=false;io_error=true;}if(fclose(f)!=0){ok=false;io_error=true;}
    if(!ok||!decode(bytes,out))return false;
    if(raw)memcpy(raw,bytes,sizeof bytes);
    return true;
}
int newest(const std::string &dir,Record &out,bool &io_error){
    Record a,b;bool ea=false,eb=false;const bool va=read(dir+"/reverb-a.bin",a,ea),vb=read(dir+"/reverb-b.bin",b,eb);
    io_error=ea||eb;if(!va&&!vb)return -1;const int slot=vb&&(!va||b.sequence>a.sequence)?1:0;out=slot?b:a;return slot;
}
}
ReverbStore::ReverbStore(std::string directory):directory_(std::move(directory)){}
bool ReverbStore::load(audio_reverb_config_t &out){
    Record r;bool error=false;if(newest(directory_,r,error)<0)return false;out=r.config;sequence_=r.sequence;return true;
}
bool ReverbStore::save(const audio_reverb_config_t &config){
    if(!audio_reverb_valid_config(&config))return false;
#ifdef _WIN32
    if(_mkdir(directory_.c_str())!=0&&errno!=EEXIST)return false;
#else
    if(mkdir(directory_.c_str(),0755)!=0&&errno!=EEXIST)return false;
#endif
    Record best;bool error=false;const int active=newest(directory_,best,error);
    if(error||best.sequence==UINT64_MAX)return false;
    Record next;next.config=config;next.sequence=best.sequence+1;
    uint8_t bytes[RECORD_BYTES];encode(next,bytes);
    const auto path=directory_+(active==0?"/reverb-b.bin":"/reverb-a.bin");
    FILE *f=fopen(path.c_str(),"wb");if(!f)return false;
    bool ok=fwrite(bytes,1,sizeof bytes,f)==sizeof bytes&&fflush(f)==0;
#ifdef _WIN32
    if(ok)ok=_commit(_fileno(f))==0;
#else
    if(ok)ok=fsync(fileno(f))==0;
#endif
    if(fclose(f)!=0)ok=false;
    Record verified;uint8_t back[RECORD_BYTES];
    if(ok)ok=read(path,verified,error,back)&&!memcmp(bytes,back,sizeof bytes);
    if(ok)sequence_=next.sequence;
    return ok;
}
}
