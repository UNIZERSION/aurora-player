#pragma once
#include <cstdint>
#include <string>

namespace player {
// Paths are selected independently so a failed migration never makes a newer
// valid A/B record disappear from the application's next load/save cycle.
struct StorageLayout {
    std::string settings_dir, eq_dir, reverb_dir, favorites_dir;
    std::string cache_file, screenshots_dir;
    uint32_t moved = 0, archived = 0, errors = 0;
    bool using_legacy = false;
    // A newer record may exist but be temporarily unreadable. The controller
    // must keep durable-store writes disabled until preparation succeeds on a
    // subsequent boot/retry, to avoid overwriting or superseding unknown data.
    bool persistent_read_error = false;
};

// Optional fault-injection/diagnostic hook. Returning false prevents the
// operation. It is called before mkdir, rename and empty-directory rmdir;
// production callers normally omit it. No file content is written here.
struct StorageMigrationHooks {
    void *context = nullptr;
    bool (*before_mutation)(void *context, const char *operation,
                            const char *source, const char *destination) = nullptr;
    bool (*before_read)(void *context, const char *path) = nullptr;
};

// Call after the SD mount and before creating/loading stores or starting any
// library, screenshot or artwork workers. This function performs filesystem
// I/O; it must not run under the LVGL/audio locks. The root is the SD mount,
// not the Music directory. PIN/private NVS and music files are never touched.
//
// Normal layout: /Aurora/Settings, /Aurora/Cache, /Aurora/Screenshots and
// /Aurora/Lockscreen. Existing artwork remains /Aurora/startup.jpg or .png.
// The legacy directory rename is fast and preserves complete A/B pairs.
// Conflicts use the actual store validators and sequence numbers; the older
// records are retained under /Aurora/Legacy, never overwritten or deleted.
// If an operation fails, the returned paths still select the newest readable
// records. A subsequent boot safely retries. Only known Aurora screenshots
// are moved from /Screenshots; other files/directories there are untouched.
StorageLayout prepare_storage_layout(const std::string &sd_root = "/sdcard",
                                     const StorageMigrationHooks *hooks = nullptr);
}
