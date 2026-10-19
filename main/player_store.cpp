#include "player_store.h"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace player {
uint32_t crc32(const void *data, size_t size) {
    struct Table { uint32_t v[256]; Table() { for(unsigned i=0;i<256;i++) { uint32_t c=i;for(int b=0;b<8;b++)c=(c>>1)^(0xedb88320U&(0U-(c&1U)));v[i]=c; } } };
    static const Table table;
    uint32_t crc = 0xffffffffU;
    const auto *p = static_cast<const uint8_t *>(data);
    while (size--) crc = (crc >> 8) ^ table.v[(crc ^ *p++) & 255];
    return ~crc;
}
bool valid_music_path(const std::string &path, const std::string &root) {
    if (path.size() >= PATH_LIMIT || path.size() < root.size()) return false;
    if (path.compare(0, root.size(), root) != 0) return false;
    if (path.size() > root.size() && path[root.size()] != '/') return false;
    if (path.find('\\') != std::string::npos || path.find('\0') != std::string::npos) return false;
    size_t start = 0;
    while (start < path.size()) {
        size_t end = path.find('/', start);
        const size_t length=(end==std::string::npos?path.size():end)-start;
        if ((length==1&&path[start]=='.') || (length==2&&path[start]=='.'&&path[start+1]=='.')) return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}
// Fixed layout, private versioned format. A failed save never overwrites the newest valid slot.
struct Record {
    uint32_t magic, version, payload_size, reserved;
    uint64_t sequence;
    Settings data;
    uint32_t checksum;
};
struct LegacySettings {
    uint8_t volume, brightness, repeat, visual, shuffle, muted, grid, sort;
    uint32_t resume_ms, dim_seconds;
    char last_path[PATH_LIMIT], folder[PATH_LIMIT];
};
struct LegacyRecord {
    uint32_t magic, version, payload_size, reserved;
    uint64_t sequence;
    LegacySettings data;
    uint32_t checksum;
};
/* Preserve the exact released v2 layout, including its one byte of tail
 * padding. Do not reinterpret it as Settings: v3 uses that byte for effect. */
struct LegacySettingsV2 {
    uint8_t volume, brightness, repeat, visual, shuffle, muted, grid, sort;
    uint32_t resume_ms, dim_seconds;
    char last_path[PATH_LIMIT], folder[PATH_LIMIT];
    uint8_t bt_enabled, local_output, bt_volume, receiver_count, covers;
    char bt_target[18];
    SavedReceiver receivers[8];
};
struct LegacyRecordV2 {
    uint32_t magic, version, payload_size, reserved;
    uint64_t sequence;
    LegacySettingsV2 data;
    uint32_t checksum;
};
// Version 3 payload includes two bytes of tail padding. Version 4 begins
// using those bytes, so read and verify the released record independently.
struct LegacySettingsV3 {
    uint8_t volume, brightness, repeat, visual, shuffle, muted, grid, sort;
    uint32_t resume_ms, dim_seconds;
    char last_path[PATH_LIMIT], folder[PATH_LIMIT];
    uint8_t bt_enabled, local_output, bt_volume, receiver_count, covers;
    char bt_target[18]; SavedReceiver receivers[8];
    uint8_t effect, library_tab, format_filter;
};
struct LegacyRecordV3 {
    uint32_t magic, version, payload_size, reserved;uint64_t sequence;
    LegacySettingsV3 data;uint32_t checksum;
};
static_assert(sizeof(LegacySettingsV3)==1308,"Released v3 ABI changed");
static_assert(sizeof(Settings)==1312 && offsetof(Settings,speed_percent)==1311,"Settings v4/v5 ABI changed");
static_assert(offsetof(Settings,dragon_type)==1306,"v4 prefix changed");
static_assert(sizeof(LegacySettings) == 496, "Released settings v1 ABI changed");
static_assert(sizeof(LegacySettingsV2) == 1304, "Released settings v2 ABI changed");
static_assert(offsetof(Settings, bt_enabled) == sizeof(LegacySettings), "Settings prefix changed");
static_assert(offsetof(Settings, effect) == 1303, "Settings v2 payload prefix changed");
template<class Old> static void migrate_base(Settings &to, const Old &old) {
    to = Settings{};
    to.volume=old.volume;to.brightness=old.brightness;to.repeat=old.repeat;to.visual=old.visual;
    to.shuffle=old.shuffle;to.muted=old.muted;to.grid=old.grid;to.sort=old.sort;
    to.resume_ms=old.resume_ms;to.dim_seconds=old.dim_seconds;
    memcpy(to.last_path,old.last_path,sizeof to.last_path);
    memcpy(to.folder,old.folder,sizeof to.folder);
}
static bool valid_mac(const char *mac, bool empty = false) {
    if (empty && !mac[0]) return true;
    for (int i = 0; i < 17; ++i) {
        if (i % 3 == 2) { if (mac[i] != ':') return false; }
        else if (!((mac[i] >= '0' && mac[i] <= '9') || (mac[i] >= 'A' && mac[i] <= 'F') || (mac[i] >= 'a' && mac[i] <= 'f'))) return false;
    }
    return mac[17] == 0;
}
static bool sane(const Settings &s) {
    const bool basic = s.volume <= 100 && s.brightness >= 10 && s.brightness <= 100 && s.repeat <= 2 &&
        s.visual <= 2 && s.shuffle <= 1 && s.muted <= 1 && s.grid <= 1 && s.sort <= 2 &&
        s.dim_seconds <= 3600 && memchr(s.last_path, 0, sizeof s.last_path) &&
        memchr(s.folder, 0, sizeof s.folder);
    if (!basic || s.bt_enabled > 1 || s.local_output > 1 || s.bt_volume > 100 ||
        s.receiver_count > 8 || s.covers > 1 || s.effect > 3 || s.library_tab > 2 ||
        s.format_filter > 4 || s.dragon_type > 1 || s.dragon_speed < 4 || s.dragon_speed > 16 || s.dragon_colors > 1 || s.animations > 1 || s.clock24 > 1 || s.speed_percent < 75 || s.speed_percent > 125 || !valid_mac(s.bt_target, true)) return false;
    for (unsigned i = 0; i < s.receiver_count; ++i)
        if (!valid_mac(s.receivers[i].mac) || !memchr(s.receivers[i].name, 0, sizeof s.receivers[i].name)) return false;
    return true;
}
static bool read_record(const std::string &path, Record &r) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint32_t header[3]{};
    if (fread(header, sizeof header, 1, f) != 1 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }
    if (header[0] == 0x35504154 && header[1] == 1 && header[2] == sizeof(LegacySettings)) {
        LegacyRecord old{};
        const bool valid = fread(&old, sizeof old, 1, f) == 1 && fgetc(f) == EOF && !ferror(f) &&
            old.checksum == crc32(&old, offsetof(LegacyRecord, checksum));
        fclose(f);
        if (!valid) return false;
        r = {}; r.magic = old.magic; r.version = 5; r.payload_size = sizeof(Settings); r.sequence = old.sequence;
        migrate_base(r.data, old.data);
        return sane(r.data);
    }
    if (header[0] == 0x35504154 && header[1] == 2 && header[2] == sizeof(LegacySettingsV2)) {
        LegacyRecordV2 old{};
        const bool valid = fread(&old, sizeof old, 1, f) == 1 && fgetc(f) == EOF && !ferror(f) &&
            old.checksum == crc32(&old, offsetof(LegacyRecordV2, checksum));
        fclose(f);
        if (!valid) return false;
        r={};r.magic=old.magic;r.version=5;r.payload_size=sizeof(Settings);r.sequence=old.sequence;
        migrate_base(r.data,old.data);
        r.data.bt_enabled=old.data.bt_enabled;r.data.local_output=old.data.local_output;
        r.data.bt_volume=old.data.bt_volume;r.data.receiver_count=old.data.receiver_count;r.data.covers=old.data.covers;
        memcpy(r.data.bt_target,old.data.bt_target,sizeof r.data.bt_target);
        memcpy(r.data.receivers,old.data.receivers,sizeof r.data.receivers);
        return sane(r.data);
    }
    if (header[0]==0x35504154 && header[1]==3 && header[2]==sizeof(LegacySettingsV3)) {
        LegacyRecordV3 old{};
        const bool valid=fread(&old,sizeof old,1,f)==1 && fgetc(f)==EOF && !ferror(f) && old.checksum==crc32(&old,offsetof(LegacyRecordV3,checksum));
        fclose(f);if(!valid)return false;
        r={};r.magic=old.magic;r.version=5;r.payload_size=sizeof(Settings);r.sequence=old.sequence;
        r.data=Settings{};
        migrate_base(r.data,old.data);
        r.data.bt_enabled=old.data.bt_enabled;r.data.local_output=old.data.local_output;
        r.data.bt_volume=old.data.bt_volume;r.data.receiver_count=old.data.receiver_count;r.data.covers=old.data.covers;
        memcpy(r.data.bt_target,old.data.bt_target,sizeof r.data.bt_target);
        memcpy(r.data.receivers,old.data.receivers,sizeof r.data.receivers);
        r.data.effect=old.data.effect;r.data.library_tab=old.data.library_tab;r.data.format_filter=old.data.format_filter;
        return sane(r.data);
    }
    const bool ok = fread(&r, 1, sizeof r, f) == sizeof r && fgetc(f) == EOF && !ferror(f);
    fclose(f);
    if(!ok || r.magic!=0x35504154 || (r.version!=4 && r.version!=5) || r.payload_size!=sizeof(Settings) ||
       r.checksum!=crc32(&r,offsetof(Record,checksum)))return false;
    if(r.version==4)r.data.speed_percent=100; // Ignore released v4 padding, after CRC verification.
    return sane(r.data);
}
SettingsStore::SettingsStore(std::string directory) : directory_(std::move(directory)) {}
bool SettingsStore::load(Settings &out) {
    Record a{}, b{};
    bool va = read_record(directory_ + "/settings-a.bin", a);
    bool vb = read_record(directory_ + "/settings-b.bin", b);
    if (!va && !vb) { active_ = -1; sequence_ = 0; return false; }
    active_ = vb && (!va || b.sequence > a.sequence) ? 1 : 0;
    const auto &best = active_ ? b : a;
    sequence_ = best.sequence;
    out = best.data;
    return true;
}
bool SettingsStore::save(const Settings &settings) {
    if (!sane(settings) || sequence_ == UINT64_MAX) return false;
#ifdef _WIN32
    if (_mkdir(directory_.c_str()) != 0 && errno != EEXIST) return false;
#else
    if (mkdir(directory_.c_str(), 0755) != 0 && errno != EEXIST) return false;
#endif
    Record r{};
    r.magic = 0x35504154; r.version = 5; r.payload_size = sizeof(Settings);
    r.sequence = sequence_ + 1; r.data = settings;
    r.checksum = crc32(&r, offsetof(Record, checksum));
    const int target = active_ == 0 ? 1 : 0;
    std::string path = directory_ + (target ? "/settings-b.bin" : "/settings-a.bin");
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(&r, 1, sizeof r, f) == sizeof r && fflush(f) == 0;
#ifdef _WIN32
    if (ok) ok = _commit(_fileno(f)) == 0;
#else
    if (ok) ok = fsync(fileno(f)) == 0;
#endif
    if (fclose(f) != 0) ok = false;
    Record verify{};
    if (ok) ok = read_record(path, verify) && verify.sequence == r.sequence;
    if (ok) { sequence_ = r.sequence; active_ = target; }
    return ok;
}
}
