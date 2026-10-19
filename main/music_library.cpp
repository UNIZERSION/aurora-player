#include "music_library.h"
#include "favorites_store.h"
#include <algorithm>
#include <cstring>
#include <string_view>
#include <cstdio>
#include <cctype>
#include <dirent.h>
#include <sys/stat.h>
#include <climits>
#include <set>
#include <unordered_set>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace player {
// IDF builds disable C++ exceptions: avoid a known-over-budget reserve rather
// than letting operator new abort the player. Keep space for live UI tasks,
// paths and directory handles while the old catalog is still being displayed.
static bool scan_memory_available(size_t allocation=0) {
#ifdef AURORA_SCAN_MEMORY_TEST
    extern bool scan_memory_test_budget(size_t);
    return scan_memory_test_budget(allocation);
#elif defined(ESP_PLATFORM)
    constexpr size_t margin=512*1024;
    return allocation<=SIZE_MAX-margin &&
        heap_caps_get_free_size(MALLOC_CAP_8BIT)>=allocation+margin &&
        (!allocation || heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)>=allocation+16384);
#else
    (void)allocation;return true;
#endif
}
// Discovery now includes FLAC and ADTS AAC. Invalidate the old checkpoint once
// to find them, while retaining all unchanged v1 metadata cache records.
static constexpr uint32_t INDEX_VERSION = 2;
struct CachedTrack {
    char path[PATH_LIMIT];
    int64_t modified;
    audio_file_info_t info;
    uint32_t crc;
};
struct CacheHeader { uint32_t magic, version, record_size, count; };
struct IndexHeader {
    uint32_t magic, version;
    uint64_t volume_id;
    char root[PATH_LIMIT];
    uint32_t crc;
};
static bool valid_cached_record(const CachedTrack &record) {
    return record.crc == crc32(&record, offsetof(CachedTrack, crc)) &&
        memchr(record.path, 0, sizeof record.path) &&
        memchr(record.info.title, 0, sizeof record.info.title) &&
        memchr(record.info.artist, 0, sizeof record.info.artist) &&
        memchr(record.info.album, 0, sizeof record.info.album);
}
static CachedTrack cached_record(const Track &track) {
    CachedTrack record{};
    snprintf(record.path, sizeof record.path, "%s", track.path.c_str());
    record.modified = track.modified; record.info = track.info;
    record.crc = crc32(&record, offsetof(CachedTrack, crc));
    return record;
}
// Keep only a compact lookup table while discovering the replacement library.
// The displayed catalog remains alive during a refresh. Keeping a second
// complete CachedTrack + path in every tree node used over 3 MiB for 4313
// songs, before vector growth; that could abort an exceptions-disabled build.
// One open file avoids FatFs's repeated directory walks for per-track fopen.
class CatalogCache {
    struct Entry { uint64_t hash; uint32_t record; };
    FILE *file_=nullptr;
    uint32_t next_record_=UINT32_MAX;
    std::vector<Entry> entries_;
    std::vector<char> buffer_;
    static uint64_t path_hash(const char *path) {
        uint64_t value=UINT64_C(14695981039346656037);
        while(*path){value^=static_cast<unsigned char>(*path++);value*=UINT64_C(1099511628211);}
        return value;
    }
public:
    ~CatalogCache(){close();}
    void close(){if(file_){fclose(file_);file_=nullptr;}next_record_=UINT32_MAX;std::vector<Entry>().swap(entries_);std::vector<char>().swap(buffer_);}
    size_t size() const{return entries_.size();}
    void load(const std::string &path,const std::string &root) {
        file_=fopen(path.c_str(),"rb");if(!file_)return;
        if(!scan_memory_available(8192)){close();return;}
        buffer_.resize(8192);setvbuf(file_,buffer_.data(),_IOFBF,buffer_.size());
        CacheHeader header{};
        if(fread(&header,sizeof header,1,file_)!=1 || header.magic!=0x35434154 || header.version!=1 ||
           header.record_size!=sizeof(CachedTrack) || header.count>MusicLibrary::MAX_TRACKS){close();return;}
        if(!scan_memory_available(size_t(header.count)*sizeof(Entry))){close();return;}
        entries_.reserve(header.count);
        for(uint32_t i=0;i<header.count;++i){
            CachedTrack record{};
            if(fread(&record,sizeof record,1,file_)!=1)break;
            if(valid_cached_record(record) && valid_music_path(record.path,root))entries_.push_back({path_hash(record.path),i});
#ifdef ESP_PLATFORM
            if((i%128)==0)vTaskDelay(1);
#endif
        }
        // Equal hashes stay in original file order. We always compare the full
        // path after rereading and checking the CRC, so collisions cannot alias.
        std::sort(entries_.begin(),entries_.end(),[](const auto &a,const auto &b){return a.hash<b.hash || (a.hash==b.hash && a.record<b.record);});
    }
    bool find(const std::string &path,CachedTrack &out) {
        if(!file_)return false;
        const auto hash=path_hash(path.c_str());
        auto entry=std::lower_bound(entries_.begin(),entries_.end(),hash,[](const Entry &e,uint64_t h){return e.hash<h;});
        for(;entry!=entries_.end() && entry->hash==hash;++entry){
            const uint64_t offset=sizeof(CacheHeader)+uint64_t(entry->record)*sizeof(CachedTrack);
            CachedTrack record{};
            // Usually directory enumeration retains its previous order. Keep
            // those matches sequential so stdio can reuse its read-ahead block.
            if(offset>LONG_MAX || (next_record_!=entry->record && fseek(file_,static_cast<long>(offset),SEEK_SET)!=0)){
                next_record_=UINT32_MAX;continue;
            }
            if(fread(&record,sizeof record,1,file_)!=1){next_record_=UINT32_MAX;continue;}
            next_record_=entry->record+1;
            if(valid_cached_record(record) && path==record.path){out=record;return true;}
        }
        return false;
    }
};
static bool sync_cache(FILE *file) {
    if (fflush(file) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}
static std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // Polish UTF-8 capital letters; ASCII folding above deliberately leaves
    // multibyte characters intact. No locale-dependent filesystem behaviour.
    const char *upper[]{"Ą", "Ć", "Ę", "Ł", "Ń", "Ó", "Ś", "Ź", "Ż"};
    const char *down[]{"ą", "ć", "ę", "ł", "ń", "ó", "ś", "ź", "ż"};
    for (size_t i = 0; i < 9; ++i) {
        size_t at = 0;
        while ((at = s.find(upper[i], at)) != std::string::npos) { s.replace(at, 2, down[i]); at += 2; }
    }
    return s;
}
// Search without constructing four heap strings and scanning each nine times
// per track. Paths and metadata are bounded by PATH_LIMIT in the index/parser.
static bool contains_folded(const char *input, const std::string &needle) {
    char folded[PATH_LIMIT];size_t n=0;
    while(input[n]&&n+1<sizeof folded){unsigned c=static_cast<unsigned char>(input[n]);folded[n]=char(c>='A'&&c<='Z'?c+32:c);n++;}
    folded[n]=0;
    for(size_t i=0;i+1<n;i++){
        unsigned a=static_cast<unsigned char>(folded[i]),b=static_cast<unsigned char>(folded[i+1]);
        if(a==0xc3&&b==0x93)folded[i+1]=char(0xb3);
        else if((a==0xc4&&(b==0x84||b==0x86||b==0x98))||(a==0xc5&&(b==0x81||b==0x83||b==0x9a||b==0xb9||b==0xbb)))folded[i+1]=char(b+1);
    }
    return std::string_view(folded,n).find(needle)!=std::string_view::npos;
}
static audio_format_t name_format(const std::string &s) {
    auto dot = s.find_last_of('.');
    if (dot == std::string::npos) return AUDIO_FORMAT_UNKNOWN;
    auto ext = lower(s.substr(dot));
    if(ext==".mp3")return AUDIO_FORMAT_MP3;
    if(ext==".wav")return AUDIO_FORMAT_WAV;
    if(ext==".flac")return AUDIO_FORMAT_FLAC;
    if(ext==".aac")return AUDIO_FORMAT_AAC;
    return AUDIO_FORMAT_UNKNOWN; // M4A is a different, unsupported container.
}
static bool supported_name(const std::string &s) { return name_format(s)!=AUDIO_FORMAT_UNKNOWN; }
static bool supported_format(audio_format_t format) {
    return format==AUDIO_FORMAT_MP3 || format==AUDIO_FORMAT_WAV ||
        format==AUDIO_FORMAT_FLAC || format==AUDIO_FORMAT_AAC;
}
static int compare_folded(const char *left, const char *right) {
    // Compare ASCII case-insensitively without allocating strings per comparison.
    for (;;) {
        unsigned a = static_cast<unsigned char>(*left++), b = static_cast<unsigned char>(*right++);
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return a < b ? -1 : 1;
        if (!a) return 0;
    }
}
static bool write_index(const std::string &root, const std::string &cache_file, uint64_t volume_id) {
    IndexHeader header{};
    header.magic = 0x58494154; header.version = INDEX_VERSION; header.volume_id = volume_id;
    snprintf(header.root, sizeof header.root, "%s", root.c_str());
    header.crc = crc32(&header, offsetof(IndexHeader, crc));
    const std::string path = cache_file + ".index", temporary = path + ".tmp";
    FILE *file = fopen(temporary.c_str(), "wb");
    if (!file) return false;
    bool ok = fwrite(&header, sizeof header, 1, file) == 1 && sync_cache(file);
    if (fclose(file) != 0) ok = false;
    if (ok) {
        // This checkpoint is disposable. An interruption here falls back to a
        // scan; settings and music are never touched.
        remove(path.c_str());
        ok = rename(temporary.c_str(), path.c_str()) == 0;
    }
    if (!ok) remove(temporary.c_str());
    return ok;
}
bool MusicLibrary::load_index(const std::string &root, const std::string &cache_file, uint64_t volume_id) {
    IndexHeader index{};
    FILE *checkpoint = fopen((cache_file + ".index").c_str(), "rb");
    if (!checkpoint) return false;
    bool ok = fread(&index, sizeof index, 1, checkpoint) == 1 && fgetc(checkpoint) == EOF && !ferror(checkpoint);
    fclose(checkpoint);
    if (!ok || index.magic != 0x58494154 || index.version != INDEX_VERSION || index.volume_id != volume_id ||
        !memchr(index.root, 0, sizeof index.root) || root != index.root ||
        index.crc != crc32(&index, offsetof(IndexHeader, crc))) return false;
    FILE *file = fopen(cache_file.c_str(), "rb");
    if (!file) return false;
    // setvbuf must precede the first read (otherwise some C libraries lose the
    // remaining bytes of the original read-ahead buffer).
    std::vector<char> io_buffer(32768);setvbuf(file,io_buffer.data(),_IOFBF,io_buffer.size());
    CacheHeader header{};
    ok = fread(&header, sizeof header, 1, file) == 1 && header.magic == 0x35434154 &&
        header.version == 1 && header.record_size == sizeof(CachedTrack) && header.count <= MAX_TRACKS;
    MusicLibrary loaded;
    std::unordered_set<std::string> names;names.reserve(ok?header.count:0);
    std::set<std::string> parents{root};
#ifdef ESP_PLATFORM
    TickType_t yielded=xTaskGetTickCount();
#endif
    if (ok) { loaded.tracks.reserve(header.count); loaded.cache_record_crc_.reserve(header.count); }
    for (uint32_t i = 0; ok && i < header.count; ++i) {
        CachedTrack record{};
        ok = fread(&record, sizeof record, 1, file) == 1 && valid_cached_record(record) &&
            valid_music_path(record.path, root) && supported_name(record.path) &&
            record.info.file_size > 0 && record.info.file_size <= 0x7fffffffULL &&
            supported_format(record.info.format) &&
            names.emplace(record.path).second;
        if (!ok) break;
        Track track{}; track.path = record.path; track.modified = record.modified;
        track.info = record.info; track.metadata_ready = record.info.sample_rate != 0;
        loaded.tracks.push_back(std::move(track)); loaded.cache_record_crc_.push_back(record.crc);
        std::string parent = record.path;
        for (;;) {
            const auto slash = parent.find_last_of('/');
            if (slash == std::string::npos || slash < root.size()) break;
            parent.resize(slash); parents.insert(parent);
            if (parents.size() > 4096) { ok = false; break; }
            if (parent == root) break;
        }
#ifdef ESP_PLATFORM
        if ((i % 128) == 0 && xTaskGetTickCount()-yielded>=pdMS_TO_TICKS(20)) {vTaskDelay(1);yielded=xTaskGetTickCount();}
#endif
    }
    if (ok) ok = fgetc(file) == EOF && !ferror(file);
    fclose(file);
    if (!ok) return false;
    loaded.folders.assign(parents.begin(), parents.end());
    loaded.cache_file_ = cache_file; loaded.cache_writable_ = true;
    loaded.metadata_dirty_.assign(loaded.tracks.size(), 0);
    loaded.filter_sort(root, 0);
    *this = std::move(loaded);
    return true;
}
ScanResult MusicLibrary::scan(const std::string &root, const std::string &cache_file,
                              ScanProgress progress, void *context, bool probe_metadata, uint64_t volume_id) {
    cache_writable_ = false; cache_file_.clear();
    metadata_dirty_.clear(); cache_record_crc_.clear();
    tracks.clear(); visible.clear(); folders.clear();
    ScanResult result;
    if (progress) progress(0, 0, 0, context);
    CatalogCache cache;cache.load(cache_file,root);
    // An unchanged library needs one exact allocation, not the transient
    // 4096 + 8192 track buffers of geometric vector growth. A fresh/expanded
    // library grows in bounded blocks below instead of doubling its capacity.
    if(cache.size()>tracks.capacity()){
        if(!scan_memory_available(cache.size()*sizeof(Track))){result.out_of_memory=true;return result;}
        tracks.reserve(cache.size());
    }
    struct Pending { std::string path; unsigned depth; };
    std::vector<Pending> pending{{root, 0}};
    size_t visited = 0;
    while (!pending.empty() && !result.limited) {
        auto item = std::move(pending.back()); pending.pop_back();
        DIR *dir = opendir(item.path.c_str());
        if (!dir) { ++result.skipped; continue; }
        if (item.path == root) result.readable = true;
        folders.push_back(item.path);
        while (auto *entry = readdir(dir)) {
            if (entry->d_name[0] == '.') continue;
            if((visited%32)==0 && !scan_memory_available()){result.out_of_memory=true;break;}
            if (++visited > 100000) { result.limited = true; break; }
            if (progress) progress(visited, tracks.size(), result.cached, context);
#ifdef ESP_PLATFORM
            // Also yield for unsupported files and directories, not just valid tracks.
            if ((visited % 32) == 0) vTaskDelay(1);
#endif
            std::string path = item.path + "/" + entry->d_name;
            if (!valid_music_path(path, root)) { ++result.skipped; continue; }
            struct stat st{};
            if (stat(path.c_str(), &st) != 0) { ++result.skipped; continue; }
            if (S_ISDIR(st.st_mode)) {
                if (item.depth < 16 && pending.size() + folders.size() < 4096) pending.push_back({path, item.depth + 1});
                else ++result.skipped;
                continue;
            }
            if (!S_ISREG(st.st_mode) || !supported_name(path)) continue;
            if (st.st_size <= 0 || uint64_t(st.st_size) > 0x7fffffffULL) { ++result.unsupported; continue; }
            Track track{}; track.path = path; track.modified = st.st_mtime;
            CachedTrack previous{};
            if (cache.find(path,previous) && previous.modified == track.modified &&
                previous.info.file_size == uint64_t(st.st_size) &&
                (!probe_metadata || previous.info.sample_rate != 0)) {
                track.info = previous.info; ++result.cached;
            } else if (probe_metadata) {
                ++result.probed;
                if (audio_probe_file(path.c_str(), &track.info) != 0) { ++result.unsupported; continue; }
            } else {
                // fopen itself walks the directory from its beginning on FatFs.
                // Opening all 5000 files costs O(n^2), even for a tiny header.
                // Keep discovery linear; the decoder enriches a selected track.
                track.info.format = name_format(path);
                track.info.file_size = uint64_t(st.st_size);
            }
            track.metadata_ready = track.info.sample_rate != 0;
            if (!track.info.title[0]) {
                auto title = std::string(entry->d_name);
                title.resize(title.find_last_of('.'));
                snprintf(track.info.title, sizeof track.info.title, "%s", title.c_str());
            }
            if(tracks.size()==tracks.capacity()){
                const size_t capacity=std::min(MAX_TRACKS,tracks.capacity()+512);
                if(!scan_memory_available(capacity*sizeof(Track))){result.out_of_memory=true;break;}
                tracks.reserve(capacity);
            }
            tracks.push_back(std::move(track));
            if (tracks.size() >= MAX_TRACKS) { result.limited = true; break; }
        }
        closedir(dir);
        if(result.out_of_memory)break;
    }
    std::sort(folders.begin(), folders.end());
    // Release the old descriptor before truncating/replacing its cache file.
    cache.close();
    if(result.out_of_memory){if(progress)progress(visited,tracks.size(),result.cached,context);return result;}
    // Cache is disposable: truncation merely triggers a reprobe, never affects music/settings.
    if (result.readable) if (FILE *f = fopen(cache_file.c_str(), "wb")) {
        CacheHeader h{0x35434154, 1, sizeof(CachedTrack), static_cast<uint32_t>(tracks.size())};
        bool ok = fwrite(&h, sizeof h, 1, f) == 1;
        cache_record_crc_.reserve(tracks.size());
        for (const auto &track : tracks) {
            if (!ok) break;
            const CachedTrack c = cached_record(track);
            ok = fwrite(&c, sizeof c, 1, f) == 1;
            cache_record_crc_.push_back(c.crc);
        }
        if (ok) ok = sync_cache(f);
        if (fclose(f) != 0) ok = false;
        if (ok && cache_record_crc_.size() == tracks.size()) {
            cache_file_ = cache_file;
            metadata_dirty_.assign(tracks.size(), 0);
            cache_writable_ = true;
            write_index(root, cache_file, volume_id);
        } else cache_record_crc_.clear();
    }
    filter_sort(root, 0);
    if (progress) progress(visited, tracks.size(), result.cached, context);
    return result;
}
bool MusicLibrary::update_metadata(size_t index, const audio_file_info_t &info) {
    if (index >= tracks.size() || !info.sample_rate ||
        !supported_format(info.format) ||
        info.file_size != tracks[index].info.file_size ||
        !memchr(info.title, 0, sizeof info.title) ||
        !memchr(info.artist, 0, sizeof info.artist) ||
        !memchr(info.album, 0, sizeof info.album)) return false;
    auto &track = tracks[index];
    audio_file_info_t metadata = info;
    if (!metadata.title[0]) snprintf(metadata.title, sizeof metadata.title, "%s", track.info.title);
    const bool changed = !track.metadata_ready || memcmp(&track.info, &metadata, sizeof metadata) != 0;
    track.info = metadata; track.metadata_ready = true;
    if (changed && index < metadata_dirty_.size()) metadata_dirty_[index] = 1;
    return true;
}
size_t MusicLibrary::flush_metadata_cache(size_t max_records) {
    if (!max_records || !cache_writable_ || metadata_dirty_.size() != tracks.size() ||
        cache_record_crc_.size() != tracks.size()) return 0;
    if (std::find(metadata_dirty_.begin(), metadata_dirty_.end(), uint8_t{1}) == metadata_dirty_.end()) return 0;
    FILE *file = fopen(cache_file_.c_str(), "rb+");
    if (!file) return 0;
    CacheHeader header{};
    if (fread(&header, sizeof header, 1, file) != 1 || header.magic != 0x35434154 ||
        header.version != 1 || header.record_size != sizeof(CachedTrack) || header.count != tracks.size()) {
        cache_writable_ = false; fclose(file); return 0;
    }
    std::vector<size_t> written;
    for (size_t index = 0; index < tracks.size() && written.size() < max_records; ++index) {
        if (!metadata_dirty_[index]) continue;
        const uint64_t offset = sizeof(CacheHeader) + uint64_t(index) * sizeof(CachedTrack);
        CachedTrack old{};
        if (offset > LONG_MAX || fseek(file, static_cast<long>(offset), SEEK_SET) != 0 ||
            fread(&old, sizeof old, 1, file) != 1 || !valid_cached_record(old) ||
            old.crc != cache_record_crc_[index] || old.path != tracks[index].path ||
            old.modified != tracks[index].modified || old.info.file_size != tracks[index].info.file_size) {
            // A truncated, replaced or corrupt catalog invalidates our offsets.
            // Keep dirty RAM metadata and rebuild on the next ordinary scan.
            cache_writable_ = false; break;
        }
        const CachedTrack record = cached_record(tracks[index]);
        if (fseek(file, static_cast<long>(offset), SEEK_SET) != 0 ||
            fwrite(&record, sizeof record, 1, file) != 1 || !sync_cache(file)) {
            cache_writable_ = false; break;
        }
        cache_record_crc_[index] = record.crc;
        metadata_dirty_[index] = 0;
        written.push_back(index);
    }
    if (fclose(file) != 0) {
        for (size_t index : written) metadata_dirty_[index] = 1;
        return 0;
    }
    return written.size();
}
void MusicLibrary::filter_sort(const std::string &folder, uint8_t sort, const std::string &query,
                               uint8_t format_filter, const FavoritesStore *favorites, bool favorites_only) {
    visible.clear();
    if(format_filter>4 || (favorites_only && !favorites))return;
    const std::string needle = lower(query);
    for (size_t i = 0; i < tracks.size(); ++i) {
        const auto &track = tracks[i];
        if (!folder.empty() && !valid_music_path(track.path, folder)) continue;
        if (format_filter && static_cast<unsigned>(track.info.format)!=format_filter) continue;
        if (favorites_only && !favorites->contains(track.path)) continue;
        if (!needle.empty() && !contains_folded(track.path.c_str(),needle) &&
            !contains_folded(track.info.title,needle) &&
            !contains_folded(track.info.artist,needle) &&
            !contains_folded(track.info.album,needle)) continue;
        visible.push_back(i);
    }
    std::stable_sort(visible.begin(), visible.end(), [&](size_t a, size_t b) {
        if (sort == 2 && tracks[a].metadata_ready != tracks[b].metadata_ready)
            return tracks[a].metadata_ready; // Unknown duration sorts after decoded metadata.
        if (sort == 2 && tracks[a].info.duration_ms != tracks[b].info.duration_ms)
            return tracks[a].info.duration_ms < tracks[b].info.duration_ms;
        const int order = compare_folded(sort == 1 ? tracks[a].info.title : tracks[a].path.c_str(),
                                         sort == 1 ? tracks[b].info.title : tracks[b].path.c_str());
        return order == 0 ? tracks[a].path < tracks[b].path : order < 0;
    });
}
int MusicLibrary::find_visible(const std::string &path) const {
    for (size_t i = 0; i < visible.size(); ++i) if (tracks[visible[i]].path == path) return static_cast<int>(i);
    return -1;
}
uint32_t ShuffleBag::random() {
    uint32_t x = random_; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return random_ = x;
}
void ShuffleBag::fill(int current) {
    bag_.clear(); cursor_ = 0;
    for (size_t i = 0; i < count_; ++i) if (static_cast<int>(i) != current) bag_.push_back(static_cast<int>(i));
    for (size_t i = bag_.size(); i > 1; --i) std::swap(bag_[i - 1], bag_[random() % i]);
}
void ShuffleBag::reset(size_t count, int current, uint32_t seed) {
    count_ = count; random_ = seed ? seed : 1; fill(current);
}
int ShuffleBag::next(int current, bool wrap) {
    if (count_ == 0) return -1;
    if (cursor_ == bag_.size()) {
        if (!wrap) return -1;
        fill(current);
        if (bag_.empty()) return current;
    }
    return bag_[cursor_++];
}
}

namespace player {
std::vector<size_t> MusicLibrary::folder_queue(const std::string &folder, uint8_t sort, const FavoritesStore *favorites) const {
    std::vector<size_t> result;result.reserve(tracks.size());
    for(size_t i=0;i<tracks.size();++i)
        if(valid_music_path(tracks[i].path,folder) && (!favorites || favorites->contains(tracks[i].path)))result.push_back(i);
    std::stable_sort(result.begin(),result.end(),[&](size_t a,size_t b){
        const auto &x=tracks[a],&y=tracks[b];
        if(sort==2 && x.info.duration_ms!=y.info.duration_ms)return (x.info.duration_ms?x.info.duration_ms:UINT32_MAX)<(y.info.duration_ms?y.info.duration_ms:UINT32_MAX);
        int c=compare_folded(sort==1?x.info.title:x.path.c_str(),sort==1?y.info.title:y.path.c_str());
        return c?c<0:x.path<y.path;
    });return result;
}
}
