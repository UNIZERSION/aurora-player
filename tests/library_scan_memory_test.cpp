#include "music_library.h"
#include <algorithm>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <new>

// Count real C++ allocations while the currently displayed catalog stays
// alive, as it does during Settings > Update library on the device.
namespace {
struct alignas(std::max_align_t) Allocation { size_t bytes; };
size_t live_bytes=0,peak_bytes=0,largest_allocation=0;
size_t reject_above=0;
int periodic_checks_before_failure=-1;
void *allocate(size_t bytes) {
    auto *p=static_cast<Allocation *>(std::malloc(sizeof(Allocation)+bytes));
    if(!p)throw std::bad_alloc();
    p->bytes=bytes;live_bytes+=bytes;peak_bytes=std::max(peak_bytes,live_bytes);
    largest_allocation=std::max(largest_allocation,bytes);return p+1;
}
void release(void *ptr) noexcept {
    if(!ptr)return;
    auto *p=static_cast<Allocation *>(ptr)-1;live_bytes-=p->bytes;std::free(p);
}
void make_wave(const std::filesystem::path &path) {
    const unsigned char bytes[]={
        'R','I','F','F',38,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
        1,0,1,0,0x80,0x3e,0,0,0,0x7d,0,0,2,0,16,0,'d','a','t','a',2,0,0,0,0,0};
    FILE *f=fopen(path.string().c_str(),"wb");assert(f);
    assert(fwrite(bytes,1,sizeof bytes,f)==sizeof bytes);assert(fclose(f)==0);
}
#ifdef AURORA_SCAN_MEMORY_TEST
uint64_t file_hash(const std::string &path) {
    FILE *f=fopen(path.c_str(),"rb");assert(f);uint64_t hash=14695981039346656037ULL;
    unsigned char bytes[4096];size_t n;
    while((n=fread(bytes,1,sizeof bytes,f)))for(size_t i=0;i<n;++i){hash^=bytes[i];hash*=1099511628211ULL;}
    assert(!ferror(f));assert(fclose(f)==0);return hash;
}
#endif
}
namespace player {
bool scan_memory_test_budget(size_t bytes) {
    if(reject_above && bytes>reject_above)return false;
    if(!bytes && periodic_checks_before_failure>=0 && periodic_checks_before_failure--==0)return false;
    return true;
}
}
void *operator new(size_t n){return allocate(n);}
void *operator new[](size_t n){return allocate(n);}
void operator delete(void *p) noexcept{release(p);}
void operator delete[](void *p) noexcept{release(p);}
void operator delete(void *p,size_t) noexcept{release(p);}
void operator delete[](void *p,size_t) noexcept{release(p);}

int main(int argc,char **argv) {
    assert(argc==2 || argc==3);setvbuf(stdout,nullptr,_IONBF,0);
    namespace fs=std::filesystem;
    const fs::path root=argv[1],music=root/"Music",cache=root/"catalog.bin";
    fs::create_directories(music/"Album");
    constexpr size_t count=4313;
    for(size_t i=0;i<count;++i){char name[32];snprintf(name,sizeof name,"%05u.wav",unsigned(i));make_wave(music/"Album"/name);}
    const auto music_path=music.generic_string(),cache_path=cache.generic_string();
    {
        player::MusicLibrary initial;auto scan=initial.scan(music_path,cache_path,nullptr,nullptr,false,1234);
        assert(scan.readable && initial.tracks.size()==count);
        audio_file_info_t metadata{};assert(audio_probe_file(initial.tracks[0].path.c_str(),&metadata)==0);
        snprintf(metadata.title,sizeof metadata.title,"Preserved decoded title");
        assert(initial.update_metadata(0,metadata));assert(initial.flush_metadata_cache(1)==1);
    }
    player::MusicLibrary displayed;assert(displayed.load_index(music_path,cache_path,1234));
    const auto decoded_path=displayed.tracks[0].path;
    const size_t baseline=live_bytes;peak_bytes=baseline;largest_allocation=0;
    player::MusicLibrary replacement;
    const auto scan=replacement.scan(music_path,cache_path,nullptr,nullptr,false,1234);
    const size_t extra_peak=peak_bytes-baseline;
    printf("tracks=%zu sizeof_track=%zu baseline=%zu additional_peak=%zu largest_allocation=%zu cached=%zu\n",
           count,sizeof(player::Track),baseline,extra_peak,largest_allocation,scan.cached);
    assert(scan.readable && scan.cached==count && scan.probed==0);
    assert(displayed.tracks.size()==count && replacement.tracks.size()==count);
    const auto enriched=std::find_if(replacement.tracks.begin(),replacement.tracks.end(),[&](const auto &t){return t.path==decoded_path;});
    assert(enriched!=replacement.tracks.end() && enriched->metadata_ready && !strcmp(enriched->info.title,"Preserved decoded title"));
    const size_t budget=argc==3?std::strtoull(argv[2],nullptr,10):4500000;
    if(budget)assert(extra_peak<budget);
    player::MusicLibrary reloaded;assert(reloaded.load_index(music_path,cache_path,1234));
    assert(reloaded.tracks.size()==count);
#ifdef AURORA_SCAN_MEMORY_TEST
    const auto cache_hash=file_hash(cache_path),index_hash=file_hash(cache_path+".index");
    reject_above=1000*sizeof(player::Track);
    player::MusicLibrary denied;
    auto failed=denied.scan(music_path,cache_path,nullptr,nullptr,false,1234);
    assert(failed.out_of_memory && denied.tracks.empty());
    assert(file_hash(cache_path)==cache_hash && file_hash(cache_path+".index")==index_hash);
    reject_above=0;periodic_checks_before_failure=8;
    player::MusicLibrary interrupted;
    failed=interrupted.scan(music_path,cache_path,nullptr,nullptr,false,1234);
    assert(failed.out_of_memory && !interrupted.tracks.empty() && interrupted.tracks.size()<count);
    assert(file_hash(cache_path)==cache_hash && file_hash(cache_path+".index")==index_hash);
    periodic_checks_before_failure=-1;
    const size_t cold_baseline=live_bytes;peak_bytes=cold_baseline;largest_allocation=0;
    player::MusicLibrary cold;
    const auto cold_scan=cold.scan(music_path,(root/"cold.bin").generic_string(),nullptr,nullptr,false,1234);
    printf("cold_scan additional_peak=%zu largest_allocation=%zu tracks=%zu\n",peak_bytes-cold_baseline,largest_allocation,cold.tracks.size());
    assert(cold_scan.readable && !cold_scan.out_of_memory && cold.tracks.size()==count);
    assert(peak_bytes-cold_baseline<5500000);
#endif
    puts("PASS: refresh keeps displayed catalog and decoded metadata, writes a valid index, and stays within bounded replacement memory.");
}
