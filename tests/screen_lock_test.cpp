#ifdef NDEBUG
#undef NDEBUG
#endif
#include "screen_lock.h"
#include "screen_lock_storage.h"
#include "nvs.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using namespace player;

// Deterministic test crypto proves the controller hashes salt || exactly four
// digits and compares every digest byte; production hashing is mbedTLS SHA-256.
// These fixtures deliberately do not claim to validate SHA-256 itself.
struct Crypto : ScreenLockCrypto {
    unsigned random_calls = 0, hash_calls = 0;
    bool random_ok = true, hash_ok = true;
    bool random_salt(std::array<uint8_t, 16> &out) override {
        ++random_calls;
        out.fill(static_cast<uint8_t>(random_calls));
        return random_ok;
    }
    bool sha256(const uint8_t *input, size_t size, std::array<uint8_t, 32> &out) override {
        ++hash_calls;
        assert(size == 20);
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = static_cast<uint8_t>(input[i % size] ^ input[(i + 7) % size] ^ (i * 13));
        return hash_ok;
    }
};

struct Store : ScreenLockStore {
    ScreenLockConfig stored{};
    LockLoadResult result = LockLoadResult::Missing;
    bool save_ok = true;
    unsigned saves = 0;
    LockLoadResult load(ScreenLockConfig &out) override { out = stored; return result; }
    bool save(const ScreenLockConfig &in) override {
        ++saves;
        if (!save_ok) return false;
        stored = in;
        result = LockLoadResult::Loaded;
        return true;
    }
};

struct Fixture {
    Store store;
    Crypto crypto;
    ScreenLock lock{store, crypto};
    Fixture() { assert(lock.load() == LockLoadResult::Missing); }
};

static void pin_lifecycle() {
    Fixture f;
    assert(!f.lock.enabled() && !f.lock.locked());
    f.lock.lock();
    assert(!f.lock.locked());
    assert(f.lock.unlock("0000", 0) == LockResult::NotEnabled);
    for (auto pin : {"", "000", "00000", "12a4", "-123", "12 4", "12\n4"})
        assert(f.lock.set_pin(pin) == LockResult::InvalidPin);
    assert(f.store.saves == 0 && f.crypto.hash_calls == 0);
    assert(f.lock.set_pin("0000") == LockResult::Ok);
    assert(f.lock.enabled() && !f.lock.locked());
    assert(f.lock.set_pin("1234") == LockResult::AlreadyEnabled);
    f.lock.lock();
    assert(f.lock.locked());
    assert(f.lock.unlock("0001", 0) == LockResult::WrongPin && f.lock.locked());
    assert(f.lock.unlock("0000", 0) == LockResult::Ok && !f.lock.locked());
    // Already-unlocked verification is used to authenticate Settings changes.
    assert(f.lock.unlock("0001", 0) == LockResult::WrongPin);
    const auto old_salt = f.store.stored.salt;
    assert(f.lock.change_pin("0001", "1234", 0) == LockResult::WrongPin);
    assert(f.lock.change_pin("0000", "123", 0) == LockResult::InvalidPin);
    assert(f.lock.change_pin("0000", "1234", 0) == LockResult::Ok);
    assert(f.store.stored.salt != old_salt && f.crypto.random_calls == 2);
    assert(f.lock.unlock("0000", 0) == LockResult::WrongPin);
    assert(f.lock.disable("0000", 0) == LockResult::WrongPin);
    assert(f.lock.unlock("1234", 0) == LockResult::Ok);
    f.lock.lock();
    ScreenLock rebooted(f.store, f.crypto);
    assert(rebooted.locked() && rebooted.enabled() && rebooted.storage_error());
    assert(rebooted.load() == LockLoadResult::Loaded);
    assert(rebooted.enabled() && rebooted.locked());
    assert(rebooted.disable("1234", 0) == LockResult::Ok);
    assert(!rebooted.enabled() && !rebooted.locked());
    for (auto b : f.store.stored.salt) assert(b == 0);
    for (auto b : f.store.stored.digest) assert(b == 0);
    ScreenLock disabled_reboot(f.store, f.crypto);
    assert(disabled_reboot.load() == LockLoadResult::Loaded);
    assert(!disabled_reboot.enabled() && !disabled_reboot.locked());
}

