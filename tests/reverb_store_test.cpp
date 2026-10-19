#undef NDEBUG
#include "reverb_store.h"
#include "player_store.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
namespace fs=std::filesystem;
static bool same(const audio_reverb_config_t &a,const audio_reverb_config_t &b){return a.amount==b.amount&&a.decay_ms==b.decay_ms&&a.tone_hz==b.tone_hz&&a.low_cut_hz==b.low_cut_hz;}
static std::vector<char> bytes(const fs::path &p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void write(const fs::path &p,const std::vector<char> &b){std::ofstream f(p,std::ios::binary|std::ios::trunc);f.write(b.data(),b.size());assert(f.good());}
int main(int argc,char **argv){
 assert(argc==2);const fs::path root=argv[1];assert(!fs::exists(root));fs::create_directories(root);
 const auto a=audio_reverb_default_config();auto b=a;b.amount=55;b.decay_ms=3800;b.tone_hz=20000;b.low_cut_hz=1200;auto out=b;
 player::ReverbStore store(root.string());assert(!store.load(out)&&same(out,b));assert(store.save(a));assert(store.sequence()==1);
 assert(store.save(b));assert(store.sequence()==2);player::ReverbStore fresh(root.string());assert(fresh.load(out)&&same(out,b)&&fresh.sequence()==2);
 auto invalid=b;invalid.amount=61;assert(!fresh.save(invalid)&&fresh.sequence()==2);
 const auto original=bytes(root/"reverb-b.bin");assert(original.size()==32);
 for(unsigned i=0;i<original.size();++i){auto broken=original;broken[i]^=0x55;write(root/"reverb-b.bin",broken);player::ReverbStore test(root.string());assert(test.load(out)&&same(out,a)&&test.sequence()==1);}
 for(unsigned size=0;size<original.size();++size){auto truncated=original;truncated.resize(size);write(root/"reverb-b.bin",truncated);player::ReverbStore test(root.string());assert(test.load(out)&&same(out,a));}
 auto extra=original;extra.push_back('x');write(root/"reverb-b.bin",extra);assert(fresh.load(out)&&same(out,a));
 assert(fresh.save(b)&&fresh.sequence()==2);assert(fresh.load(out)&&same(out,b));
 // No partial serialized struct/padding and no ABI dependency on Settings.
 const auto restored=bytes(root/"reverb-b.bin");assert(restored==original);
 // Exact released v1 bytes: preserve Amount/Decay/Tone and add bypassed low cut.
 auto legacy=bytes(root/"reverb-a.bin");legacy[4]=1;
 auto checksum=player::crc32(reinterpret_cast<const uint8_t*>(legacy.data()),28);
 for(unsigned i=0;i<4;++i)legacy[28+i]=char(checksum>>(8*i));
 const auto migration=root/"migration";fs::create_directory(migration);write(migration/"reverb-a.bin",legacy);
 player::ReverbStore migrated(migration.string());assert(migrated.load(out)&&same(out,a)&&out.low_cut_hz==0);
 assert(migrated.save(b)&&migrated.sequence()==2);assert(bytes(migration/"reverb-b.bin")[4]==2);
 player::ReverbStore restart(migration.string());assert(restart.load(out)&&same(out,b));
 // A CRC-valid but impossible band must not replace the last valid slot.
 auto crossed=original;crossed[21]=char(20000&255);crossed[22]=char(20000>>8);
 checksum=player::crc32(reinterpret_cast<const uint8_t*>(crossed.data()),28);
 for(unsigned i=0;i<4;++i)crossed[28+i]=char(checksum>>(8*i));
 write(migration/"reverb-b.bin",crossed);assert(restart.load(out)&&same(out,a));
 const auto blocked=root/"unwritable";fs::create_directory(blocked);fs::create_directory(blocked/"reverb-a.bin");
 player::ReverbStore failure(blocked.string());assert(!failure.save(a));assert(!fs::exists(blocked/"reverb-b.bin"));
 puts("PASS reverb A/B persistence: v1 Tone migration, v2 both cutoffs, invalid crossed bands, clean restart, all32 byte corruptions, every truncated record, trailing bytes, fallback/recovery and I/O refusal preserve last good state.");return 0;
}
