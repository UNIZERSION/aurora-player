#include "screen_lock.h"
#include <algorithm>
#include <cstring>

namespace player {
namespace {
void wipe(void *memory, size_t size) {
    volatile uint8_t *bytes = static_cast<volatile uint8_t *>(memory);
    while (size--) *bytes++ = 0;
}

bool same_digest(const std::array<uint8_t, 32> &a, const std::array<uint8_t, 32> &b) {
    // No early return based on the first matching/mismatching digest byte.
    volatile uint8_t difference = 0;
    for (size_t i = 0; i < a.size(); ++i) difference |= a[i] ^ b[i];
    return difference == 0;
}

bool suffix_is(std::string_view name, std::string_view extension) {
    if (name.size() <= extension.size()) return false;
    name.remove_prefix(name.size() - extension.size());
    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != extension[i]) return false;
    }
    return true;
}
}

bool screen_lock_valid_pin(std::string_view pin) {
    if (pin.size() != 4) return false;
    for (const char c : pin) if (c < '0' || c > '9') return false;
    return true;
}

bool screen_lock_valid_wallpaper(std::string_view name) {
    if (name.empty()) return true;
    if (name.size() > SCREEN_LOCK_WALLPAPER_MAX || name.front() == '.' ||
        name.back() == '.' || name.back() == ' ' || name.find("..") != name.npos) return false;
    for (const unsigned char c : name) {
        if (c < 32 || c == 127 || c == '/' || c == '\\' || c == ':' ||
            c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
    }
    return suffix_is(name, ".jpg") || suffix_is(name, ".jpeg") || suffix_is(name, ".png");
}

bool screen_lock_valid_config(const ScreenLockConfig &config) {
    if (!screen_lock_valid_wallpaper(config.wallpaper)) return false;
    if (!config.enabled) {
        for (const auto byte : config.salt) if (byte) return false;
        for (const auto byte : config.digest) if (byte) return false;
    }
    return true;
}

ScreenLock::ScreenLock(ScreenLockStore &store, ScreenLockCrypto &crypto)
    : store_(store), crypto_(crypto) {}

LockLoadResult ScreenLock::load() {
    ScreenLockConfig loaded;
    const LockLoadResult result = store_.load(loaded);
    if (result == LockLoadResult::Error ||
        (result == LockLoadResult::Loaded && !screen_lock_valid_config(loaded))) {
        storage_error_ = true;
        locked_ = true;
        return LockLoadResult::Error;
    }
    config_ = result == LockLoadResult::Missing ? ScreenLockConfig{} : loaded;
    storage_error_ = false;
    locked_ = config_.enabled; // configured devices always start locked
    // Keep a running instance's budget if a caller reloads after a storage
    // retry. Only a new controller (a new boot) begins with no attempts.
    return result;
}

void ScreenLock::clear_attempts() {
    bad_attempts_ = 0;
    cooldown_ = false;
    cooldown_start_ms_ = 0;
}

uint64_t ScreenLock::observe_time(uint64_t now_ms) {
    last_now_ms_ = std::max(last_now_ms_, now_ms);
    return last_now_ms_;
}

uint32_t ScreenLock::retry_after_ms(uint64_t now_ms) {
    now_ms = observe_time(now_ms);
    if (!cooldown_) return 0;
    const uint64_t elapsed = now_ms - cooldown_start_ms_;
    if (elapsed >= SCREEN_LOCK_COOLDOWN_MS) {
        clear_attempts();
        return 0;
    }
    return SCREEN_LOCK_COOLDOWN_MS - static_cast<uint32_t>(elapsed);
}

bool ScreenLock::hash_pin(std::string_view pin, const std::array<uint8_t, 16> &salt,
                          std::array<uint8_t, 32> &digest) {
    uint8_t input[20];
    std::copy(salt.begin(), salt.end(), input);
    std::memcpy(input + salt.size(), pin.data(), 4);
    const bool ok = crypto_.sha256(input, sizeof input, digest);
    wipe(input, sizeof input);
    return ok;
}

LockResult ScreenLock::verify(std::string_view pin, uint64_t now_ms) {
    if (storage_error_) return LockResult::StorageError;
    if (!config_.enabled) return LockResult::NotEnabled;
    if (retry_after_ms(now_ms)) return LockResult::Cooldown;
    const bool valid = screen_lock_valid_pin(pin);
    std::array<uint8_t, 32> digest{};
    if (valid && !hash_pin(pin, config_.salt, digest)) {
        wipe(digest.data(), digest.size());
        return LockResult::CryptoError;
    }
    const bool matches = valid && same_digest(digest, config_.digest);
    wipe(digest.data(), digest.size());
    if (matches) {
        clear_attempts();
        return LockResult::Ok;
    }
    if (++bad_attempts_ >= SCREEN_LOCK_MAX_ATTEMPTS) {
        cooldown_ = true;
        cooldown_start_ms_ = last_now_ms_;
        return LockResult::Cooldown;
    }
    return valid ? LockResult::WrongPin : LockResult::InvalidPin;
}

LockResult ScreenLock::replace_pin(std::string_view pin) {
    ScreenLockConfig candidate = config_;
    candidate.enabled = true;
    if (!crypto_.random_salt(candidate.salt) || !hash_pin(pin, candidate.salt, candidate.digest))
        return LockResult::CryptoError;
    if (!store_.save(candidate)) return LockResult::StorageError;
    config_ = candidate;
    clear_attempts();
    return LockResult::Ok;
}

LockResult ScreenLock::set_pin(std::string_view pin) {
    if (storage_error_) return LockResult::StorageError;
    if (config_.enabled) return LockResult::AlreadyEnabled;
    if (!screen_lock_valid_pin(pin)) return LockResult::InvalidPin;
    return replace_pin(pin);
}

LockResult ScreenLock::change_pin(std::string_view current, std::string_view replacement, uint64_t now_ms) {
    // Authenticate even malformed replacements, so this path cannot sidestep
    // the shared attempt counter when checking the current credential.
    const LockResult result = verify(current, now_ms);
    if (result != LockResult::Ok) return result;
    if (!screen_lock_valid_pin(replacement)) return LockResult::InvalidPin;
    return replace_pin(replacement);
}

LockResult ScreenLock::disable(std::string_view current, uint64_t now_ms) {
    const LockResult result = verify(current, now_ms);
    if (result != LockResult::Ok) return result;
    ScreenLockConfig candidate = config_;
    candidate.enabled = false;
    candidate.salt.fill(0);
    candidate.digest.fill(0);
    if (!store_.save(candidate)) return LockResult::StorageError;
    config_ = candidate;
    locked_ = false;
    clear_attempts();
    return LockResult::Ok;
}

LockResult ScreenLock::unlock(std::string_view pin, uint64_t now_ms) {
    const LockResult result = verify(pin, now_ms);
    if (result == LockResult::Ok) locked_ = false;
    return result;
}

void ScreenLock::lock() {
    if (enabled()) locked_ = true;
}

LockResult ScreenLock::set_wallpaper(std::string_view basename) {
    if (storage_error_) return LockResult::StorageError;
    if (locked_) return LockResult::Locked;
    if (!screen_lock_valid_wallpaper(basename)) return LockResult::InvalidWallpaper;
    ScreenLockConfig candidate = config_;
    candidate.wallpaper.assign(basename.empty() ? "" : basename.data(), basename.size());
    if (!store_.save(candidate)) return LockResult::StorageError;
    config_ = candidate;
    return LockResult::Ok;
}

} // namespace player
