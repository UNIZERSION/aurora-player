#include "screen_lock_storage.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_flash.h"
#include "esp_rom_crc.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

namespace player {
namespace {
constexpr char NVS_NAMESPACE[] = "aurora_lock";
constexpr char NVS_CONFIG_KEY[] = "config";
constexpr char LOCK_PARTITION[] = "aurora_cfg";
constexpr char PROVISIONED_KEY[] = "provisioned";
constexpr char PRIVATE_REQUIRED_KEY[] = "private";
constexpr char TAG[] = "screen_lock";
constexpr uint32_t LOCK_ADDRESS = 0xF10000;
constexpr uint32_t LOCK_SIZE = 0x6000;
static_assert(SCREEN_LOCK_WALLPAPER_MAX <= 255, "Wallpaper length must fit the version 1 record");
constexpr size_t RECORD_SIZE = 60 + SCREEN_LOCK_WALLPAPER_MAX;
using Record = std::array<uint8_t, RECORD_SIZE>;

bool encode(const ScreenLockConfig &config, Record &record) {
    if (!screen_lock_valid_config(config)) return false;
    record.fill(0);
    std::memcpy(record.data(), "ALK1", 4);
    record[4] = 1; // format version
    record[5] = config.enabled ? 1 : 0;
    std::copy(config.salt.begin(), config.salt.end(), record.begin() + 8);
    std::copy(config.digest.begin(), config.digest.end(), record.begin() + 24);
    record[56] = static_cast<uint8_t>(config.wallpaper.size());
    std::copy(config.wallpaper.begin(), config.wallpaper.end(), record.begin() + 60);
    return true;
}

bool decode(const Record &record, ScreenLockConfig &out) {
    if (std::memcmp(record.data(), "ALK1", 4) || record[4] != 1 || record[5] > 1 ||
        record[6] || record[7] || record[56] > SCREEN_LOCK_WALLPAPER_MAX ||
        record[57] || record[58] || record[59]) return false;
    const size_t length = record[56];
    for (size_t i = 60 + length; i < record.size(); ++i) if (record[i]) return false;
    ScreenLockConfig config;
    config.enabled = record[5] != 0;
    std::copy_n(record.begin() + 8, config.salt.size(), config.salt.begin());
    std::copy_n(record.begin() + 24, config.digest.size(), config.digest.begin());
    config.wallpaper.assign(reinterpret_cast<const char *>(record.data() + 60), length);
    if (!screen_lock_valid_config(config)) return false;
    out = config;
    return true;
}

uint32_t read_le32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// This is an ownership probe, NOT a second credential reader. It only accepts
// erased storage or an ESP-IDF v5 NVS namespace with valid page/item CRCs. The
// NVS library and ALK1 decoder still select and validate the actual credential.
bool probe_private_region(bool &owned) {
    owned = false;
    auto page = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[4096]);
    if (!page) return false;
    bool erased = true;
    for (uint32_t offset = 0; offset < LOCK_SIZE; offset += 4096) {
        if (esp_flash_read(nullptr, page.get(), LOCK_ADDRESS + offset, 4096) != ESP_OK) return false;
        for (size_t i = 0; i < 4096; ++i) if (page[i] != 0xff) { erased = false; break; }
        const auto state = read_le32(page.get());
        if ((state != 0xfffffffe && state != 0xfffffffc && state != 0xfffffff8) || page[8] != 0xfe ||
            esp_rom_crc32_le(0xffffffff, page.get() + 4, 24) != read_le32(page.get() + 28)) continue;
        for (size_t i = 0; i < 126; ++i) {
            if (((page[32 + i / 4] >> ((i % 4) * 2)) & 3) != 2) continue;
            const auto *entry = page.get() + 64 + i * 32;
            if (entry[0] != 0 || entry[1] != 1 || entry[2] != 1 || entry[3] != 0xff ||
                !entry[24] || entry[24] == 0xff || std::memcmp(entry + 8, NVS_NAMESPACE, sizeof(NVS_NAMESPACE))) continue;
            auto crc = esp_rom_crc32_le(0xffffffff, entry, 4);
            crc = esp_rom_crc32_le(crc, entry + 8, 24);
            if (crc == read_le32(entry + 4)) owned = true;
        }
    }
    return erased || owned;
}

bool ensure_private_partition(bool &owned) {
    const auto *partition = esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, LOCK_PARTITION);
    if (partition && (partition->flash_chip != esp_flash_default_chip || partition->address != LOCK_ADDRESS ||
        partition->size != LOCK_SIZE || partition->type != ESP_PARTITION_TYPE_DATA ||
        partition->subtype != ESP_PARTITION_SUBTYPE_DATA_NVS || partition->encrypted || partition->readonly)) return false;
    if (!probe_private_region(owned)) return false;
    if (partition) return true;
    // Registration changes only the in-memory partition list. ESP-IDF rejects
    // overlap with ANY current partition and rejects regions outside the chip.
    if (esp_partition_register_external(nullptr, LOCK_ADDRESS, LOCK_SIZE, LOCK_PARTITION,
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, &partition) != ESP_OK) return false;
    ESP_LOGI(TAG, "PIN storage: reserved private NVS registered after missing table entry");
    return true;
}

