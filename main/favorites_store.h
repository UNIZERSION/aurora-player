#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace player {
/* Path-based favourites live on the same SD as settings. No filesystem I/O
 * occurs in contains/set/toggle. Caller serializes this store's operations.
 * For responsiveness, batch save away from playback/scans and LVGL locks;
 * an independent artwork reader may continue under FatFs's filesystem locks.
 * Save explicitly before shutdown, before the global SD unmount barrier.
 * Missing tracks remain favourites; filtering/removal does not erase them. */
class FavoritesStore {
public:
    static constexpr size_t MAX_FAVORITES = 10000;
    FavoritesStore(std::string directory, std::string music_root);
    bool load(); // Select newest valid A/B; failed load preserves RAM contents.
    bool save(); // Durable A/B + CRC + readback. Failure retains dirty=true.
    bool contains(const std::string &path) const;
    bool set(const std::string &path, bool favorite); // false: invalid path/limit
    bool toggle(const std::string &path);
    size_t size() const { return paths_.size(); }
    bool dirty() const { return dirty_; }
    uint64_t sequence() const { return sequence_; }
    const std::vector<std::string> &paths() const { return paths_; }
private:
    bool valid_path(const std::string &path) const;
    bool read_slot(const std::string &path, std::vector<std::string> &out, uint64_t &sequence) const;
    std::string directory_, music_root_;
    std::vector<std::string> paths_;
    uint64_t sequence_ = 0;
    int active_ = -1;
    bool dirty_ = false;
};
}