static void cooldown_boundaries() {
    Fixture f;
    assert(f.lock.set_pin("4321") == LockResult::Ok);
    f.lock.lock();
    assert(f.lock.unlock("1234", 100) == LockResult::WrongPin);
    assert(f.lock.change_pin("1234", "0000", 200) == LockResult::WrongPin);
    assert(f.lock.disable("1234", 300) == LockResult::WrongPin);
    assert(f.lock.unlock("bad", 400) == LockResult::InvalidPin);
    assert(f.lock.unlock("9999", 500) == LockResult::Cooldown);
    assert(f.lock.retry_after_ms(500) == 30000);
    const auto hashes = f.crypto.hash_calls;
    assert(f.lock.unlock("4321", 1500) == LockResult::Cooldown);
    assert(f.lock.change_pin("4321", "0000", 1500) == LockResult::Cooldown);
    assert(f.lock.disable("4321", 1500) == LockResult::Cooldown);
    assert(f.crypto.hash_calls == hashes);
    assert(f.lock.retry_after_ms(1499) == 29000); // regressing clock is clamped
    assert(f.lock.retry_after_ms(30499) == 1);
    assert(f.lock.unlock("4321", 30500) == LockResult::Ok);
    f.lock.lock();
    for (unsigned i = 0; i < 4; ++i) assert(f.lock.unlock("0000", 30500) == LockResult::WrongPin);
    assert(f.lock.unlock("4321", 30500) == LockResult::Ok); // success resets budget
    for (unsigned i = 0; i < 4; ++i) assert(f.lock.unlock("0000", 30500) == LockResult::WrongPin);
    assert(f.lock.unlock("0000", 30500) == LockResult::Cooldown);
    f.lock.lock(); // blank/wake cycles must not reset the cooldown
    assert(f.lock.retry_after_ms(30500) == 30000);
    assert(f.lock.load() == LockLoadResult::Loaded);
    assert(f.lock.retry_after_ms(30500) == 30000); // reloading also preserves it

    Fixture long_uptime;
    assert(long_uptime.lock.set_pin("4321") == LockResult::Ok);
    const uint64_t t = uint64_t(std::numeric_limits<uint32_t>::max()) + 9000;
    for (unsigned i = 0; i < 4; ++i) assert(long_uptime.lock.unlock("1111", t) == LockResult::WrongPin);
    assert(long_uptime.lock.unlock("1111", t) == LockResult::Cooldown);
    assert(long_uptime.lock.retry_after_ms(t + 29999) == 1);
    assert(long_uptime.lock.retry_after_ms(t + 30000) == 0);
}

static void save_and_crypto_failures() {
    Fixture f;
    f.store.save_ok = false;
    assert(f.lock.set_pin("1234") == LockResult::StorageError);
    assert(!f.lock.enabled() && !f.lock.locked());
    f.store.save_ok = true;
    f.crypto.random_ok = false;
    assert(f.lock.set_pin("1234") == LockResult::CryptoError);
    assert(!f.lock.enabled());
    f.crypto.random_ok = true;
    f.crypto.hash_ok = false;
    assert(f.lock.set_pin("1234") == LockResult::CryptoError);
    assert(!f.lock.enabled());
    f.crypto.hash_ok = true;
    assert(f.lock.set_pin("1234") == LockResult::Ok);
    f.lock.lock();
    const auto original = f.store.stored;
    f.store.save_ok = false;
    assert(f.lock.change_pin("1234", "0000", 0) == LockResult::StorageError);
    assert(f.lock.locked() && f.lock.enabled() && f.store.stored.digest == original.digest);
    assert(f.lock.unlock("0000", 0) == LockResult::WrongPin);
    assert(f.lock.disable("1234", 0) == LockResult::StorageError);
    assert(f.lock.locked() && f.lock.enabled());
    f.crypto.hash_ok = false;
    assert(f.lock.unlock("1234", 0) == LockResult::CryptoError);
    assert(f.lock.locked());
    f.crypto.hash_ok = true;
    assert(f.lock.unlock("1234", 0) == LockResult::Ok);
    assert(f.lock.set_wallpaper("new.png") == LockResult::StorageError);
    assert(f.lock.wallpaper().empty());
    f.store.save_ok = true;
    assert(f.lock.set_wallpaper("new.png") == LockResult::Ok);
    assert(f.lock.wallpaper() == "new.png");
}