esp_err_t initialize(const char *partition) {
    // Never erase either partition on an initialization error.
    return partition ? nvs_flash_init_partition(partition) : nvs_flash_init();
}

esp_err_t open_store(const char *partition, nvs_open_mode_t mode, nvs_handle_t *handle) {
    return partition ? nvs_open_from_partition(partition, NVS_NAMESPACE, mode, handle)
                     : nvs_open(NVS_NAMESPACE, mode, handle);
}

LockLoadResult read_store(const char *partition, ScreenLockConfig &out, bool *private_required = nullptr) {
    if (private_required) *private_required = false;
    if (initialize(partition) != ESP_OK) return LockLoadResult::Error;
    nvs_handle_t handle;
    const esp_err_t opened = open_store(partition, NVS_READONLY, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND) return LockLoadResult::Missing;
    if (opened != ESP_OK) return LockLoadResult::Error;
    Record record{};
    size_t size = record.size();
    const esp_err_t read = nvs_get_blob(handle, NVS_CONFIG_KEY, record.data(), &size);
    uint8_t provisioned = 0;
    const esp_err_t marker = nvs_get_u8(handle, PROVISIONED_KEY, &provisioned);
    uint8_t required = 0;
    const esp_err_t private_marker = nvs_get_u8(handle, PRIVATE_REQUIRED_KEY, &required);
    nvs_close(handle);
    if (private_required) *private_required = private_marker == ESP_OK && required == 1;
    if (private_marker != ESP_ERR_NVS_NOT_FOUND && (private_marker != ESP_OK || required != 1)) return LockLoadResult::Error;
    // A known installation with a missing credential must never become an
    // unlocked first-run setup screen. The marker remains after disabling PIN.
    if (read == ESP_ERR_NVS_NOT_FOUND)
        return marker == ESP_ERR_NVS_NOT_FOUND && private_marker == ESP_ERR_NVS_NOT_FOUND ? LockLoadResult::Missing : LockLoadResult::Error;
    if (read != ESP_OK || size != record.size() || !decode(record, out)) return LockLoadResult::Error;
    return LockLoadResult::Loaded;
}

