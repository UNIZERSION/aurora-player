#include "player_store.h"
#include "music_library.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;
static void make_wave(const fs::path &path, bool tiny = false) {
    FILE *f = fopen(path.string().c_str(), "wb"); assert(f);
    unsigned char header[] = {
        'R','I','F','F',0x24,0x7d,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
        1,0,1,0,0x80,0x3e,0,0,0,0x7d,0,0,2,0,16,0,'d','a','t','a',0,0x7d,0,0
    };
    if (tiny) { header[4] = 38; header[5] = 0; header[40] = 2; header[41] = 0; }
    assert(fwrite(header, 1, sizeof header, f) == sizeof header);
    unsigned char zero[32000]{}; const size_t bytes = tiny ? 2 : sizeof zero;
    assert(fwrite(zero, 1, bytes, f) == bytes); fclose(f);
}
static std::vector<unsigned char> read_bytes(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const fs::path root = argv[1]; fs::create_directories(root);
    assert(player::crc32("123456789", 9) == 0xcbf43926U);
    assert(player::valid_music_path("/sdcard/Music/album/a.mp3", "/sdcard/Music"));
    assert(!player::valid_music_path("/sdcard/Music/../a.mp3", "/sdcard/Music"));
    assert(!player::valid_music_path("/sdcard/MusicOther/a.mp3", "/sdcard/Music"));
    assert(!player::valid_music_path("/sdcard/Music/./a.mp3", "/sdcard/Music"));
    player::Settings s{};
    player::SettingsStore store((root / "settings").generic_string());
    assert(!store.load(s)); s.volume = 72; assert(store.save(s));
    s.volume = 24; assert(store.save(s));
    player::SettingsStore again((root / "settings").generic_string());
    assert(again.load(s) && s.volume == 24 && again.sequence() == 2);
    // Simulate interrupted write: newest slot truncated; previous value survives.
    std::ofstream(root / "settings/settings-b.bin", std::ios::binary | std::ios::trunc) << "partial";
    assert(again.load(s) && s.volume == 72 && again.sequence() == 1);
    s.volume = 55; assert(again.save(s)); assert(again.sequence() == 2);
    // One-byte corruption must fail CRC, not be accepted as a new setting.
    { std::fstream f(root / "settings/settings-b.bin", std::ios::binary | std::ios::in | std::ios::out);
      f.seekp(24); f.put('\0'); }
    assert(again.load(s) && s.volume == 72);
    s.volume = 101; assert(!again.save(s));
    player::ShuffleBag bag; bag.reset(32, 4, 7); std::set<int> seen{4};
    for (int i = 0; i < 31; ++i) { int n = bag.next(4, false); assert(n >= 0 && n < 32); assert(seen.insert(n).second); }
    assert(bag.next(4, false) == -1); assert(bag.next(4, true) != 4);
    bag.reset(1, 0, 0); assert(bag.next(0, false) == -1); assert(bag.next(0, true) == 0);
    bag.reset(0, -1, 1); assert(bag.next(-1, true) == -1);
    fs::create_directories(root / "Music/Album");
    make_wave(root / "Music/Album/Z.wav"); make_wave(root / "Music/A.WAV");
    std::ofstream(root / "Music/broken.mp3") << "not mp3";
    player::MusicLibrary lib;
    auto scan = lib.scan((root / "Music").generic_string(), (root / "catalog.bin").generic_string());
    assert(scan.readable && scan.unsupported == 1 && lib.tracks.size() == 2 && scan.cached == 0);
    assert(lib.visible.size() == 2);
    lib.filter_sort((root / "Music/Album").generic_string(), 0); assert(lib.visible.size() == 1);
    assert(lib.find_visible((root / "Music/Album/Z.wav").generic_string()) == 0);
    scan = lib.scan((root / "Music").generic_string(), (root / "catalog.bin").generic_string());
    assert(scan.cached == 2 && scan.unsupported == 1);
    fs::remove(root / "Music/A.WAV");
    scan = lib.scan((root / "Music").generic_string(), (root / "catalog.bin").generic_string());
    assert(lib.tracks.size() == 1 && scan.cached == 1);
    // A realistic 5000-track catalog must not silently truncate at the old 2000 limit.
    fs::create_directories(root / "Large");
    for (int i = 0; i < 5001; ++i) {
        char name[32]; snprintf(name, sizeof name, "%05d.WAV", i);
        make_wave(root / "Large" / name, true);
    }
    struct Progress { size_t visited = 0, found = 0, calls = 0; } progress;
    scan = lib.scan((root / "Large").generic_string(), (root / "large-cache.bin").generic_string(),
        [](size_t visited, size_t found, size_t, void *context) {
            auto &p = *static_cast<Progress *>(context);
            assert(visited >= p.visited && found >= p.found);
            p.visited = visited; p.found = found; ++p.calls;
        }, &progress);
    assert(scan.readable && !scan.limited && lib.tracks.size() == 5001 && lib.visible.size() == 5001);
    assert(progress.found == 5001 && progress.calls >= 5001);
    assert(lib.tracks[lib.visible.front()].path.find("00000.WAV") != std::string::npos);
    scan = lib.scan((root / "Large").generic_string(), (root / "large-cache.bin").generic_string());
    assert(scan.cached == 5001 && lib.visible.size() == 5001);

    // Cold lazy discovery performs zero audio probes/file opens, including for
    // invalid contents. It retains only extension, size and filename metadata.
    const auto lazy_cache = root / "lazy-cache.bin";
    scan = lib.scan((root / "Large").generic_string(), lazy_cache.generic_string(), nullptr, nullptr, false);
    assert(scan.readable && scan.probed == 0 && scan.cached == 0 && lib.tracks.size() == 5001);
    for (const auto &track : lib.tracks) {
        assert(!track.metadata_ready && !track.info.sample_rate && !track.info.duration_ms);
        assert(track.info.format == AUDIO_FORMAT_WAV && track.info.file_size == 46 && track.info.title[0]);
    }
    const size_t first = lib.visible[123], second = lib.visible[234];
    audio_file_info_t one{}, two{};
    assert(audio_probe_file(lib.tracks[first].path.c_str(), &one) == 0);
    assert(audio_probe_file(lib.tracks[second].path.c_str(), &two) == 0);
    snprintf(one.title, sizeof one.title, "Decoded title");
    snprintf(one.artist, sizeof one.artist, "Decoded artist");
    snprintf(two.title, sizeof two.title, "Second decoded title");
    auto bad = one; ++bad.file_size;
    assert(!lib.update_metadata(first, bad));
    assert(lib.update_metadata(first, one) && lib.update_metadata(second, two));
    assert(lib.tracks[first].metadata_ready && !strcmp(lib.tracks[first].info.title, one.title));
    const auto before = read_bytes(lazy_cache);
    assert(before.size() > 16);
    uint32_t record_size; memcpy(&record_size, before.data() + 8, sizeof record_size);
    assert(lib.flush_metadata_cache(0) == 0 && lib.flush_metadata_cache(1) == 1);
    const auto after_one = read_bytes(lazy_cache);
    assert(before.size() == after_one.size());
    std::set<size_t> changed_records;
    for (size_t i = 0; i < before.size(); ++i) if (before[i] != after_one[i]) {
        assert(i >= 16); changed_records.insert((i - 16) / record_size);
    }
    assert(changed_records.size() == 1 && (changed_records.count(first) || changed_records.count(second)));
    assert(lib.flush_metadata_cache(1) == 1 && lib.flush_metadata_cache(1) == 0);
    player::MusicLibrary reloaded;
    scan = reloaded.scan((root / "Large").generic_string(), lazy_cache.generic_string(), nullptr, nullptr, false);
    assert(scan.probed == 0 && scan.cached == 5001 && reloaded.tracks.size() == 5001);
    const int visible_first = reloaded.find_visible(lib.tracks[first].path);
    const int visible_second = reloaded.find_visible(lib.tracks[second].path);
    assert(visible_first >= 0 && visible_second >= 0);
    const size_t enriched = reloaded.visible[static_cast<size_t>(visible_first)];
    assert(reloaded.tracks[enriched].metadata_ready && !strcmp(reloaded.tracks[enriched].info.artist, one.artist));
    assert(reloaded.tracks[reloaded.visible[static_cast<size_t>(visible_second)]].metadata_ready);
    size_t ready = 0; for (const auto &track : reloaded.tracks) ready += track.metadata_ready;
    assert(ready == 2);
    reloaded.filter_sort((root / "Large").generic_string(), 2);
    assert(reloaded.tracks[reloaded.visible[0]].metadata_ready && reloaded.tracks[reloaded.visible[1]].metadata_ready);
    assert(!reloaded.tracks[reloaded.visible[2]].metadata_ready); // Known zero-duration tiny WAVs stay before unknown.

    // A changed/truncated on-disk catalog must invalidate retained record offsets.
    snprintf(one.album, sizeof one.album, "Changed after reload");
    assert(reloaded.update_metadata(enriched, one));
    { std::fstream file(lazy_cache, std::ios::binary | std::ios::in | std::ios::out);
      file.seekp(16 + static_cast<std::streamoff>(enriched) * record_size + 4); file.put('!'); }
    const auto corrupt = read_bytes(lazy_cache);
    assert(reloaded.flush_metadata_cache(1) == 0 && read_bytes(lazy_cache) == corrupt);
    scan = reloaded.scan((root / "Large").generic_string(), lazy_cache.generic_string(), nullptr, nullptr, false);
    assert(scan.probed == 0 && scan.cached == 5000);
    const size_t repaired = reloaded.visible[static_cast<size_t>(reloaded.find_visible(lib.tracks[first].path))];
    assert(!reloaded.tracks[repaired].metadata_ready);
    assert(reloaded.update_metadata(repaired, one));
    fs::resize_file(lazy_cache, 12);
    assert(reloaded.flush_metadata_cache(1) == 0 && fs::file_size(lazy_cache) == 12);
    scan = reloaded.scan((root / "missing-directory").generic_string(), lazy_cache.generic_string(), nullptr, nullptr, false);
    assert(!scan.readable && reloaded.flush_metadata_cache() == 0 && fs::file_size(lazy_cache) == 12);

    // Lazy malformed rows become ordinary playback errors; eager mode still
    // validates contents, even when a placeholder came from the cache.
    fs::create_directories(root / "LazySmall");
    make_wave(root / "LazySmall/valid.wav", true);
    std::ofstream(root / "LazySmall/invalid.mp3") << "not an MP3";
    scan = lib.scan((root / "LazySmall").generic_string(), (root / "small-lazy-cache.bin").generic_string(), nullptr, nullptr, false);
    assert(scan.probed == 0 && lib.tracks.size() == 2);
    scan = lib.scan((root / "LazySmall").generic_string(), (root / "small-lazy-cache.bin").generic_string());
    assert(scan.probed == 2 && scan.unsupported == 1 && lib.tracks.size() == 1 && lib.tracks[0].metadata_ready);
    assert(player::crc32("123456789",9)==0xcbf43926U);
    player::MusicLibrary scoped;
    for(const char *p:{"/sdcard/Music/Rock/a.mp3","/sdcard/Music/Rock/Live/b.wav","/sdcard/Music/Rockabilly/c.mp3","/sdcard/Music/Jazz/d.mp3"}){player::Track t;t.path=p;scoped.tracks.push_back(t);}
    auto queue=scoped.folder_queue("/sdcard/Music/Rock");assert(queue.size()==2);
    scoped.filter_sort("/sdcard/Music",0,"Jazz");assert(scoped.visible.size()==1);
    assert(scoped.folder_queue("/sdcard/Music/Rock")==queue);
    player::ShuffleBag folder_bag;folder_bag.reset(queue.size(),-1,123);
    for(int i=0;i<20;i++){int n=folder_bag.next(-1,true);assert(n>=0&&n<2);assert(player::valid_music_path(scoped.tracks[queue[n]].path,"/sdcard/Music/Rock"));}
    assert(scoped.folder_queue("/sdcard/Music/Empty").empty());
    // Warm index loading must work without touching the music directories.
    const auto music = (root / "LazySmall").generic_string();
    const auto index_cache = (root / "index-cache.bin").generic_string();
    scan = lib.scan(music, index_cache, nullptr, nullptr, false, 0x1234);
    assert(scan.readable && lib.tracks.size() == 2);
    fs::rename(root / "LazySmall", root / "MusicTemporarilyUnavailable");
    player::MusicLibrary indexed;
    assert(indexed.load_index(music, index_cache, 0x1234) && indexed.tracks.size() == 2);
    assert(!indexed.load_index(music, index_cache, 0x5678)); // Different SD card.
    assert(!indexed.load_index(music + "Other", index_cache, 0x1234));
    assert(indexed.tracks.size() == 2); // Failed loads never discard valid RAM state.
    fs::rename(root / "MusicTemporarilyUnavailable", root / "LazySmall");
    indexed.filter_sort(music, 0, "VALID.WAV"); assert(indexed.visible.size() == 1);
    auto &tagged = indexed.tracks[indexed.visible[0]];
    snprintf(tagged.info.artist, sizeof tagged.info.artist, "ŻÓŁW TEST");
    indexed.filter_sort(music, 0, "żółw"); assert(indexed.visible.size() == 1);
    indexed.filter_sort(music, 0, "not present"); assert(indexed.visible.empty());
    { std::fstream f(index_cache, std::ios::binary | std::ios::in | std::ios::out); f.seekp(20); f.put('!'); }
    assert(!indexed.load_index(music, index_cache, 0x1234));
    scan = lib.scan(music, index_cache, nullptr, nullptr, false, 0x1234);
    { std::ofstream f(index_cache + ".index", std::ios::binary | std::ios::app); f.put('!'); }
    assert(!indexed.load_index(music, index_cache, 0x1234));
    // Preserve actual v1 on-SD layout while adding Bluetooth settings in v2.
    struct OldSettings { uint8_t volume,brightness,repeat,visual,shuffle,muted,grid,sort;
        uint32_t resume_ms,dim_seconds; char last_path[player::PATH_LIMIT],folder[player::PATH_LIMIT]; };
    struct OldRecord { uint32_t magic,version,payload_size,reserved; uint64_t sequence; OldSettings data; uint32_t checksum; };
    OldRecord old{}; old.magic=0x35504154;old.version=1;old.payload_size=sizeof(OldSettings);old.sequence=55;
    old.data.volume=37;old.data.brightness=70;old.data.dim_seconds=120;old.data.resume_ms=12345;
    snprintf(old.data.last_path,sizeof old.data.last_path,"/sdcard/Music/track.mp3");
    old.checksum=player::crc32(&old,offsetof(OldRecord,checksum));
    fs::create_directories(root/"legacy-settings");
    { std::ofstream f(root/"legacy-settings/settings-a.bin",std::ios::binary);f.write(reinterpret_cast<const char*>(&old),sizeof old); }
    player::SettingsStore migrated((root/"legacy-settings").generic_string());
    assert(migrated.load(s) && s.volume==37 && s.resume_ms==12345 && migrated.sequence()==55);
    assert(!s.bt_enabled && s.local_output && s.covers);
    assert(s.effect==0 && s.library_tab==0 && s.format_filter==0);
    s.bt_enabled=1;s.local_output=0;s.bt_volume=42;s.receiver_count=1;
    snprintf(s.bt_target,sizeof s.bt_target,"AA:BB:CC:DD:EE:FF");
    snprintf(s.receivers[0].mac,sizeof s.receivers[0].mac,"AA:BB:CC:DD:EE:FF");
    snprintf(s.receivers[0].name,sizeof s.receivers[0].name,"Headphones");
    assert(migrated.save(s)); player::SettingsStore migrated_again((root/"legacy-settings").generic_string());
    s={};assert(migrated_again.load(s) && s.bt_enabled && !s.local_output && s.bt_volume==42 && s.receiver_count==1);
    s.receiver_count=9;assert(!migrated_again.save(s));s.receiver_count=1;s.bt_target[0]='X';assert(!migrated_again.save(s));
    // Read the exact released v2 record, including nonzero tail padding. The
    // new effect member occupies old padding and must receive its default.
    struct V2Settings {uint8_t volume,brightness,repeat,visual,shuffle,muted,grid,sort;
        uint32_t resume_ms,dim_seconds;char last_path[player::PATH_LIMIT],folder[player::PATH_LIMIT];
        uint8_t bt_enabled,local_output,bt_volume,receiver_count,covers;
        char bt_target[18];player::SavedReceiver receivers[8];};
    struct V2Record {uint32_t magic,version,payload_size,reserved;uint64_t sequence;V2Settings data;uint32_t checksum;};
    static_assert(sizeof(V2Settings)==1304);
    V2Record v2{};v2.magic=0x35504154;v2.version=2;v2.payload_size=sizeof(V2Settings);v2.sequence=70;
    v2.data.volume=67;v2.data.brightness=80;v2.data.dim_seconds=120;v2.data.resume_ms=54321;
    v2.data.bt_enabled=1;v2.data.local_output=0;v2.data.bt_volume=31;v2.data.receiver_count=1;v2.data.covers=1;
    snprintf(v2.data.last_path,sizeof v2.data.last_path,"/sdcard/Music/kept.mp3");
    snprintf(v2.data.folder,sizeof v2.data.folder,"/sdcard/Music/Kept Album");
    snprintf(v2.data.bt_target,sizeof v2.data.bt_target,"12:34:56:78:9A:BC");
    snprintf(v2.data.receivers[0].mac,sizeof v2.data.receivers[0].mac,"12:34:56:78:9A:BC");
    snprintf(v2.data.receivers[0].name,sizeof v2.data.receivers[0].name,"Saved receiver");
    reinterpret_cast<unsigned char*>(&v2.data)[1303]=0xe7;
    v2.checksum=player::crc32(&v2,offsetof(V2Record,checksum));
    fs::create_directories(root/"v2-settings");
    {std::ofstream f(root/"v2-settings/settings-a.bin",std::ios::binary);f.write(reinterpret_cast<const char*>(&v2),sizeof v2);}
    player::SettingsStore from_v2((root/"v2-settings").generic_string());
    s={};assert(from_v2.load(s) && from_v2.sequence()==70);
    assert(s.volume==67 && s.resume_ms==54321 && s.bt_enabled && !s.local_output && s.bt_volume==31 && s.covers);
    assert(!strcmp(s.last_path,v2.data.last_path) && !strcmp(s.folder,v2.data.folder));
    assert(!strcmp(s.bt_target,v2.data.bt_target) && !strcmp(s.receivers[0].name,"Saved receiver"));
    assert(s.effect==0 && s.library_tab==0 && s.format_filter==0);
    s.effect=3;s.library_tab=2;s.format_filter=4;assert(from_v2.save(s));
    player::SettingsStore v3((root/"v2-settings").generic_string());
    s={};assert(v3.load(s) && v3.sequence()==71 && s.effect==3 && s.library_tab==2 && s.format_filter==4);
    auto invalid=s;invalid.effect=4;assert(!v3.save(invalid));
    invalid=s;invalid.library_tab=3;assert(!v3.save(invalid));
    invalid=s;invalid.format_filter=5;assert(!v3.save(invalid));
    {std::ofstream f(root/"v2-settings/settings-b.bin",std::ios::binary|std::ios::trunc);f<<"interrupted v3 save";}
    assert(v3.load(s) && v3.sequence()==70 && s.volume==67 && s.effect==0 && s.library_tab==0 && s.format_filter==0);
    struct V3Settings {uint8_t volume,brightness,repeat,visual,shuffle,muted,grid,sort;
        uint32_t resume_ms,dim_seconds;char last_path[player::PATH_LIMIT],folder[player::PATH_LIMIT];
        uint8_t bt_enabled,local_output,bt_volume,receiver_count,covers;
        char bt_target[18];player::SavedReceiver receivers[8];uint8_t effect,library_tab,format_filter;};
    struct V3Record {uint32_t magic,version,payload_size,reserved;uint64_t sequence;V3Settings data;uint32_t checksum;};
    static_assert(sizeof(V3Settings)==1308);
    V3Record old3{};old3.magic=0x35504154;old3.version=3;old3.payload_size=sizeof(V3Settings);old3.sequence=99;
    // Include nonzero legacy tail padding; it must never become dragon settings.
    memcpy(&old3.data,&s,1306);old3.data.effect=2;old3.data.library_tab=2;old3.data.format_filter=4;
    reinterpret_cast<uint8_t*>(&old3.data)[1306]=0xfe;reinterpret_cast<uint8_t*>(&old3.data)[1307]=0xfd;
    old3.checksum=player::crc32(&old3,offsetof(V3Record,checksum));fs::create_directories(root/"v3-settings");
    {std::ofstream f(root/"v3-settings/settings-a.bin",std::ios::binary);f.write(reinterpret_cast<const char*>(&old3),sizeof old3);}
    player::SettingsStore from3((root/"v3-settings").generic_string());s={};assert(from3.load(s));
    assert(s.effect==2&&s.library_tab==2&&s.format_filter==4&&s.resume_ms==54321&&s.bt_enabled&&s.bt_volume==31);
    assert(s.dragon_type==1&&s.dragon_speed==10&&s.dragon_colors&&s.animations&&s.clock24);
    s.dragon_type=0;s.dragon_speed=16;s.dragon_colors=0;s.animations=0;s.clock24=0;assert(from3.save(s));
    player::SettingsStore again4((root/"v3-settings").generic_string());s={};assert(again4.load(s));
    assert(s.dragon_type==0&&s.dragon_speed==16&&!s.dragon_colors&&!s.animations&&!s.clock24);
    invalid=s;invalid.dragon_type=2;assert(!again4.save(invalid));invalid=s;invalid.dragon_speed=3;assert(!again4.save(invalid));invalid=s;invalid.dragon_speed=17;assert(!again4.save(invalid));
    {std::ofstream f(root/"v3-settings/settings-b.bin",std::ios::binary|std::ios::trunc);f<<"interrupted v4 write";}
    assert(again4.load(s)&&s.dragon_type==1&&s.resume_ms==54321&&again4.sequence()==99);
    struct V4Record {uint32_t magic,version,payload_size,reserved;uint64_t sequence;player::Settings data;uint32_t checksum;};
    V4Record old4{};old4.magic=0x35504154;old4.version=4;old4.payload_size=sizeof s;old4.sequence=123;old4.data=s;
    old4.data.speed_percent=0xfe; // This byte was padding in the released v4.
    old4.checksum=player::crc32(&old4,offsetof(V4Record,checksum));fs::create_directories(root/"v4-settings");
    {std::ofstream f(root/"v4-settings/settings-a.bin",std::ios::binary);f.write(reinterpret_cast<const char*>(&old4),sizeof old4);}
    player::SettingsStore from4((root/"v4-settings").generic_string());assert(from4.load(s)&&s.speed_percent==100&&s.resume_ms==54321);
    s.speed_percent=75;assert(from4.save(s));s={};assert(from4.load(s)&&s.speed_percent==75);
    s.speed_percent=125;assert(from4.save(s));s={};assert(from4.load(s)&&s.speed_percent==125);
    s.speed_percent=74;assert(!from4.save(s));s.speed_percent=126;assert(!from4.save(s));
    puts("PASS: settings v1/v2/v3/v4 to v5, 75..125% speed persistence/bounds; legacy padding; BT/resume; A/B recovery; shuffle; 5001-track CRC catalog.");
}