static void wallpaper_and_corruption() {
    for (auto s : {"", "wall.JPG", "wall.jpeg", "wall.PnG", "lock screen.png"}) assert(screen_lock_valid_wallpaper(s));
    for (auto s : {".png", "../x.png", "x..png", "/x.png", "sub/x.jpg", "sub\\x.jpg", "C:x.png",
                   "bad\n.png", "bad\t.png", "x.bmp", "x.png ", "x.png.", "x?.png", "x|.png"})
        assert(!screen_lock_valid_wallpaper(s));
    assert(screen_lock_valid_wallpaper(std::string(236, 'x') + ".png"));
    assert(!screen_lock_valid_wallpaper(std::string(237, 'x') + ".png"));
    const char nul_name[] = {'x', '\0', '.', 'p', 'n', 'g'};
    assert(!screen_lock_valid_wallpaper(std::string_view(nul_name, sizeof nul_name)));
    Fixture f;
    assert(f.lock.set_wallpaper("lock.png") == LockResult::Ok);
    assert(f.lock.set_pin("1234") == LockResult::Ok);
    f.lock.lock();
    assert(f.lock.set_wallpaper("other.png") == LockResult::Locked);
    assert(f.lock.unlock("1234", 0) == LockResult::Ok);
    assert(f.lock.set_wallpaper("../other.png") == LockResult::InvalidWallpaper);
    assert(f.lock.wallpaper() == "lock.png");
    assert(f.lock.set_wallpaper("") == LockResult::Ok);
    f.store.result = LockLoadResult::Error;
    assert(f.lock.load() == LockLoadResult::Error);
    assert(f.lock.enabled() && f.lock.locked() && f.lock.storage_error());
    assert(f.lock.unlock("1234", 0) == LockResult::StorageError);
    assert(f.lock.set_pin("0000") == LockResult::StorageError);
    assert(f.lock.disable("1234", 0) == LockResult::StorageError);
    assert(f.lock.set_wallpaper("") == LockResult::StorageError);
    f.store.result = LockLoadResult::Loaded;
    f.store.stored.wallpaper = "../outside.png";
    assert(f.lock.load() == LockLoadResult::Error && f.lock.locked());
}

static void every_digest_byte_is_checked() {
    Fixture f;
    assert(f.lock.set_pin("1234") == LockResult::Ok);
    const auto good = f.store.stored;
    for (size_t i = 0; i < good.digest.size(); ++i) {
        f.store.stored = good;
        f.store.stored.digest[i] ^= 0x01;
        ScreenLock reboot(f.store, f.crypto);
        assert(reboot.load() == LockLoadResult::Loaded);
        assert(reboot.unlock("1234", 0) == LockResult::WrongPin);
        assert(reboot.locked());
    }
}

