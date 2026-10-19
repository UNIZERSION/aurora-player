#pragma once
#include "audio_file.h"
#include "player_store.h"
#include <vector>
#include <string>
#include <cstdint>

namespace player {
class FavoritesStore;
struct Track {
    std::string path;
    audio_file_info_t info{};
    int64_t modified = 0;
    bool metadata_ready = false;
};
struct ScanResult {
    size_t unsupported = 0, skipped = 0, cached = 0, probed = 0;
    bool readable = false, limited = false, out_of_memory = false;
};
using ScanProgress = void (*)(size_t visited, size_t found, size_t cached, void *context);
class MusicLibrary {
public:
    static constexpr size_t MAX_TRACKS = 10000;
    ScanResult scan(const std::string &root, const std::string &cache_file,
                    ScanProgress progress = nullptr, void *context = nullptr,
                    bool probe_metadata = true, uint64_t volume_id = 0);
    // Fast startup: validate sequential cache records without opening/stat-ing
    // music files. A card/root-bound checkpoint is written after a full scan.
    // External file changes become visible after an explicit scan.
    bool load_index(const std::string &root, const std::string &cache_file, uint64_t volume_id = 0);
    // Lazy scan enumerates filenames/stat only. Enrich from the decoder after
    // playback opens a file. Both methods require external serialization with scan.
    bool update_metadata(size_t underlying_track_index, const audio_file_info_t &info);
    // Call only outside audio playback and LVGL locks. Writes fixed dirty CRC
    // records in place; returns records durably written, 0 if none or unavailable.
    size_t flush_metadata_cache(size_t max_records = 1);
    // Controller selects the chosen subtree for Queue and root for Favorites.
    // Files displays folders separately. Format IDs match audio_format_t:
    // 0 all, 1 MP3, 2 WAV, 3 FLAC, 4 AAC.
    // favourites_only with a null store yields an empty view, never all tracks.
    void filter_sort(const std::string &folder, uint8_t sort, const std::string &query = {},
                     uint8_t format_filter = 0, const FavoritesStore *favorites = nullptr,
                     bool favorites_only = false);
    // Playback queues are independent of the visible search/format filter.
    // A non-null favorites store restricts membership; it never falls back to
    // ordinary folder tracks when no favorites match.
    std::vector<size_t> folder_queue(const std::string &folder, uint8_t sort = 0,
                                     const FavoritesStore *favorites = nullptr) const;
    std::vector<Track> tracks;
    std::vector<size_t> visible;
    std::vector<std::string> folders;
    int find_visible(const std::string &path) const;
private:
    std::string cache_file_;
    std::vector<uint8_t> metadata_dirty_;
    std::vector<uint32_t> cache_record_crc_;
    bool cache_writable_ = false;
};
// Fisher-Yates bag. Each item occurs once per pass and boundaries avoid repeats.
class ShuffleBag {
public:
    void reset(size_t count, int current, uint32_t seed);
    int next(int current, bool wrap);
private:
    std::vector<int> bag_;
    size_t cursor_ = 0, count_ = 0;
    uint32_t random_ = 1;
    uint32_t random();
    void fill(int current);
};
}
