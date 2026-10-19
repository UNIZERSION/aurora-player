#pragma once
#include "screen_lock.h"

namespace player {
// Private internal-flash NVS (aurora_cfg), independent of the SD card and
// launcher's shared NVS. Existing shared-NVS credentials migrate on first boot;
// missing table entries can recover the reserved, verified region at 0xF10000.
// Commits are read back before use; unknown/overlapping regions fail closed.
// Initialization errors are reported, never repaired by erasing NVS.
class EspScreenLockStore final : public ScreenLockStore {
public:
    LockLoadResult load(ScreenLockConfig &out) override;
    bool save(const ScreenLockConfig &config) override;
};

class EspScreenLockCrypto final : public ScreenLockCrypto {
public:
    bool random_salt(std::array<uint8_t, 16> &out) override;
    bool sha256(const uint8_t *input, size_t size,
                std::array<uint8_t, 32> &out) override;
};
} // namespace player