// SDK adapter shim: exercise actual serialized bytes and every I/O error path.
static std::vector<uint8_t> nvs_bytes;
static bool nvs_exists = false;
static std::vector<uint8_t> private_bytes, staged_bytes[2];
static bool private_exists=false, partition_present=false, markers[2]{}, staged_marker[2]{}, staged[2]{};
static bool required_markers[2]{}, staged_required[2]{};
static bool corrupt_readback=false;
static int nvs_init_result = ESP_OK, nvs_open_result = ESP_OK, nvs_read_result = ESP_OK;
static int nvs_write_result = ESP_OK, nvs_commit_result = ESP_OK;
static int private_init_result=ESP_OK, private_write_result=ESP_OK;
static unsigned nvs_opens = 0, nvs_closes = 0;
static esp_flash_t test_chip;
esp_flash_t *esp_flash_default_chip = &test_chip;
static esp_partition_t test_partition;
static std::array<uint8_t, 0x6000> raw_private = [] { std::array<uint8_t, 0x6000> a; a.fill(0xff); return a; }();
static bool flash_read_fail = false, registration_fail = false;
static unsigned private_inits = 0, registrations = 0;
uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *data, uint32_t size) {
    crc = ~crc;
    while (size--) { crc ^= *data++; for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1))); }
    return ~crc;
}
static void put32(uint8_t *p, uint32_t value) { for (int i = 0; i < 4; ++i) p[i] = value >> (i * 8); }
static void stamp_private_namespace() {
    raw_private.fill(0xff);
    auto *page = raw_private.data(); put32(page, 0xfffffffe); put32(page + 4, 0); page[8] = 0xfe;
    put32(page + 28, esp_rom_crc32_le(0xffffffff, page + 4, 24)); page[32] = 0xfe;
    auto *entry = page + 64; entry[0] = 0; entry[1] = 1; entry[2] = 1;
    std::memset(entry + 8, 0, 16); std::memcpy(entry + 8, "aurora_lock", 11); entry[24] = 1;
    auto crc = esp_rom_crc32_le(0xffffffff, entry, 4); crc = esp_rom_crc32_le(crc, entry + 8, 24); put32(entry + 4, crc);
}
esp_err_t esp_flash_read(esp_flash_t *chip, void *out, uint32_t address, uint32_t size) {
    assert(!chip && address >= 0xF10000 && address + size <= 0xF16000);
    if (flash_read_fail) return ESP_FAIL;
    std::memcpy(out, raw_private.data() + address - 0xF10000, size); return ESP_OK;
}
esp_err_t esp_partition_register_external(esp_flash_t *chip, size_t address, size_t size, const char *name, int type, int subtype, const esp_partition_t **out) {
    assert(!chip && address == 0xF10000 && size == 0x6000 && !strcmp(name, "aurora_cfg") && type == ESP_PARTITION_TYPE_DATA && subtype == ESP_PARTITION_SUBTYPE_DATA_NVS);
    ++registrations; if (registration_fail) return ESP_FAIL;
    partition_present = true; *out = &test_partition; return ESP_OK;
}
esp_err_t nvs_flash_init() { return nvs_init_result; }
esp_err_t nvs_flash_init_partition(const char *name) {assert(!strcmp(name,"aurora_cfg"));++private_inits;return private_init_result;}
const esp_partition_t *esp_partition_find_first(int type,int subtype,const char *name) {
    assert(type==ESP_PARTITION_TYPE_ANY&&subtype==ESP_PARTITION_SUBTYPE_ANY&&!strcmp(name,"aurora_cfg"));
    return partition_present?&test_partition:nullptr;
}
esp_err_t nvs_open(const char *space, int mode, nvs_handle_t *handle) {
    assert(std::strcmp(space, "aurora_lock") == 0);
    if (nvs_open_result != ESP_OK) return nvs_open_result;
    if (!nvs_exists && !markers[0] && !required_markers[0] && mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    ++nvs_opens; *handle = 1; return ESP_OK;
}
esp_err_t nvs_open_from_partition(const char *partition,const char *space,int mode,nvs_handle_t *handle){
    assert(!strcmp(partition,"aurora_cfg")&&!strcmp(space,"aurora_lock"));
    if(nvs_open_result!=ESP_OK)return nvs_open_result;
    if(!private_exists&&!markers[1]&&!required_markers[1]&&mode==NVS_READONLY)return ESP_ERR_NVS_NOT_FOUND;
    ++nvs_opens;*handle=2;return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *size) {
    assert((h == 1||h==2) && std::strcmp(key, "config") == 0);
    if (nvs_read_result != ESP_OK) return nvs_read_result;
    if (!(h==1?nvs_exists:private_exists)) return ESP_ERR_NVS_NOT_FOUND;
    const auto &bytes=h==1?nvs_bytes:private_bytes;
    if (*size < bytes.size()) { *size = bytes.size(); return ESP_ERR_NVS_INVALID_LENGTH; }
    std::memcpy(out, bytes.data(), bytes.size()); *size = bytes.size();
    if(corrupt_readback&&*size)static_cast<uint8_t*>(out)[24]^=1;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *input, size_t size) {
    assert((h==1||h==2) && std::strcmp(key, "config") == 0);
    if((h==1?nvs_write_result:private_write_result)!=ESP_OK)return h==1?nvs_write_result:private_write_result;
    const auto bytes = static_cast<const uint8_t *>(input);
    staged_bytes[h-1].assign(bytes, bytes + size);staged[h-1]=true;return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t h,const char *key,uint8_t *out){
    assert((h==1||h==2)&&(!strcmp(key,"provisioned")||!strcmp(key,"private")));
    if(!(strcmp(key,"private")?markers[h-1]:required_markers[h-1]))return ESP_ERR_NVS_NOT_FOUND;
    *out=1;return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h,const char *key,uint8_t value){
    assert((h==1||h==2)&&(!strcmp(key,"provisioned")||!strcmp(key,"private"))&&value==1);
    if((h==1?nvs_write_result:private_write_result)!=ESP_OK)return h==1?nvs_write_result:private_write_result;
    (strcmp(key,"private")?staged_marker[h-1]:staged_required[h-1])=true;return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    assert(h==1||h==2);if(nvs_commit_result!=ESP_OK)return nvs_commit_result;
    if(staged[h-1]){(h==1?nvs_bytes:private_bytes)=staged_bytes[h-1];(h==1?nvs_exists:private_exists)=true;}
    if(staged_marker[h-1])markers[h-1]=true;
    if(staged_required[h-1])required_markers[h-1]=true;
    if (h == 2) stamp_private_namespace();
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { assert(h==1||h==2);staged[h-1]=staged_marker[h-1]=staged_required[h-1]=false;++nvs_closes; }
void esp_fill_random(void *out, size_t size) { std::memset(out, 0xA6, size); }
int mbedtls_sha256(const unsigned char *input, size_t size, unsigned char *out, int sha224) {
    assert(size == 20 && sha224 == 0);
    std::array<uint8_t, 32> digest{};
    Crypto crypto;
    assert(crypto.sha256(input, size, digest));
    std::copy(digest.begin(), digest.end(), out);
    return 0;
}

static void storage_adapter() {
    EspScreenLockStore store;
    EspScreenLockCrypto crypto;
    ScreenLock lock(store, crypto);
    assert(lock.load() == LockLoadResult::Missing);
    assert(lock.set_wallpaper("Aurora.JPG") == LockResult::Ok);
    assert(lock.set_pin("0000") == LockResult::Ok);
    assert(private_bytes.size() == 300);
    const uint8_t pin[] = {'0', '0', '0', '0'};
    assert(std::search(private_bytes.begin(), private_bytes.end(), std::begin(pin), std::end(pin)) == private_bytes.end());
    const auto good = private_bytes;
    ScreenLock reboot(store, crypto);
    assert(reboot.load() == LockLoadResult::Loaded && reboot.locked());
    assert(reboot.wallpaper() == "Aurora.JPG");
    assert(reboot.unlock("0000", 0) == LockResult::Ok);

    ScreenLockConfig sentinel;
    sentinel.wallpaper = "unchanged.png";
    for (size_t offset : {size_t(0), size_t(4), size_t(5), size_t(6), size_t(57), size_t(59), size_t(299)}) {
        private_bytes = good; private_bytes[offset] = 0xff;
        assert(store.load(sentinel) == LockLoadResult::Error);
        assert(sentinel.wallpaper == "unchanged.png");
    }
    private_bytes = good; private_bytes.pop_back();
    assert(store.load(sentinel) == LockLoadResult::Error);
    private_bytes = good; private_bytes.push_back(0);
    assert(store.load(sentinel) == LockLoadResult::Error);
    private_bytes = good; private_bytes[5] = 0; // disabled with leftover verifier is corrupt
    assert(store.load(sentinel) == LockLoadResult::Error);
    private_bytes = good; private_bytes[60] = '/';
    assert(store.load(sentinel) == LockLoadResult::Error);
    private_bytes = good;
    nvs_read_result = ESP_FAIL;
    assert(reboot.load() == LockLoadResult::Error && reboot.locked());
    nvs_read_result = ESP_ERR_NVS_NOT_FOUND;
    assert(store.load(sentinel) == LockLoadResult::Error); // provisioning marker forbids first-run reset
    nvs_read_result = ESP_OK;
    private_init_result = ESP_FAIL;
    assert(store.load(sentinel) == LockLoadResult::Error && !store.save(ScreenLockConfig{}));
    private_init_result = ESP_OK;
    nvs_open_result = ESP_FAIL;
    assert(store.load(sentinel) == LockLoadResult::Error && !store.save(ScreenLockConfig{}));
    nvs_open_result = ESP_OK;
    private_write_result = ESP_FAIL;
    assert(!store.save(ScreenLockConfig{}) && private_bytes == good);
    private_write_result = ESP_OK;
    nvs_commit_result = ESP_FAIL;
    assert(!store.save(ScreenLockConfig{}));
    assert(private_bytes == good); // power loss before commit keeps old credential
    nvs_commit_result = ESP_OK;
    corrupt_readback=true;assert(!store.save(ScreenLockConfig{}));corrupt_readback=false;
    assert(nvs_opens == nvs_closes);
    assert(store.save(ScreenLockConfig{}));
    assert(reboot.load() == LockLoadResult::Loaded && !reboot.enabled());
}

static void private_partition_persistence() {
    nvs_bytes.clear();private_bytes.clear();nvs_exists=private_exists=false;markers[0]=markers[1]=required_markers[0]=required_markers[1]=false;
    partition_present=false; raw_private.fill(0xff);
    EspScreenLockStore store;EspScreenLockCrypto crypto;
    ScreenLock old(store,crypto);assert(old.load()==LockLoadResult::Missing);
    assert(old.set_pin("4826")==LockResult::Ok);
    const auto legacy=private_bytes;
    nvs_bytes=legacy;nvs_exists=true;markers[0]=true;required_markers[0]=false;
    private_bytes.clear();private_exists=false;markers[1]=false;raw_private.fill(0xff);
    partition_present=true;
    ScreenLock upgraded(store,crypto);assert(upgraded.load()==LockLoadResult::Loaded&&upgraded.locked());
    assert(private_bytes==legacy&&upgraded.unlock("4826",0)==LockResult::Ok);
    assert(upgraded.change_pin("4826","1953",0)==LockResult::Ok);
    assert(private_bytes!=nvs_bytes&&required_markers[0]); // old PIN is guarded against fallback
    partition_present=false;
    {ScreenLock old_table(store,crypto);assert(old_table.load()==LockLoadResult::Loaded&&old_table.locked());assert(old_table.unlock("1953",0)==LockResult::Ok);}
    partition_present=true;
    // A cold boot loses every object/RAM setting. Shared launcher NVS may be
    // missing, but the independent partition still recovers the current PIN.
    nvs_bytes.clear();nvs_exists=false;markers[0]=required_markers[0]=false;partition_present=false;
    {EspScreenLockStore fresh;ScreenLock cold(fresh,crypto);assert(cold.load()==LockLoadResult::Loaded&&cold.locked());assert(cold.unlock("1953",0)==LockResult::Ok);}
    assert(required_markers[0]); // boot recreates missing table-loss guard
    // Once the guard exists no shared-NVS rewrite is required per PIN save.
    nvs_write_result=ESP_FAIL;
    assert(upgraded.change_pin("1953","6942",0)==LockResult::Ok);
    nvs_write_result=ESP_OK;
    {ScreenLock cold(store,crypto);assert(cold.load()==LockLoadResult::Loaded);assert(cold.unlock("6942",0)==LockResult::Ok);}
    assert(upgraded.set_wallpaper("Clouds.png")==LockResult::Ok);
    const auto before_guard_failure=private_bytes;
    required_markers[0]=false;nvs_write_result=ESP_FAIL;
    assert(upgraded.change_pin("6942","9999",0)==LockResult::StorageError);
    assert(private_bytes==before_guard_failure); // no new PIN until table-loss guard is durable
    nvs_write_result=ESP_OK;
    assert(upgraded.set_wallpaper("Clouds.png")==LockResult::Ok);
    partition_present=false;
    {ScreenLock recovered(store,crypto);assert(recovered.load()==LockLoadResult::Loaded&&recovered.locked());assert(recovered.unlock("6942",0)==LockResult::Ok);}
    partition_present=true;
    const auto good=private_bytes;
    private_bytes[0]^=1;
    {ScreenLock corrupt(store,crypto);assert(corrupt.load()==LockLoadResult::Error&&corrupt.locked());}
    private_bytes=good;private_init_result=ESP_FAIL;
    {ScreenLock fail(store,crypto);assert(fail.load()==LockLoadResult::Error&&fail.locked());}
    private_init_result=ESP_OK;
    // A missing blob after provisioning is an error, never an unlocked setup.
    private_exists=false;
    {ScreenLock fail(store,crypto);assert(fail.load()==LockLoadResult::Error&&fail.locked());}
    markers[1]=false;private_write_result=ESP_FAIL;
    {ScreenLock fail(store,crypto);assert(fail.load()==LockLoadResult::Error&&fail.locked());}
    private_write_result=ESP_OK;
    // Missing private data after migration cannot revive stale legacy data.
    {ScreenLock fail(store,crypto);assert(fail.load()==LockLoadResult::Error&&fail.locked());}
    // Genuine pre-migration legacy installations still migrate; a failed
    // primary write fails closed and may be retried without losing legacy PIN.
    required_markers[0]=false;nvs_bytes=legacy;nvs_exists=true;markers[0]=true;private_write_result=ESP_FAIL;raw_private.fill(0xff);
    {ScreenLock fail(store,crypto);assert(fail.load()==LockLoadResult::Error&&fail.locked());}
    private_write_result=ESP_OK;
    {ScreenLock migrated(store,crypto);assert(migrated.load()==LockLoadResult::Loaded);assert(migrated.unlock("4826",0)==LockResult::Ok);}
    assert(nvs_opens==nvs_closes);
    // Fresh private-layout installation, failed commit, then cold startup.
    nvs_exists=private_exists=false;nvs_bytes.clear();private_bytes.clear();
    markers[0]=markers[1]=required_markers[0]=required_markers[1]=false;raw_private.fill(0xff);
    ScreenLock fresh(store,crypto);assert(fresh.load()==LockLoadResult::Missing);
    nvs_commit_result=ESP_FAIL;assert(fresh.set_pin("7001")==LockResult::StorageError);
    assert(!fresh.enabled()&&private_exists); // durable disabled first-run record
    nvs_commit_result=ESP_OK;assert(fresh.set_pin("7001")==LockResult::Ok);
    {ScreenLock cold(store,crypto);assert(cold.load()==LockLoadResult::Loaded&&cold.locked());assert(cold.unlock("7001",0)==LockResult::Ok);}
    partition_present=false;
}

static void partition_table_loss_security() {
    raw_private.fill(0xff); partition_present=false;
    nvs_exists=private_exists=false;nvs_bytes.clear();private_bytes.clear();
    markers[0]=markers[1]=required_markers[0]=required_markers[1]=false;
    EspScreenLockStore store;EspScreenLockCrypto crypto;
    ScreenLock fresh(store,crypto);assert(fresh.load()==LockLoadResult::Missing);
    // Even a first-run reboot before selecting PIN has an owned disabled record.
    partition_present=false;nvs_exists=false;markers[0]=required_markers[0]=false;
    {ScreenLock disabled(store,crypto);assert(disabled.load()==LockLoadResult::Loaded&&!disabled.enabled());}
    assert(fresh.set_pin("9057")==LockResult::Ok);
    const auto good_raw=raw_private;const auto good_record=private_bytes;
    // Restore through six missing-table reboots, including lost shared NVS.
    for(int i=0;i<6;++i){
        partition_present=false;nvs_exists=false;nvs_bytes.clear();markers[0]=required_markers[0]=false;
        EspScreenLockStore restarted;ScreenLock cold(restarted,crypto);
        assert(cold.load()==LockLoadResult::Loaded&&cold.locked());
        assert(cold.unlock("9057",0)==LockResult::Ok);
        assert(private_bytes==good_record);
    }
    auto reject_without_init = [&] {
        partition_present=false;const auto before=private_inits;const auto attempts=registrations;
        ScreenLock rejected(store,crypto);assert(rejected.load()==LockLoadResult::Error&&rejected.locked());
        assert(!store.save(ScreenLockConfig{}));
        assert(private_inits==before&&registrations==attempts&&private_bytes==good_record);
    };
    for(size_t offset : {size_t(4),size_t(8),size_t(28),size_t(64+4),size_t(64+8),size_t(64+24)}){
        raw_private=good_raw;raw_private[offset]^=1;reject_without_init();
    }
    raw_private=good_raw;raw_private[32]=0xfc;reject_without_init(); // erased namespace item
    raw_private.fill(0xa5);reject_without_init(); // arbitrary preexisting flash is never initialized
    raw_private=good_raw;flash_read_fail=true;reject_without_init();flash_read_fail=false;
    raw_private=good_raw;partition_present=false;registration_fail=true;
    const auto before=private_inits;
    {ScreenLock collision(store,crypto);assert(collision.load()==LockLoadResult::Error&&collision.locked());}
    assert(private_inits==before);registration_fail=false; // SDK overlap/bounds refusal
    for(int field=0;field<6;++field){
        partition_present=true;test_partition=esp_partition_t{};
        if(field==0)test_partition.address+=4096;
        if(field==1)test_partition.size-=4096;
        if(field==2)test_partition.type=0;
        if(field==3)test_partition.subtype=0;
        if(field==4)test_partition.encrypted=true;
        if(field==5)test_partition.readonly=true;
        const auto count=private_inits;
        {ScreenLock wrong(store,crypto);assert(wrong.load()==LockLoadResult::Error&&wrong.locked());}
        assert(private_inits==count);
    }
    test_partition=esp_partition_t{};
    // Ownership cannot authenticate a missing/corrupt credential or fall back
    // to a stale valid legacy credential, even with both provisioning flags gone.
    partition_present=false;nvs_bytes=good_record;nvs_exists=true;required_markers[0]=false;
    private_exists=false;markers[1]=false;
    {ScreenLock missing(store,crypto);assert(missing.load()==LockLoadResult::Error&&missing.locked());}
    private_exists=true;private_bytes=good_record;private_bytes[0]^=1;
    {ScreenLock corrupt(store,crypto);assert(corrupt.load()==LockLoadResult::Error&&corrupt.locked());}
    private_bytes=good_record;markers[1]=true;
    raw_private.fill(0xff);private_exists=false;markers[1]=false;required_markers[0]=true;
    {ScreenLock erased(store,crypto);assert(erased.load()==LockLoadResult::Error&&erased.locked());}
    assert(nvs_opens==nvs_closes);
}

int main() {
    pin_lifecycle();
    cooldown_boundaries();
    save_and_crypto_failures();
    wallpaper_and_corruption();
    every_digest_byte_is_checked();
    storage_adapter();
    private_partition_persistence();
    partition_table_loss_security();
    std::puts("Screen lock tests passed: PIN lifecycle, private NVS migration/cold persistence, missing partition-table entry plus shared NVS loss, CRC ownership probe, arbitrary flash/overlap refusal, commit/readback failures, stale PIN fallback prevention, fail-closed reads, shared cooldown, wallpaper validation, NVS/crypto adapter.");
    return 0;
}
