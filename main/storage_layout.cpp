#include "storage_layout.h"
#include "player_store.h"
#include "eq_store.h"
#include "reverb_store.h"
#include "favorites_store.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <vector>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

namespace player {
namespace {
enum class Kind { Missing, File, Directory, Other, Error };
enum class Family { Settings, Eq, Reverb, Favorites };
struct Saved { bool valid = false; uint64_t sequence = 0; bool io_error = false; };
struct Migration {
    StorageLayout result;
    const StorageMigrationHooks *hooks;
    std::string root, aurora, legacy, settings, cache, archive, music;

    Migration(std::string mount, const StorageMigrationHooks *h) : hooks(h), root(std::move(mount)) {
        while (root.size() > 1 && root.back() == '/') root.pop_back();
        aurora = root + "/Aurora"; legacy = root + "/.tab5-player";
        settings = aurora + "/Settings"; cache = aurora + "/Cache";
        archive = aurora + "/Legacy"; music = root + "/Music";
        result.settings_dir = result.eq_dir = result.reverb_dir = result.favorites_dir = settings;
        result.cache_file = cache + "/catalog.bin";
        result.screenshots_dir = aurora + "/Screenshots";
    }
    Kind kind(const std::string &path) {
        struct stat info{};
        if (stat(path.c_str(), &info) != 0) {
            if (errno == ENOENT) return Kind::Missing;
            ++result.errors; return Kind::Error;
        }
        if (S_ISDIR(info.st_mode)) return Kind::Directory;
        if (S_ISREG(info.st_mode)) return Kind::File;
        return Kind::Other;
    }
    bool allowed(const char *operation, const std::string &from, const std::string &to = {}) {
        if (!hooks || !hooks->before_mutation ||
            hooks->before_mutation(hooks->context, operation, from.c_str(), to.c_str())) return true;
        ++result.errors; return false;
    }
    bool directory(const std::string &path) {
        const auto existing = kind(path);
        if (existing == Kind::Directory) return true;
        if (existing != Kind::Missing) { if (existing != Kind::Error) ++result.errors; return false; }
        if (!allowed("mkdir", path)) return false;
#ifdef _WIN32
        const int rc = _mkdir(path.c_str());
#else
        const int rc = mkdir(path.c_str(), 0755);
#endif
        if (rc == 0) return true;
        ++result.errors; return false;
    }
    // A pre-existing destination is never passed to rename (POSIX rename
    // otherwise replaces it). The caller serializes every filesystem worker.
    bool move(const std::string &from, const std::string &to, bool recovery = false) {
        if (kind(to) != Kind::Missing) { ++result.errors; return false; }
        if (!allowed("rename", from, to)) return false;
        if (rename(from.c_str(), to.c_str()) != 0) { ++result.errors; return false; }
        ++result.moved; if (recovery) ++result.archived;
        return true;
    }
    bool preserve(const std::string &from, const std::string &basename) {
        const auto source = kind(from);
        if (source == Kind::Missing) return true;
        if (source != Kind::File) { if (source != Kind::Error) ++result.errors; return false; }
        if (!directory(archive)) return false;
        // Existing recovery files may belong to an interrupted previous boot.
        // Keep every one; a bounded search also bounds malformed-card work.
        for (unsigned i = 0; i < 10000; ++i) {
            char suffix[16]; snprintf(suffix, sizeof suffix, ".%04u", i);
            const auto destination = archive + "/" + basename + suffix;
            const auto target = kind(destination);
            if (target == Kind::Missing) return move(from, destination, true);
            if (target == Kind::Error) return false;
        }
        ++result.errors; return false;
    }
    bool readable(const std::string &path) {
        const auto entry = kind(path);
        if (entry == Kind::Missing) return true;
        if (entry != Kind::File) { if (entry != Kind::Error) ++result.errors; return false; }
        if (hooks && hooks->before_read && !hooks->before_read(hooks->context, path.c_str())) {
            ++result.errors; return false;
        }
        FILE *file = fopen(path.c_str(), "rb");
        if (!file) { ++result.errors; return false; }
        // Public stores distinguish valid/corrupt, not I/O failures. Probe the
        // bytes before interpreting their result; an unreadable newer record
        // must not be archived as if it were an invalid CRC. The largest valid
        // record is 10,000 bounded favorite paths (under 2.5 MiB).
        uint8_t buffer[2048]; size_t total = 0;
        while (total <= 2500000) { const auto n = fread(buffer, 1, sizeof buffer, file); total += n; if (n != sizeof buffer) break; }
        bool ok = !ferror(file);
        if (fclose(file) != 0) ok = false;
        if (!ok) ++result.errors;
        return ok;
    }
    Saved saved(Family family, const std::string &dir, bool probe_io = false) {
        if (probe_io) {
            const char *stem = family == Family::Settings ? "settings" : family == Family::Eq ? "eq" : family == Family::Reverb ? "reverb" : "favorites";
            if (!readable(dir + "/" + stem + "-a.bin") || !readable(dir + "/" + stem + "-b.bin")) return {false, 0, true};
        }
        switch (family) {
            case Family::Settings: {
                SettingsStore store(dir); Settings value;
                const bool valid = store.load(value); return {valid, store.sequence()};
            }
            case Family::Eq: {
                EqStore store(dir); EqBank value;
                const bool valid = store.load(value); return {valid, store.sequence()};
            }
            case Family::Reverb: {
                ReverbStore store(dir); audio_reverb_config_t value{};
                const bool valid = store.load(value); return {valid, store.sequence()};
            }
            case Family::Favorites: {
                FavoritesStore store(dir, music);
                const bool valid = store.load(); return {valid, store.sequence()};
            }
        }
        return {};
    }
    static bool newer(const Saved &a, const Saved &b) {
        return a.valid && (!b.valid || a.sequence > b.sequence);
    }
    void family(Family type, const char *stem, std::string &selected) {
        // Ordinary boots need no conflict/CRC/sequence comparison. Only check
        // I/O health, so a prior migration read failure cannot enable writes
        // again while the active record is still unreadable.
        if (kind(legacy + "/" + stem + "-a.bin") == Kind::Missing &&
            kind(legacy + "/" + stem + "-b.bin") == Kind::Missing) {
            if (!readable(settings + "/" + stem + "-a.bin") || !readable(settings + "/" + stem + "-b.bin")) result.persistent_read_error = true;
            return;
        }
        const auto old = saved(type, legacy, true), current = saved(type, settings, true);
        if (old.io_error || current.io_error) {
            // Leave both active copies exactly where they are. Retry next boot;
            // never move an unreadable high-sequence record out of consideration.
            selected = newer(old, current) || (!current.valid && old.io_error) ? legacy : settings;
            result.persistent_read_error = true;
            if (selected == legacy) result.using_legacy = true;
            return;
        }
        bool proceed = true;
        if (newer(old, current)) {
            // The newest complete source remains in legacy while every older
            // destination slot is archived. Then move A/B individually; after
            // any interruption the newest complete slot remains in one dir.
            for (const char slot : {'a', 'b'}) {
                const std::string name = std::string(stem) + "-" + slot + ".bin";
                if (!preserve(settings + "/" + name, "settings-" + name)) { proceed = false; break; }
            }
            if (proceed) for (const char slot : {'a', 'b'}) {
                const std::string name = std::string(stem) + "-" + slot + ".bin";
                const auto from = legacy + "/" + name;
                const auto source = kind(from);
                if (source == Kind::Missing) continue;
                if (source != Kind::File || !move(from, settings + "/" + name)) {
                    if (source != Kind::File && source != Kind::Error) ++result.errors;
                    break;
                }
            }
        } else {
            // Ties deliberately favor the current directory. Corrupt records
            // are retained for recovery but never allowed to outrank valid CRCs.
            for (const char slot : {'a', 'b'}) {
                const std::string name = std::string(stem) + "-" + slot + ".bin";
                if (!preserve(legacy + "/" + name, "legacy-" + name)) break;
            }
        }
        // Read again after mutations: this is also the correct result after an
        // injected failure or card becoming read-only partway through migration.
        const auto remaining = saved(type, legacy, true), migrated = saved(type, settings, true);
        if (remaining.io_error || migrated.io_error) result.persistent_read_error = true;
        selected = newer(remaining, migrated) || (!migrated.valid && remaining.io_error) ? legacy : settings;
        if (selected == legacy) result.using_legacy = true;
    }
    void cached_files(const std::string &from) {
        const auto source = from + "/catalog.bin", target = cache + "/catalog.bin";
        const auto source_kind = kind(source), target_kind = kind(target);
        if (source_kind == Kind::File && target_kind == Kind::Missing) {
            // An orphan destination checkpoint could be left by a different
            // cache. Never attach it to the incoming catalog accidentally.
            if (preserve(target + ".index", "cache-catalog.bin.index") && move(source, target)) {
                const auto index = source + ".index";
                if (kind(index) == Kind::File) move(index, target + ".index");
            }
        } else if (source_kind == Kind::File && target_kind == Kind::File) {
            // Metadata caches are disposable. Keep both catalog versions for
            // recovery and force one proper scan instead of trusting an index
            // from a different catalog after a conflict/interrupted migration.
            if (preserve(target + ".index", "cache-catalog.bin.index")) {
                preserve(source + ".index", "legacy-catalog.bin.index");
                preserve(source, "legacy-catalog.bin");
            }
        } else if (source_kind != Kind::Missing && source_kind != Kind::Error) {
            ++result.errors;
        }
        // Without its original catalog an orphan index cannot certify the new
        // one; retaining it as recovery data forces safe rebuilding on resume.
        if (kind(source) == Kind::Missing) preserve(source + ".index", "orphan-catalog.bin.index");
        preserve(source + ".index.tmp", "catalog.bin.index.tmp");
        preserve(source + ".tmp", "catalog.bin.tmp");
    }
    static bool screenshot_name(const char *name) {
        if (strlen(name) != 16 || strncmp(name, "Aurora-", 7) != 0 || strcmp(name + 12, ".bmp") != 0) return false;
        for (unsigned i = 7; i < 12; ++i) if (name[i] < '0' || name[i] > '9') return false;
        return true;
    }
    void screenshots() {
        const auto from = root + "/Screenshots";
        if (kind(from) != Kind::Directory) return;
        if (kind(result.screenshots_dir) != Kind::Directory) return;
        DIR *dir = opendir(from.c_str());
        if (!dir) { ++result.errors; return; }
        // Collect before moving: FAT directory cursor positions must not be
        // changed by a rename while readdir is traversing the same directory.
        std::vector<std::string> names;
        while (const auto *entry = readdir(dir)) if (screenshot_name(entry->d_name)) names.emplace_back(entry->d_name);
        if (closedir(dir) != 0) ++result.errors;
        for (const auto &name : names) {
            const auto source = from + "/" + name;
            if (kind(source) != Kind::File) continue; // Never move same-named directories.
            const auto destination = result.screenshots_dir + "/" + name;
            const auto existing = kind(destination);
            if (existing == Kind::Missing) move(source, destination);
            else if (existing != Kind::Error) {
                // Keep the existing screenshot name, and put a collision in
                // the same Screenshots directory with an explicit suffix.
                for (unsigned i = 1; i <= 9999; ++i) {
                    char suffix[24]; snprintf(suffix, sizeof suffix, "-legacy-%04u.bmp", i);
                    const auto alternative = result.screenshots_dir + "/" + name.substr(0, 12) + suffix;
                    const auto candidate = kind(alternative);
                    if (candidate == Kind::Missing) { move(source, alternative); break; }
                    if (candidate == Kind::Error) break;
                    if (i == 9999) ++result.errors;
                }
            }
        }
        remove_empty(from);
    }
    void remove_empty(const std::string &path) {
        if (kind(path) != Kind::Directory) return;
        DIR *dir = opendir(path.c_str());
        if (!dir) { ++result.errors; return; }
        bool empty = true;
        while (const auto *entry = readdir(dir)) if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) { empty = false; break; }
        closedir(dir);
        if (!empty || !allowed("rmdir", path)) return;
#ifdef _WIN32
        const int rc = _rmdir(path.c_str());
#else
        const int rc = rmdir(path.c_str());
#endif
        if (rc != 0) ++result.errors;
    }
    StorageLayout run() {
        if (!directory(aurora)) {
            result.settings_dir = result.eq_dir = result.reverb_dir = result.favorites_dir = legacy;
            result.cache_file = legacy + "/catalog.bin";
            result.screenshots_dir = root + "/Screenshots";
            result.using_legacy = true;
            for (const char *stem : {"settings", "eq", "reverb", "favorites"})
                if (!readable(legacy + "/" + stem + "-a.bin") || !readable(legacy + "/" + stem + "-b.bin")) result.persistent_read_error = true;
            return result;
        }
        if (kind(settings) == Kind::Missing && kind(legacy) == Kind::Directory) move(legacy, settings);
        const bool settings_ready = directory(settings);
        const bool cache_ready = directory(cache);
        if (!directory(result.screenshots_dir)) result.screenshots_dir = root + "/Screenshots";
        directory(aurora + "/Lockscreen");
        if (settings_ready) {
            family(Family::Settings, "settings", result.settings_dir);
            family(Family::Eq, "eq", result.eq_dir);
            family(Family::Reverb, "reverb", result.reverb_dir);
            family(Family::Favorites, "favorites", result.favorites_dir);
        } else {
            result.settings_dir = result.eq_dir = result.reverb_dir = result.favorites_dir = legacy;
            result.using_legacy = true;
            for (const char *stem : {"settings", "eq", "reverb", "favorites"})
                if (!readable(legacy + "/" + stem + "-a.bin") || !readable(legacy + "/" + stem + "-b.bin")) result.persistent_read_error = true;
        }
        if (cache_ready) { cached_files(settings); cached_files(legacy); }
        if (!cache_ready || kind(result.cache_file) != Kind::File) {
            if (kind(settings + "/catalog.bin") == Kind::File) result.cache_file = settings + "/catalog.bin";
            else if (kind(legacy + "/catalog.bin") == Kind::File) result.cache_file = legacy + "/catalog.bin";
        }
        if (result.cache_file == legacy + "/catalog.bin") result.using_legacy = true;
        if (result.screenshots_dir != root + "/Screenshots") screenshots();
        remove_empty(legacy);
        return result;
    }
};
}

StorageLayout prepare_storage_layout(const std::string &root, const StorageMigrationHooks *hooks) {
    return Migration(root, hooks).run();
}
}
