#include "eq_store.h"
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
constexpr uint32_t MAGIC=0x51455241; // AREQ, little endian
constexpr size_t HEADER_BYTES=24;
constexpr size_t CONFIG_BYTES=AUDIO_EQ_BANDS+2;
constexpr size_t PAYLOAD_BYTES=4+(EQ_PRESET_COUNT+1)*CONFIG_BYTES;
constexpr size_t RECORD_BYTES=HEADER_BYTES+PAYLOAD_BYTES+4;
static_assert(AUDIO_EQ_BANDS==11 && EQ_PRESET_COUNT==4,"Version 1 EQ wire layout changed");
struct Record { EqBank bank; uint64_t sequence=0; };

void put_le(uint8_t *to,uint64_t value,size_t bytes) {
    for(size_t i=0;i<bytes;++i){to[i]=static_cast<uint8_t>(value);value>>=8;}
}
uint64_t get_le(const uint8_t *from,size_t bytes) {
    uint64_t value=0;
    for(size_t i=bytes;i;--i)value=(value<<8)|from[i-1];
    return value;
}
bool sane(const EqBank &bank) {
    if(bank.selected>=EQ_PRESET_COUNT || bank.saved_mask>=(1U<<EQ_PRESET_COUNT) ||
       !audio_eq_valid_config(&bank.current))return false;
    for(const auto &preset:bank.presets)if(!audio_eq_valid_config(&preset))return false;
    return true;
}
void encode_config(uint8_t *p,const audio_eq_config_t &config) {
    for(unsigned i=0;i<AUDIO_EQ_BANDS;++i)p[i]=static_cast<uint8_t>(config.gain_db[i]);
    p[AUDIO_EQ_BANDS]=static_cast<uint8_t>(config.preamp_db);
    p[AUDIO_EQ_BANDS+1]=config.enabled;
}
int8_t signed_byte(uint8_t value) {
    return static_cast<int8_t>(value<=127?int(value):int(value)-256);
}
void decode_config(const uint8_t *p,audio_eq_config_t &config) {
    for(unsigned i=0;i<AUDIO_EQ_BANDS;++i)config.gain_db[i]=signed_byte(p[i]);
    config.preamp_db=signed_byte(p[AUDIO_EQ_BANDS]);
    config.enabled=p[AUDIO_EQ_BANDS+1];
}
void encode(const Record &record,uint8_t (&bytes)[RECORD_BYTES]) {
    memset(bytes,0,sizeof bytes);
    put_le(bytes,MAGIC,4);put_le(bytes+4,1,4);put_le(bytes+8,record.sequence,8);
    put_le(bytes+16,PAYLOAD_BYTES,4);
    auto *p=bytes+HEADER_BYTES;
    p[0]=record.bank.selected;p[1]=record.bank.saved_mask;p[2]=AUDIO_EQ_BANDS;p[3]=EQ_PRESET_COUNT;
    encode_config(p+4,record.bank.current);
    for(unsigned i=0;i<EQ_PRESET_COUNT;++i)encode_config(p+4+(i+1)*CONFIG_BYTES,record.bank.presets[i]);
    put_le(bytes+RECORD_BYTES-4,crc32(bytes,RECORD_BYTES-4),4);
}
bool decode(const uint8_t (&bytes)[RECORD_BYTES],Record &record) {
    if(get_le(bytes,4)!=MAGIC || get_le(bytes+4,4)!=1 || !get_le(bytes+8,8) ||
       get_le(bytes+16,4)!=PAYLOAD_BYTES || get_le(bytes+20,4)!=0 ||
       get_le(bytes+RECORD_BYTES-4,4)!=crc32(bytes,RECORD_BYTES-4))return false;
    const auto *p=bytes+HEADER_BYTES;
    if(p[2]!=AUDIO_EQ_BANDS || p[3]!=EQ_PRESET_COUNT)return false;
    Record decoded;
    decoded.sequence=get_le(bytes+8,8);
    decoded.bank.selected=p[0];decoded.bank.saved_mask=p[1];
    decode_config(p+4,decoded.bank.current);
    for(unsigned i=0;i<EQ_PRESET_COUNT;++i)decode_config(p+4+(i+1)*CONFIG_BYTES,decoded.bank.presets[i]);
    if(!sane(decoded.bank))return false;
    record=decoded;return true;
}
bool read_record(const std::string &path,Record &out,uint8_t *raw=nullptr,bool *io_error=nullptr) {
    if(io_error)*io_error=false;
    FILE *file=fopen(path.c_str(),"rb");
    if(!file){if(io_error&&errno!=ENOENT)*io_error=true;return false;}
    uint8_t bytes[RECORD_BYTES];
    bool ok=fread(bytes,1,sizeof bytes,file)==sizeof bytes && fgetc(file)==EOF;
    if(ferror(file)){ok=false;if(io_error)*io_error=true;}
    if(fclose(file)!=0){ok=false;if(io_error)*io_error=true;}
    if(!ok || !decode(bytes,out))return false;
    if(raw)memcpy(raw,bytes,sizeof bytes);
    return true;
}
int newest(const std::string &directory,Record &best,bool *io_error=nullptr) {
    Record a,b;
    bool ea=false,eb=false;
    const bool va=read_record(directory+"/eq-a.bin",a,nullptr,&ea);
    const bool vb=read_record(directory+"/eq-b.bin",b,nullptr,&eb);
    if(io_error)*io_error=ea||eb;
    if(!va&&!vb)return -1;
    const int slot=vb&&(!va||b.sequence>a.sequence)?1:0;
    best=slot?b:a;return slot;
}
}
EqStore::EqStore(std::string directory):directory_(std::move(directory)) {}
bool EqStore::load(EqBank &out) {
    Record best;
    if(newest(directory_,best)<0)return false;
    out=best.bank;sequence_=best.sequence;return true;
}
bool EqStore::save(const EqBank &bank) {
    if(!sane(bank))return false;
#ifdef _WIN32
    if(_mkdir(directory_.c_str())!=0&&errno!=EEXIST)return false;
#else
    if(mkdir(directory_.c_str(),0755)!=0&&errno!=EEXIST)return false;
#endif
    // Rediscover the authoritative slot before each write. This also protects
    // fresh store objects and retries after a failed sync/readback: a complete
    // candidate from that attempt may already be valid on the card.
    Record best;
    bool io_error=false;
    const int active=newest(directory_,best,&io_error);
    if(io_error)return false; // Never overwrite a slot whose read merely failed.
    if(best.sequence==UINT64_MAX)return false;
    const int target=active==0?1:0;
    Record record;record.bank=bank;record.sequence=best.sequence+1;
    uint8_t bytes[RECORD_BYTES];encode(record,bytes);
    const std::string path=directory_+(target?"/eq-b.bin":"/eq-a.bin");
    FILE *file=fopen(path.c_str(),"wb");
    if(!file)return false;
    bool ok=fwrite(bytes,1,sizeof bytes,file)==sizeof bytes && fflush(file)==0;
#ifdef _WIN32
    if(ok)ok=_commit(_fileno(file))==0;
#else
    if(ok)ok=fsync(fileno(file))==0;
#endif
    if(fclose(file)!=0)ok=false;
    Record verified;uint8_t readback[RECORD_BYTES];
    if(ok)ok=read_record(path,verified,readback) && !memcmp(bytes,readback,sizeof bytes);
    if(ok)sequence_=record.sequence;
    return ok;
}
}
