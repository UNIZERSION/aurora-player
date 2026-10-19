#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace player {
constexpr size_t PATH_LIMIT = 240; // IDF FAT VFS temporary path buffer is 255 bytes.
struct SavedReceiver { char mac[18] = {}, name[80] = {}; };
struct Settings {
    uint8_t volume = 60, brightness = 80, repeat = 0, visual = 0;
    uint8_t shuffle = 0, muted = 0, grid = 0, sort = 0;
    uint32_t resume_ms = 0;
    uint32_t dim_seconds = 30;
    char last_path[PATH_LIMIT] = {};
    char folder[PATH_LIMIT] = {};
    uint8_t bt_enabled = 0, local_output = 1, bt_volume = 60, receiver_count = 0;
    uint8_t covers = 1;
    char bt_target[18] = {};
    SavedReceiver receivers[8]{};
    uint8_t effect = 0;        // 0..3 audio preset identifier
    uint8_t library_tab = 0;   // 0 playlist, 1 local folder, 2 favourites
    uint8_t format_filter = 0; // 0 all, 1 MP3, 2 WAV, 3 FLAC, 4 AAC
    uint8_t dragon_type = 1, dragon_speed = 10, dragon_colors = 1;
    uint8_t animations = 1, clock24 = 1;
    uint8_t speed_percent = 100; // v5 uses the last byte of v4 tail padding.
};
uint32_t crc32(const void *data, size_t size);
bool valid_music_path(const std::string &path, const std::string &root);
class SettingsStore {
public:
    explicit SettingsStore(std::string directory);
    bool load(Settings &out);
    bool save(const Settings &settings);
    uint64_t sequence() const { return sequence_; }
private:
    std::string directory_;
    uint64_t sequence_ = 0;
    int active_ = -1;
};
}