bool write_store(const char *partition, const ScreenLockConfig &config) {
    Record record{};
    if (!encode(config, record) || initialize(partition) != ESP_OK) return false;
    nvs_handle_t handle;
    if (open_store(partition, NVS_READWRITE, &handle) != ESP_OK) return false;
    // NVS provides a CRC-protected atomic blob update. Commit before publishing
    // the candidate in RAM. A power interruption can recover old or new config.
    bool ok = nvs_set_blob(handle, NVS_CONFIG_KEY, record.data(), record.size()) == ESP_OK;
    if (ok) ok = nvs_set_u8(handle, PROVISIONED_KEY, 1) == ESP_OK;
    if (ok) ok = nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    if (!ok) return false;
    // Re-open the committed namespace before acknowledging a PIN change.
    ScreenLockConfig verified;
    Record readback{};
    return read_store(partition, verified) == LockLoadResult::Loaded &&
           encode(verified, readback) && record == readback;
}

bool require_private_partition() {
    if (initialize(nullptr) != ESP_OK) return false;
    nvs_handle_t handle;
    if (open_store(nullptr, NVS_READWRITE, &handle) != ESP_OK) return false;
    uint8_t marker = 0;
    const auto previous = nvs_get_u8(handle, PRIVATE_REQUIRED_KEY, &marker);
    if (previous == ESP_OK) { nvs_close(handle); return marker == 1; }
    if (previous != ESP_ERR_NVS_NOT_FOUND) { nvs_close(handle); return false; }
    bool ok = nvs_set_u8(handle, PRIVATE_REQUIRED_KEY, 1) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    if (!ok || open_store(nullptr, NVS_READONLY, &handle) != ESP_OK) return false;
    ok = nvs_get_u8(handle, PRIVATE_REQUIRED_KEY, &marker) == ESP_OK && marker == 1;
    nvs_close(handle);
    return ok;
}
}

LockLoadResult EspScreenLockStore::load(ScreenLockConfig &out) {
    bool owned = false;
    if (!ensure_private_partition(owned)) return LockLoadResult::Error;
    const auto result = read_store(LOCK_PARTITION, out);
    if (result != LockLoadResult::Missing) {
        if (result == LockLoadResult::Loaded && !require_private_partition()) return LockLoadResult::Error;
        ESP_LOGI(TAG, "PIN storage: private NVS, result=%u", unsigned(result));
        return result;
    }
    if (owned) return LockLoadResult::Error; // never reset an existing private installation
    ScreenLockConfig legacy;
    bool private_required = false;
    const auto legacy_result = read_store(nullptr, legacy, &private_required);
    if (private_required) return LockLoadResult::Error;
    if (legacy_result == LockLoadResult::Error) return legacy_result;
    // Persist the disabled first-run record too, so a reboot before PIN setup
    // still leaves verifiable ownership if a launcher removes the table entry.
    if (legacy_result == LockLoadResult::Missing) {
        if (!write_store(LOCK_PARTITION, ScreenLockConfig{}) || !require_private_partition()) return LockLoadResult::Error;
        return LockLoadResult::Missing;
    }
    // Upgrade from the verified legacy record. A failed migration fails
    // closed; it must not discard an existing PIN or ask to create a new one.
    if (!write_store(LOCK_PARTITION, legacy)) return LockLoadResult::Error;
    if (!require_private_partition()) return LockLoadResult::Error;
    out = legacy;
    ESP_LOGI(TAG, "PIN storage: existing settings migrated to private NVS");
    return LockLoadResult::Loaded;
}

bool EspScreenLockStore::save(const ScreenLockConfig &config) {
    bool owned = false;
    if (!ensure_private_partition(owned)) return false;
    // Before changing the authoritative PIN, persist a guard in the old
    // namespace. An old partition table must never resurrect an older PIN.
    if (!require_private_partition()) return false;
    return write_store(LOCK_PARTITION, config);
}

bool EspScreenLockCrypto::random_salt(std::array<uint8_t, 16> &out) {
    esp_fill_random(out.data(), out.size());
    return true;
}

bool EspScreenLockCrypto::sha256(const uint8_t *input, size_t size,
                                std::array<uint8_t, 32> &out) {
    return mbedtls_sha256(input, size, out.data(), 0) == 0;
}
} // namespace player
