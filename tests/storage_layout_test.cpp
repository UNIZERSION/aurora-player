#undef NDEBUG
#include "storage_layout.h"
#include "player_store.h"
#include "eq_store.h"
#include "reverb_store.h"
#include "favorites_store.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
namespace fs = std::filesystem;
using player::StorageLayout;
static std::string path(const fs::path &p) { return p.generic_string(); }
static void write(const fs::path &p, const std::string &bytes) {
    fs::create_directories(p.parent_path()); std::ofstream file(p, std::ios::binary);
    file << bytes; assert(file.good());
}
static std::string read(const fs::path &p) {
    std::ifstream file(p, std::ios::binary); return {std::istreambuf_iterator<char>(file), {}};
}
static unsigned file_count(const fs::path &dir) {
    unsigned count = 0; if (fs::exists(dir)) for (const auto &f : fs::recursive_directory_iterator(dir)) if (f.is_regular_file()) ++count;
    return count;
}
struct Expected { unsigned settings = 0, eq = 0, reverb = 0, favorites = 0; };
static void seed(const fs::path &root, const fs::path &dir, unsigned saves, unsigned base) {
    fs::create_directories(dir); fs::create_directories(root / "Music");
    player::SettingsStore settings(path(dir)); player::EqStore eq(path(dir));
    player::ReverbStore reverb(path(dir)); player::FavoritesStore favorites(path(dir), path(root / "Music"));
    for (unsigned n = 0; n < saves; ++n) {
        player::Settings s; s.volume = uint8_t(base + n); assert(settings.save(s));
        player::EqBank e; e.current.gain_db[0] = int8_t(base / 10 + n); assert(eq.save(e));
        auto r = audio_reverb_default_config(); r.amount = uint8_t(base / 2 + n); assert(reverb.save(r));
        assert(favorites.set(path(root / "Music" / (std::to_string(base + n) + ".mp3")), true));
        assert(favorites.save());
    }
}
static Expected inspect(const fs::path &root, const StorageLayout &layout) {
    Expected out;
    player::SettingsStore settings(layout.settings_dir); player::Settings s;
    assert(settings.load(s)); out.settings = s.volume;
    player::EqStore eq(layout.eq_dir); player::EqBank e;
    assert(eq.load(e)); out.eq = unsigned(e.current.gain_db[0]);
    player::ReverbStore reverb(layout.reverb_dir); audio_reverb_config_t r{};
    assert(reverb.load(r)); out.reverb = r.amount;
    player::FavoritesStore favorites(layout.favorites_dir, path(root / "Music"));
    assert(favorites.load()); out.favorites = unsigned(favorites.size());
    return out;
}
static void expect(const fs::path &root, const StorageLayout &layout, const Expected &expected) {
    const auto actual = inspect(root, layout);
    assert(actual.settings == expected.settings && actual.eq == expected.eq &&
           actual.reverb == expected.reverb && actual.favorites == expected.favorites);
}
static StorageLayout choose(const fs::path &root) {
    // Read-only newest-valid selection when a simulated abrupt stop prevented
    // prepare_storage_layout from returning. It uses public production stores.
    StorageLayout selected; const std::string old = path(root / ".tab5-player"), now = path(root / "Aurora/Settings");
    player::SettingsStore sa(old), sb(now); player::Settings s;
    const bool va = sa.load(s), vb = sb.load(s);
    selected.settings_dir = va && (!vb || sa.sequence() > sb.sequence()) ? old : now;
    player::EqStore ea(old), eb(now); player::EqBank e;
    const bool eva = ea.load(e), evb = eb.load(e);
    selected.eq_dir = eva && (!evb || ea.sequence() > eb.sequence()) ? old : now;
    player::ReverbStore ra(old), rb(now); audio_reverb_config_t r{};
    const bool rva = ra.load(r), rvb = rb.load(r);
    selected.reverb_dir = rva && (!rvb || ra.sequence() > rb.sequence()) ? old : now;
    player::FavoritesStore fa(old, path(root / "Music")), fb(now, path(root / "Music"));
    const bool fva = fa.load(), fvb = fb.load();
    selected.favorites_dir = fva && (!fvb || fa.sequence() > fb.sequence()) ? old : now;
    return selected;
}
struct Fault {
    unsigned calls = 0, fail_at = 0;
    bool abrupt = false, after_failure = false;
    static bool before(void *context, const char *, const char *, const char *) {
        auto &f = *static_cast<Fault *>(context);
        ++f.calls;
        if (f.calls == f.fail_at || f.after_failure) {
            if (f.abrupt) throw std::runtime_error("simulated power loss");
            f.after_failure = true; return false;
        }
        return true;
    }
};
static void setup_conflict(const fs::path &root) {
    seed(root, root / ".tab5-player", 4, 70); // Newest B, A remains a valid previous save.
    seed(root, root / "Aurora/Settings", 2, 20);
    write(root / ".tab5-player/catalog.bin", "legacy catalog");
    write(root / ".tab5-player/catalog.bin.index", "legacy index");
    write(root / "Aurora/Cache/catalog.bin", "current catalog");
    write(root / "Aurora/Cache/catalog.bin.index", "current index");
    write(root / "Screenshots/Aurora-00001.bmp", "old screenshot");
    write(root / "Aurora/Screenshots/Aurora-00001.bmp", "new screenshot");
    write(root / "Screenshots/unrelated.bmp", "leave alone");
    write(root / "Aurora/Lockscreen/wall.jpg", "user wallpaper");
    write(root / "Aurora/startup.jpg", "user startup");
}
int main(int argc, char **argv) {
    assert(argc == 2); const fs::path base = argv[1]; assert(!fs::exists(base)); fs::create_directories(base);
    const Expected legacy{73, 10, 38, 4};
    {
        const auto root = base / "normal"; seed(root, root / ".tab5-player", 4, 70);
        write(root / ".tab5-player/catalog.bin", "catalog"); write(root / ".tab5-player/catalog.bin.index", "index");
        write(root / ".tab5-player/catalog.bin.index.tmp", "interrupted index");
        write(root / ".tab5-player/unknown.txt", "keep");
        write(root / "Screenshots/Aurora-00003.bmp", "screen");
        write(root / "Screenshots/Other-00003.bmp", "not Aurora");
        write(root / "Screenshots/Aurora-1234.bmp", "not exact Aurora name");
        fs::create_directories(root / "Screenshots/Aurora-00004.bmp");
        auto layout = player::prepare_storage_layout(path(root)); assert(layout.errors == 0 && !layout.using_legacy);
        expect(root, layout, legacy); assert(!fs::exists(root / ".tab5-player"));
        assert(read(root / "Aurora/Settings/unknown.txt") == "keep");
        assert(read(root / "Aurora/Cache/catalog.bin") == "catalog" && read(root / "Aurora/Cache/catalog.bin.index") == "index");
        assert(read(root / "Aurora/Screenshots/Aurora-00003.bmp") == "screen");
        assert(read(root / "Screenshots/Other-00003.bmp") == "not Aurora");
        assert(fs::is_directory(root / "Screenshots/Aurora-00004.bmp"));
        assert(fs::exists(root / "Screenshots/Aurora-1234.bmp"));
        const auto count = file_count(root); layout = player::prepare_storage_layout(path(root));
        assert(layout.moved == 0 && layout.errors == 0 && file_count(root) == count); expect(root, layout, legacy);
    }
    unsigned operations = 0;
    {
        const auto root = base / "conflict"; setup_conflict(root); const auto files = file_count(root);
        Fault f; player::StorageMigrationHooks hooks{&f, Fault::before};
        auto layout = player::prepare_storage_layout(path(root), &hooks); operations = f.calls;
        assert(layout.errors == 0 && !layout.using_legacy && layout.archived >= 10); expect(root, layout, legacy);
        assert(file_count(root) == files); // No data discarded, including both cache/checkpoint versions.
        assert(read(root / "Aurora/Screenshots/Aurora-00001.bmp") == "new screenshot");
        assert(read(root / "Aurora/Screenshots/Aurora-00001-legacy-0001.bmp") == "old screenshot");
        assert(read(root / "Aurora/Lockscreen/wall.jpg") == "user wallpaper");
        assert(read(root / "Aurora/startup.jpg") == "user startup");
        assert(!fs::exists(root / "Aurora/Cache/catalog.bin.index")); // Conflict must rebuild, not trust a mismatched index.
        layout = player::prepare_storage_layout(path(root)); assert(layout.errors == 0 && layout.moved == 0); expect(root, layout, legacy);
    }
    // Every mutation boundary is interrupted, followed by a cold restart of
    // the real migration. Both abrupt loss and a mounted card turning read-only
    // must preserve newest valid user data and all original file bytes.
    for (const bool abrupt : {false, true}) for (unsigned cut = 1; cut <= operations; ++cut) {
        const auto root = base / (std::string(abrupt ? "cut-" : "failure-") + std::to_string(cut));
        setup_conflict(root); const auto files = file_count(root);
        Fault f; f.fail_at = cut; f.abrupt = abrupt; player::StorageMigrationHooks hooks{&f, Fault::before};
        try { const auto partial = player::prepare_storage_layout(path(root), &hooks); assert(!abrupt && partial.errors); expect(root, partial, legacy); }
        catch (const std::runtime_error &) { assert(abrupt); expect(root, choose(root), legacy); }
        assert(file_count(root) == files);
        const auto resumed = player::prepare_storage_layout(path(root)); assert(resumed.errors == 0 && !resumed.using_legacy);
        expect(root, resumed, legacy); assert(file_count(root) == files);
        const auto repeat = player::prepare_storage_layout(path(root)); assert(repeat.errors == 0 && repeat.moved == 0); expect(root, repeat, legacy);
    }
    unsigned fast_operations = 0;
    const auto seed_fast = [](const fs::path &root) {
        seed(root, root / ".tab5-player", 4, 70);
        write(root / ".tab5-player/catalog.bin", "cached metadata");
        write(root / ".tab5-player/catalog.bin.index", "completed scan");
        write(root / "Screenshots/Aurora-00002.bmp", "screenshot");
    };
    {
        const auto root = base / "fast-count"; seed_fast(root);
        Fault f; player::StorageMigrationHooks hooks{&f, Fault::before};
        const auto layout = player::prepare_storage_layout(path(root), &hooks); fast_operations = f.calls;
        assert(!layout.errors); expect(root, layout, legacy);
    }
    for (const bool abrupt : {false, true}) for (unsigned cut = 1; cut <= fast_operations; ++cut) {
        const auto root = base / (std::string(abrupt ? "fast-cut-" : "fast-failure-") + std::to_string(cut));
        seed_fast(root); const auto files = file_count(root);
        Fault f; f.fail_at = cut; f.abrupt = abrupt; player::StorageMigrationHooks hooks{&f, Fault::before};
        try { const auto partial = player::prepare_storage_layout(path(root), &hooks); assert(!abrupt && partial.errors); expect(root, partial, legacy); }
        catch (const std::runtime_error &) { assert(abrupt); expect(root, choose(root), legacy); }
        assert(file_count(root) == files);
        const auto resumed = player::prepare_storage_layout(path(root)); assert(!resumed.errors && !resumed.using_legacy); expect(root, resumed, legacy);
        assert(file_count(root) == files);
    }
    {
        const auto root = base / "destination-newer";
        seed(root, root / ".tab5-player", 2, 20); seed(root, root / "Aurora/Settings", 4, 70);
        write(root / ".tab5-player/custom-user-file.txt", "unknown must remain");
        const auto files = file_count(root); auto layout = player::prepare_storage_layout(path(root));
        assert(!layout.using_legacy && !layout.errors); expect(root, layout, legacy); assert(file_count(root) == files);
        assert(read(root / ".tab5-player/custom-user-file.txt") == "unknown must remain");
    }
    {
        const auto root = base / "equal-sequence";
        seed(root, root / ".tab5-player", 4, 20); seed(root, root / "Aurora/Settings", 4, 70);
        const auto layout = player::prepare_storage_layout(path(root)); assert(!layout.errors); expect(root, layout, legacy);
    }
    {
        const auto root = base / "corrupt-destination";
        seed(root, root / ".tab5-player", 4, 70); seed(root, root / "Aurora/Settings", 6, 20);
        for (const char *stem : {"settings", "eq", "reverb", "favorites"}) for (const char slot : {'a', 'b'})
            write(root / "Aurora/Settings" / (std::string(stem) + "-" + slot + ".bin"), "truncated newer file");
        const auto files = file_count(root); const auto layout = player::prepare_storage_layout(path(root));
        assert(!layout.errors); expect(root, layout, legacy); assert(file_count(root) == files);
    }
    {
        const auto root = base / "older-a-newer-b"; setup_conflict(root);
        // The higher-sequence destination B is corrupt, so actual store load
        // must select A, compare against all source slots and preserve source B.
        write(root / "Aurora/Settings/settings-b.bin", "broken");
        const auto layout = player::prepare_storage_layout(path(root)); assert(!layout.errors); expect(root, layout, legacy);
    }
    {
        const auto root = base / "fresh"; fs::create_directories(root);
        const auto layout = player::prepare_storage_layout(path(root)); assert(!layout.errors && !layout.using_legacy);
        assert(fs::is_directory(root / "Aurora/Settings") && fs::is_directory(root / "Aurora/Cache") && fs::is_directory(root / "Aurora/Screenshots") && fs::is_directory(root / "Aurora/Lockscreen"));
        assert(!fs::exists(root / ".tab5-player"));
    }
    {
        const auto root = base / "root-blocked"; seed(root, root / ".tab5-player", 4, 70); write(root / "Aurora", "not a directory");
        const auto files = file_count(root); const auto layout = player::prepare_storage_layout(path(root));
        assert(layout.errors && layout.using_legacy); expect(root, layout, legacy); assert(file_count(root) == files);
    }
    {
        const auto root = base / "read-only"; seed(root, root / ".tab5-player", 4, 70);
        Fault f; f.fail_at = 1; player::StorageMigrationHooks hooks{&f, Fault::before};
        const auto layout = player::prepare_storage_layout(path(root), &hooks);
        assert(layout.errors && layout.using_legacy); expect(root, layout, legacy);
        const auto resumed = player::prepare_storage_layout(path(root)); assert(!resumed.errors); expect(root, resumed, legacy);
    }
    {
        const auto root = base / "unreadable-newest"; setup_conflict(root);
        const auto before = read(root / ".tab5-player/settings-b.bin");
        player::StorageMigrationHooks hooks;
        hooks.before_read = [](void *, const char *p) { return std::string(p).find("/.tab5-player/settings-b.bin") == std::string::npos; };
        const auto partial = player::prepare_storage_layout(path(root), &hooks); assert(partial.errors && partial.persistent_read_error);
        expect(root, partial, {21, 10, 38, 4});
        assert(read(root / ".tab5-player/settings-b.bin") == before); // No archival of transiently unreadable records.
        const auto resumed = player::prepare_storage_layout(path(root)); assert(!resumed.errors && !resumed.persistent_read_error); expect(root, resumed, legacy);
    }
    printf("PASS storage migration: real A/B stores, newest-valid conflict resolution, corrupt/newer/tied destinations, %u abrupt power cuts and %u persistent write failures across fast/conflict paths, transient read-error write protection, idempotent retries, cache checkpoint safety, screenshot collisions, unknown-file retention, fresh/read-only/blocked SD.\n", operations + fast_operations, operations + fast_operations);
}
