#include "favorites_store.h"
#include "player_store.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace player {
namespace {
constexpr uint32_t MAGIC = 0x56464154; // TAFV
constexpr size_t HEADER_SIZE = 24;
void put_le(uint8_t *to, uint64_t value, size_t bytes) {
    for (size_t i=0;i<bytes;++i) { to[i]=static_cast<uint8_t>(value);value>>=8; }
}
uint64_t get_le(const uint8_t *from, size_t bytes) {
    uint64_t value=0;
    for (size_t i=bytes;i;--i) value=(value<<8)|from[i-1];
    return value;
}
uint32_t extend_crc(uint32_t crc, const void *data, size_t bytes) {
    const auto *p=static_cast<const uint8_t *>(data);
    while(bytes--) {
        crc^=*p++;
        for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^(0xedb88320U&(0U-(crc&1U)));
    }
    return crc;
}
bool write_crc(FILE *file, const void *data, size_t bytes, uint32_t &crc) {
    if(fwrite(data,1,bytes,file)!=bytes)return false;
    crc=extend_crc(crc,data,bytes);return true;
}
bool sync_file(FILE *file) {
    if(fflush(file)!=0)return false;
#ifdef _WIN32
    return _commit(_fileno(file))==0;
#else
    return fsync(fileno(file))==0;
#endif
}
}
FavoritesStore::FavoritesStore(std::string directory,std::string music_root)
    : directory_(std::move(directory)),music_root_(std::move(music_root)) {}
bool FavoritesStore::valid_path(const std::string &path) const {
    return !music_root_.empty() && path.size()>music_root_.size()+1 &&
        path.back()!='/' && path.find("//")==std::string::npos && valid_music_path(path,music_root_);
}
bool FavoritesStore::contains(const std::string &path) const {
    return std::binary_search(paths_.begin(),paths_.end(),path);
}
bool FavoritesStore::set(const std::string &path,bool favorite) {
    if(!valid_path(path))return false;
    const auto found=std::lower_bound(paths_.begin(),paths_.end(),path);
    const bool present=found!=paths_.end() && *found==path;
    if(present==favorite)return true;
    if(favorite) {
        if(paths_.size()>=MAX_FAVORITES)return false;
        paths_.insert(found,path);
    } else paths_.erase(found);
    dirty_=true;return true;
}
bool FavoritesStore::toggle(const std::string &path) { return set(path,!contains(path)); }

bool FavoritesStore::read_slot(const std::string &path,std::vector<std::string> &out,uint64_t &sequence) const {
    FILE *file=fopen(path.c_str(),"rb");
    if(!file)return false;
    uint8_t header[HEADER_SIZE]{};
    bool ok=fread(header,1,sizeof header,file)==sizeof header;
    const auto count=static_cast<uint32_t>(get_le(header+16,4));
    const auto payload=static_cast<uint32_t>(get_le(header+20,4));
    const auto saved_sequence=get_le(header+8,8);
    ok=ok && get_le(header,4)==MAGIC && get_le(header+4,4)==1 && saved_sequence &&
        count<=MAX_FAVORITES && payload<=count*(PATH_LIMIT+1) && payload>=count*3;
    std::vector<std::string> loaded;
    if(ok)loaded.reserve(count);
    uint32_t crc=extend_crc(0xffffffffU,header,sizeof header),consumed=0;
    for(uint32_t i=0;ok && i<count;++i) {
        uint8_t length_bytes[2];
        if(consumed+2>payload || fread(length_bytes,1,2,file)!=2) {ok=false;break;}
        consumed+=2;crc=extend_crc(crc,length_bytes,2);
        const size_t length=static_cast<size_t>(get_le(length_bytes,2));
        if(!length || length>=PATH_LIMIT || length>payload-consumed) {ok=false;break;}
        char value[PATH_LIMIT];
        if(fread(value,1,length,file)!=length) {ok=false;break;}
        consumed+=static_cast<uint32_t>(length);crc=extend_crc(crc,value,length);
        std::string item(value,length);
        if(!valid_path(item) || (!loaded.empty() && loaded.back()>=item)) {ok=false;break;}
        loaded.push_back(std::move(item));
    }
    uint8_t checksum[4]{};
    if(ok)ok=consumed==payload && fread(checksum,1,4,file)==4 &&
        get_le(checksum,4)==~crc && fgetc(file)==EOF && !ferror(file);
    if(fclose(file)!=0)ok=false;
    if(!ok)return false;
    out=std::move(loaded);sequence=saved_sequence;return true;
}
bool FavoritesStore::load() {
    std::vector<std::string> a,b;
    uint64_t sa=0,sb=0;
    const bool va=read_slot(directory_+"/favorites-a.bin",a,sa);
    const bool vb=read_slot(directory_+"/favorites-b.bin",b,sb);
    if(!va && !vb)return false;
    active_=vb && (!va || sb>sa)?1:0;
    paths_=active_?std::move(b):std::move(a);sequence_=active_?sb:sa;dirty_=false;
    return true;
}
bool FavoritesStore::save() {
    if(!dirty_)return true;
    if(sequence_==UINT64_MAX)return false;
#ifdef _WIN32
    if(_mkdir(directory_.c_str())!=0 && errno!=EEXIST)return false;
#else
    if(mkdir(directory_.c_str(),0755)!=0 && errno!=EEXIST)return false;
#endif
    uint32_t payload=0;
    for(const auto &path:paths_)payload+=static_cast<uint32_t>(path.size()+2);
    uint8_t header[HEADER_SIZE]{};
    put_le(header,MAGIC,4);put_le(header+4,1,4);put_le(header+8,sequence_+1,8);
    put_le(header+16,paths_.size(),4);put_le(header+20,payload,4);
    const int target=active_==0?1:0;
    const std::string file_path=directory_+(target?"/favorites-b.bin":"/favorites-a.bin");
    FILE *file=fopen(file_path.c_str(),"wb");
    if(!file)return false;
    uint32_t crc=0xffffffffU;
    bool ok=write_crc(file,header,sizeof header,crc);
    for(const auto &path:paths_) {
        if(!ok)break;
        uint8_t length[2];put_le(length,path.size(),2);
        ok=write_crc(file,length,2,crc) && write_crc(file,path.data(),path.size(),crc);
    }
    uint8_t checksum[4];put_le(checksum,~crc,4);
    if(ok)ok=fwrite(checksum,1,sizeof checksum,file)==sizeof checksum && sync_file(file);
    if(fclose(file)!=0)ok=false;
    std::vector<std::string> verified;uint64_t verified_sequence=0;
    if(ok)ok=read_slot(file_path,verified,verified_sequence) && verified_sequence==sequence_+1 && verified==paths_;
    if(ok) {active_=target;sequence_=verified_sequence;dirty_=false;}
    return ok;
}
}
