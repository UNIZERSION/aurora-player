#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace player {

constexpr char SCREEN_LOCK_WALLPAPER_DIR[] = "/sdcard/Aurora/Lockscreen";
constexpr size_t SCREEN_LOCK_WALLPAPER_MAX = 240;
constexpr uint32_t SCREEN_LOCK_COOLDOWN_MS = 30000;
constexpr unsigned SCREEN_LOCK_MAX_ATTEMPTS = 5;

// The record contains a salted verifier, never the PIN itself. A missing record
// is a new, disabled installation; a damaged/unreadable record fails closed.
struct ScreenLockConfig {
    bool enabled = false;
    std::array<uint8_t, 16> salt{};
    std::array<uint8_t, 32> digest{};
    std::string wallpaper; // basename, or empty for the frosted player backdrop
};

enum class LockLoadResult { Missing, Loaded, Error };
enum class LockResult {
    Ok, InvalidPin, WrongPin, Cooldown, AlreadyEnabled, NotEnabled,
    StorageError, CryptoError, Locked, InvalidWallpaper
};

class ScreenLockStore {
public:
    virtual ~ScreenLockStore() = default;
    virtual LockLoadResult load(ScreenLockConfig &out) = 0;
    // An unsuccessful save never changes the controller's active config.
    // Persistent storage must recover a complete old or new record on power loss.
    virtual bool save(const ScreenLockConfig &config) = 0;
};

class ScreenLockCrypto {
public:
    virtual ~ScreenLockCrypto() = default;
    virtual bool random_salt(std::array<uint8_t, 16> &out) = 0;
    virtual bool sha256(const uint8_t *input, size_t size,
                        std::array<uint8_t, 32> &out) = 0;
};

bool screen_lock_valid_pin(std::string_view pin);
bool screen_lock_valid_wallpaper(std::string_view basename);
bool screen_lock_valid_config(const ScreenLockConfig &config);

// Caller serializes access. All time values are monotonic milliseconds from
// the same boot (esp_timer_get_time()/1000). Clock regressions never shorten a
// cooldown. Credential operations share the same attempt budget.
class ScreenLock {
public:
    ScreenLock(ScreenLockStore &store, ScreenLockCrypto &crypto);
    LockLoadResult load();
    bool enabled() const { return config_.enabled || storage_error_; }
    bool locked() const { return locked_; }
    bool storage_error() const { return storage_error_; }
    const std::string &wallpaper() const { return config_.wallpaper; }

    // The UI confirms a newly entered PIN before invoking set/change.
    LockResult set_pin(std::string_view pin);
    LockResult change_pin(std::string_view current, std::string_view replacement, uint64_t now_ms);
    LockResult disable(std::string_view current, uint64_t now_ms);
    LockResult unlock(std::string_view pin, uint64_t now_ms);
    void lock();
    uint32_t retry_after_ms(uint64_t now_ms);
    LockResult set_wallpaper(std::string_view basename);

private:
    LockResult verify(std::string_view pin, uint64_t now_ms);
    bool hash_pin(std::string_view pin, const std::array<uint8_t, 16> &salt,
                  std::array<uint8_t, 32> &digest);
    LockResult replace_pin(std::string_view pin);
    void clear_attempts();
    uint64_t observe_time(uint64_t now_ms);
    ScreenLockStore &store_;
    ScreenLockCrypto &crypto_;
    ScreenLockConfig config_;
    bool locked_ = true;
    bool storage_error_ = true; // load() is required before use
    unsigned bad_attempts_ = 0;
    uint64_t last_now_ms_ = 0;
    uint64_t cooldown_start_ms_ = 0;
    bool cooldown_ = false;
};

} // namespace player
