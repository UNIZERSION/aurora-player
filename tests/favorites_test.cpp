#ifdef NDEBUG
#undef NDEBUG
#endif
#include "favorites_store.h"
#include "player_store.h"
#include "music_library.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
namespace fs=std::filesystem;
static std::vector<uint8_t> bytes(const fs::path &path) {
    std::ifstream file(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
static void write_bytes(const fs::path &path,const std::vector<uint8_t> &data) {
    std::ofstream file(path,std::ios::binary|std::ios::trunc);assert(file);
    file.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));assert(file);
}
static void put32(std::vector<uint8_t> &data,size_t offset,uint32_t value) {
    for(unsigned i=0;i<4;++i){data[offset+i]=static_cast<uint8_t>(value);value>>=8;}
}
static void checksum(std::vector<uint8_t> &data) {put32(data,data.size()-4,player::crc32(data.data(),data.size()-4));}
int main(int argc,char **argv) {
    assert(argc==2);const fs::path root=argv[1];fs::create_directories(root);
    const std::string music="/sdcard/Music",one=music+"/Alpha.mp3",two=music+"/Album/Beta.flac";
    const auto dir=root/"favorites";
    player::FavoritesStore f(dir.generic_string(),music);
    assert(!f.load() && f.size()==0 && !f.dirty());
    assert(f.set(one,true) && f.contains(one) && f.dirty());
    assert(!fs::exists(dir)); // UI toggles must perform no SD writes.
    assert(!f.set(music,true) && !f.set(music+"/",true));
    assert(!f.set(music+"Other/track.mp3",true) && !f.set(music+"/../track.mp3",true));
    assert(!f.set(music+"//track.mp3",true) && !f.set(music+"/a\\b.mp3",true));
    assert(!f.set(music+std::string("/a\0b.mp3",8),true));
    assert(!f.set(music+"/"+std::string(250,'a'),true));
    assert(f.save() && !f.dirty() && f.sequence()==1);
    auto valid_a=bytes(dir/"favorites-a.bin");
    assert(f.set(one,true) && !f.dirty() && f.save() && f.sequence()==1);
    assert(f.toggle(two) && f.contains(two) && f.sequence()==1);
    assert(f.save() && f.sequence()==2);
    player::FavoritesStore loaded(dir.generic_string(),music);
    assert(loaded.load() && loaded.size()==2 && loaded.sequence()==2);
    fs::resize_file(dir/"favorites-b.bin",11);
    assert(loaded.load() && loaded.size()==1 && loaded.contains(one) && loaded.sequence()==1);
    assert(loaded.set(two,true) && loaded.save() && loaded.sequence()==2);
    auto bad=bytes(dir/"favorites-b.bin");bad[26]^=1;write_bytes(dir/"favorites-b.bin",bad);
    assert(loaded.load() && loaded.size()==1 && loaded.sequence()==1);
    assert(loaded.toggle(two) && loaded.dirty());
    write_bytes(dir/"favorites-a.bin",{});
    assert(!loaded.load() && loaded.contains(two) && loaded.dirty()); // Failed reload never loses pending RAM edits.
    write_bytes(dir/"favorites-a.bin",valid_a);fs::remove(dir/"favorites-b.bin");

    // Bounds/path/version are checked even when a malicious record has a valid CRC.
    const auto forge=root/"forged";fs::create_directories(forge);
    player::FavoritesStore forged(forge.generic_string(),music);
    bad=valid_a;bad[26]='X';checksum(bad);write_bytes(forge/"favorites-a.bin",bad);assert(!forged.load());
    bad=valid_a;put32(bad,16,10001);checksum(bad);write_bytes(forge/"favorites-a.bin",bad);assert(!forged.load());
    bad=valid_a;put32(bad,4,99);checksum(bad);write_bytes(forge/"favorites-a.bin",bad);assert(!forged.load());
    bad=valid_a;bad.push_back(0);write_bytes(forge/"favorites-a.bin",bad);assert(!forged.load());
    bad=valid_a;const std::vector<uint8_t> entry(valid_a.begin()+24,valid_a.end()-4);
    bad.insert(bad.end()-4,entry.begin(),entry.end());put32(bad,16,2);put32(bad,20,static_cast<uint32_t>(entry.size()*2));checksum(bad);
    write_bytes(forge/"favorites-a.bin",bad);assert(!forged.load()); // Duplicate paths.

    player::FavoritesStore blocked((root/"absent-parent/nested").generic_string(),music);
    assert(blocked.set(one,true) && !blocked.save() && blocked.dirty());
    fs::create_directories(root/"absent-parent");assert(blocked.save() && !blocked.dirty());
    assert(blocked.toggle(one) && blocked.save());
    player::FavoritesStore empty((root/"absent-parent/nested").generic_string(),music);
    assert(empty.load() && empty.size()==0); // Clearing the last favourite persists.

    player::FavoritesStore large((root/"large").generic_string(),music);
    for(unsigned i=0;i<10000;++i){char name[40];snprintf(name,sizeof name,"/%05u.mp3",i);assert(large.set(music+name,true));}
    assert(large.size()==10000 && !large.set(music+"/overflow.aac",true));
    assert(large.save());player::FavoritesStore large_loaded((root/"large").generic_string(),music);
    assert(large_loaded.load() && large_loaded.paths()==large.paths());
    assert(large_loaded.set(music+"/00000.mp3",false) && large_loaded.set(music+"/Żółw.aac",true));
    assert(large_loaded.save());

    // Local/favourites/search/format are intersected without opening tracks.
    const auto actual=root/"Music";fs::create_directories(actual/"Album");
    for(const char *path:{"a.mp3","b.WAV","Album/c.FLAC","Album/d.AAC","unsupported.m4a"})
        std::ofstream(actual/path)<<"lazy placeholder";
    const auto cache=(root/"catalog.bin").generic_string();
    player::MusicLibrary lib;auto result=lib.scan(actual.generic_string(),cache,nullptr,nullptr,false,123);
    assert(result.probed==0 && lib.tracks.size()==4);
    player::FavoritesStore selections((root/"selection").generic_string(),actual.generic_string());
    assert(selections.set((actual/"b.WAV").generic_string(),true));
    assert(selections.set((actual/"Album/c.FLAC").generic_string(),true));
    lib.filter_sort(actual.generic_string(),0,{},0,&selections,false);assert(lib.visible.size()==4);
    for(unsigned format=1;format<=4;++format){lib.filter_sort(actual.generic_string(),0,{},format);assert(lib.visible.size()==1);assert(unsigned(lib.tracks[lib.visible[0]].info.format)==format);}
    lib.filter_sort((actual/"Album").generic_string(),0);assert(lib.visible.size()==2);
    lib.filter_sort(actual.generic_string(),0,{},0,&selections,true);assert(lib.visible.size()==2);
    lib.filter_sort((actual/"Album").generic_string(),0,{},0,&selections,true);assert(lib.visible.size()==1);
    lib.filter_sort(actual.generic_string(),0,"C.FLAC",3,&selections,true);assert(lib.visible.size()==1);
    lib.filter_sort(actual.generic_string(),0,"C.FLAC",2,&selections,true);assert(lib.visible.empty());
    lib.filter_sort(actual.generic_string(),0,{},0,nullptr,true);assert(lib.visible.empty());
    lib.filter_sort(actual.generic_string(),0,{},5);assert(lib.visible.empty());

    // Discovery checkpoint changes once, but cache record version stays v1.
    auto index=bytes(cache+".index");put32(index,4,1);
    // IndexHeader has trailing ABI padding; checksum is the uint32 at offset256.
    assert(index.size()>=260);put32(index,256,player::crc32(index.data(),256));write_bytes(cache+".index",index);
    player::MusicLibrary warm;assert(!warm.load_index(actual.generic_string(),cache,123));
    result=warm.scan(actual.generic_string(),cache,nullptr,nullptr,false,123);
    assert(result.cached==4 && result.probed==0);
    assert(warm.load_index(actual.generic_string(),cache,123));
    const auto catalog=bytes(cache);assert(catalog[4]==1 && catalog[5]==0);
    puts("PASS: favourites RAM toggles, 10k bound, durable A/B CRC recovery, malformed records, v1 catalog reuse, four formats and folder/favourite/search intersections");
}
