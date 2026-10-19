#include "tab5_hal.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "bsp/m5stack_tab5.h"
#include "diskio_sdmmc.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ff.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

_Static_assert(FF_FS_EXFAT == 1, "The entire FatFS component must enable exFAT");
_Static_assert(FF_USE_LFN != 0, "exFAT requires long filenames");
_Static_assert(sizeof(FSIZE_t) == 8, "exFAT file sizes must be 64-bit");

static const char *TAG = "tab5_hal";
static sdmmc_card_t *card;
static sd_pwr_ctrl_handle_t sd_power;
static esp_codec_dev_handle_t speaker;
static uint32_t speaker_rate;
static uint8_t speaker_volume = 55;
static bool speaker_muted;
static i2c_master_dev_handle_t rtc, battery;
static bool battery_ready;
static bool display_touch_ready;
static char display_error[128];
esp_err_t tab5_display_atomic_start(lv_display_t **display, bool *touch_ready);

bool tab5_touch_ready(void) { return display_touch_ready; }
const char *tab5_display_last_error(void) { return display_error; }

lv_display_t *tab5_display_init(uint8_t initial_brightness)
{
    const int64_t started = esp_timer_get_time();
    display_touch_ready = false;
    display_error[0] = '\0';
    ESP_LOGI(TAG, "BOOT 02 display start, free internal=%u, PSRAM=%u, largest PSRAM=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    lv_display_t *display = NULL;
    esp_err_t display_init = tab5_display_atomic_start(&display, &display_touch_ready);
    /* Preserve a working LCD even if optional touch initialization fails. */
    if (display_init != ESP_OK) {
        snprintf(display_error, sizeof(display_error), "%s (%s)",
                 display ? "Touch initialization failed; LCD is available" :
                           "LCD/LVGL initialization failed; inspect USB boot log",
                 esp_err_to_name(display_init));
        ESP_LOGE(TAG, "BOOT 03 %s", display_error);
    } else {
        ESP_LOGI(TAG, "BOOT 03 LCD and touch initialized");
    }
    const esp_err_t backlight = bsp_display_brightness_set(initial_brightness > 100 ? 100 : initial_brightness);
    if (backlight != ESP_OK) {
        ESP_LOGE(TAG, "BOOT 04 PWM backlight failed: %s",
                 esp_err_to_name(backlight));
        if (!display_error[0]) snprintf(display_error, sizeof(display_error),
                                       "Backlight PWM failed: %s", esp_err_to_name(backlight));
    } else {
        ESP_LOGI(TAG, "BOOT 04 backlight %s", initial_brightness ? "on" : "held off for first complete frame");
    }
    if (!display) return NULL;
    if (bsp_display_lock(1000)) {
        bsp_display_rotate(display, LV_DISPLAY_ROTATION_90);
        bsp_display_unlock();
    } else {
        snprintf(display_error, sizeof(display_error), "LVGL lock timed out during rotation");
        ESP_LOGE(TAG, "BOOT 05 %s", display_error);
        return NULL;
    }
    ESP_LOGI(TAG, "BOOT 05 landscape ready, touch=%s, init_ms=%lu",
             display_touch_ready ? "yes" : "no",
             (unsigned long)((esp_timer_get_time() - started) / 1000));
    return display;
}

bool tab5_display_lock(uint32_t ms) { return bsp_display_lock(ms); }
void tab5_display_unlock(void) { bsp_display_unlock(); }
esp_err_t tab5_brightness_set(uint8_t v) { return bsp_display_brightness_set(v > 100 ? 100 : v); }

esp_err_t tab5_storage_mount(void)
{
    if (card) return ESP_OK;
    const int64_t started = esp_timer_get_time();
    ESP_LOGI(TAG, "SD mount start (20 MHz, no formatting)");
    if (!sd_power) {
        const sd_pwr_ctrl_ldo_config_t power_cfg = {.ldo_chan_id = 4};
        ESP_RETURN_ON_ERROR(sd_pwr_ctrl_new_on_chip_ldo(&power_cfg, &sd_power), TAG, "SD power");
    }
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    bsp_sdcard_get_sdmmc_host(SDMMC_HOST_SLOT_0, &host);
    bsp_sdcard_sdmmc_get_slot(SDMMC_HOST_SLOT_0, &slot);
    host.pwr_ctrl_handle = sd_power;
    /* 20 MHz gives ample MP3/WAV bandwidth and margins across card models. */
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 12,
        .allocation_unit_size = 64 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdmmc_mount(TAB5_SD_PATH, &host, &slot, &mount, &card);
    if (err != ESP_OK) {
        card = NULL;
        ESP_LOGW(TAG, "SD mount failed: %s (card left untouched)", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "SD mounted, card_bytes=%llu, mount_ms=%lu",
                 (unsigned long long)card->csd.capacity * card->csd.sector_size,
                 (unsigned long)((esp_timer_get_time() - started) / 1000));
    }
    return err;
}

esp_err_t tab5_storage_unmount(void)
{
    if (!card) return ESP_OK;
    sdmmc_card_t *mounted = card;
    /* IDF frees the card before its final VFS unregister, even when that last
     * operation fails. Never retain a potentially freed card pointer. */
    card = NULL;
    esp_err_t err = esp_vfs_fat_sdcard_unmount(TAB5_SD_PATH, mounted);
    if (err == ESP_OK) ESP_LOGI(TAG, "SD unmounted");
    else ESP_LOGE(TAG, "SD unmount failed: %s", esp_err_to_name(err));
    return err;
}

esp_err_t tab5_storage_info(tab5_storage_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    if (!card) return ESP_ERR_INVALID_STATE;
    /* Hash explicit fields rather than struct padding; no SD I/O is needed. */
    uint64_t identity = UINT64_C(14695981039346656037);
    const uint64_t cid_fields[] = {
        (uint32_t)card->cid.mfg_id, (uint32_t)card->cid.oem_id,
        (uint32_t)card->cid.serial, (uint32_t)card->cid.revision,
        (uint32_t)card->cid.date, card->csd.capacity, card->csd.sector_size,
    };
    for (size_t field = 0; field < sizeof(cid_fields) / sizeof(cid_fields[0]); ++field) {
        uint64_t value = cid_fields[field];
        for (unsigned byte = 0; byte < 8; ++byte) {
            identity = (identity ^ (value & 0xff)) * UINT64_C(1099511628211);
            value >>= 8;
        }
    }
    for (size_t i = 0; i < sizeof(card->cid.name); ++i) {
        identity = (identity ^ (uint8_t)card->cid.name[i]) * UINT64_C(1099511628211);
    }
    info->identity = identity ? identity : 1;
    const int64_t started = esp_timer_get_time();
    BYTE pdrv = ff_diskio_get_pdrv_card(card);
    if (pdrv >= FF_VOLUMES) return ESP_FAIL;
    char drive[6];
    snprintf(drive, sizeof(drive), "%u:/", (unsigned)pdrv);
    FF_DIR root;
    /* Opening the root gives the public filesystem object without walking its
     * contents. f_getfree/esp_vfs_fat_info would count EVERY allocation bit on
     * an exFAT volume after mount, blocking the player for a status statistic. */
    const FRESULT opened = f_opendir(&root, drive);
    if (opened != FR_OK) {
        ESP_LOGW(TAG, "SD metadata open failed: FatFS %d", opened);
        return ESP_FAIL;
    }
    FATFS *fs = root.obj.fs;
#if FF_FS_REENTRANT
    if (!ff_mutex_take(fs->ldrv)) {
        f_closedir(&root);
        return ESP_ERR_TIMEOUT;
    }
#endif
    const DWORD clusters = fs->n_fatent >= 2 ? fs->n_fatent - 2 : 0;
    const DWORD free_clusters = fs->free_clst;
    uint32_t sector_size = FF_MIN_SS;
#if FF_MAX_SS != FF_MIN_SS
    sector_size = fs->ssize;
#endif
    const uint64_t cluster_bytes = (uint64_t)fs->csize * sector_size;
    info->total_bytes = (uint64_t)clusters * cluster_bytes;
    info->free_known = clusters != 0 && free_clusters <= clusters;
    if (info->free_known) info->free_bytes = (uint64_t)free_clusters * cluster_bytes;
    info->exfat = fs->fs_type == FS_EXFAT;
#if FF_FS_REENTRANT
    ff_mutex_give(fs->ldrv);
#endif
    const FRESULT closed = f_closedir(&root);
    ESP_LOGI(TAG, "SD metadata: %s total_bytes=%llu free_known=%d info_ms=%lu",
             info->exfat ? "exFAT" : "FAT", (unsigned long long)info->total_bytes,
             info->free_known, (unsigned long)((esp_timer_get_time() - started) / 1000));
    return closed == FR_OK && info->total_bytes ? ESP_OK : ESP_FAIL;
}

esp_err_t tab5_audio_configure(void *user, uint32_t sample_rate)
{
    (void)user;
    if (sample_rate < 8000 || sample_rate > 96000) return ESP_ERR_NOT_SUPPORTED;
    if (!speaker) {
        speaker = bsp_audio_codec_speaker_init();
        if (!speaker) return ESP_FAIL;
        ESP_RETURN_ON_ERROR(tab5_speaker_gate_restore(), TAG, "restore jack speaker gate");
    }
    if (speaker_rate == sample_rate) return ESP_OK;
    if (speaker_rate) {
        esp_codec_dev_set_out_mute(speaker, true);
        if (esp_codec_dev_close(speaker) != ESP_CODEC_DEV_OK) return ESP_FAIL;
        speaker_rate = 0;
    }
    esp_codec_dev_sample_info_t format = {
        .sample_rate = sample_rate, .channel = 2, .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(speaker, &format) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    // A failed jack read closes the speaker gate, without stopping headphone
    // PCM. The detector retries independently; transient I2C is not EOF.
    (void)tab5_headphones_poll();
    ESP_RETURN_ON_ERROR(tab5_speaker_gate_restore(), TAG, "restore amplifier after codec open");
    speaker_rate = sample_rate;
    return tab5_audio_volume(NULL, speaker_volume, speaker_muted);
}

esp_err_t tab5_audio_write(void *user, const int16_t *stereo, size_t frames)
{
    (void)user;
    if (!speaker_rate || !stereo || frames > INT_MAX / 4) return ESP_ERR_INVALID_ARG;
    return esp_codec_dev_write(speaker, (void *)stereo, (int)(frames * 4)) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t tab5_audio_volume(void *user, uint8_t percent, bool muted)
{
    (void)user;
    speaker_volume = percent > 100 ? 100 : percent;
    speaker_muted = muted;
    ESP_RETURN_ON_ERROR(tab5_speaker_gate(!muted && speaker_volume > 0), TAG, "speaker gate");
    if (!speaker || !speaker_rate) return ESP_OK;
    if (esp_codec_dev_set_out_vol(speaker, speaker_volume) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    return esp_codec_dev_set_out_mute(speaker, muted) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t add_device(uint16_t address, i2c_master_dev_handle_t *device)
{
    if (*device) return ESP_OK;
    i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = address, .scl_speed_hz = 400000};
    return i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, device);
}

static esp_err_t reg_read(i2c_master_dev_handle_t device, uint8_t reg, uint8_t *data, size_t size)
{
    if (!device) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(device, &reg, 1, data, size, 100);
}

esp_err_t tab5_sensors_init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "sensor I2C");
    ESP_RETURN_ON_ERROR(add_device(0x32, &rtc), TAG, "RTC device");
    ESP_RETURN_ON_ERROR(tab5_rtc_release_irq(), TAG, "RTC wake IRQ release");
    /* Tab5 INA226 is 0x41. 0x40 belongs to the ES7210 microphone ADC. */
    ESP_RETURN_ON_ERROR(add_device(0x41, &battery), TAG, "battery device");
    /* RTC backup switchover enabled, preserving other control bits. */
    uint8_t ctl;
    if (reg_read(rtc, 0x1f, &ctl, 1) == ESP_OK) {
        uint8_t data[] = {0x1f, (uint8_t)(ctl | 0x30)};
        (void)i2c_master_transmit(rtc, data, sizeof(data), 100);
    }
    uint8_t id[2];
    ESP_RETURN_ON_ERROR(reg_read(battery, 0xfe, id, 2), TAG, "INA226 manufacturer");
    if (id[0] != 0x54 || id[1] != 0x49) return ESP_ERR_INVALID_RESPONSE;
    ESP_RETURN_ON_ERROR(reg_read(battery, 0xff, id, 2), TAG, "INA226 device ID");
    if (id[0] != 0x22 || (id[1] & 0xf0) != 0x60) return ESP_ERR_INVALID_RESPONSE;
    /* INA226: 16 averaged samples; 1.1 ms shunt/bus; continuous conversion.
     * Voltage and raw shunt readings need no current-register calibration. */
    uint8_t config[] = {0x00, 0x05, 0x27};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(battery, config, sizeof(config), 100), TAG, "INA226 configuration");
    battery_ready = true;
    ESP_LOGI(TAG, "Battery INA226 identified at 0x41, 5 mOhm shunt");
    return ESP_OK;
}

static int from_bcd(uint8_t n)
{
    if ((n & 15) > 9 || (n >> 4) > 9) return -1;
    return (n >> 4) * 10 + (n & 15);
}

static uint8_t to_bcd(int n) { return (uint8_t)(((n / 10) << 4) | (n % 10)); }

esp_err_t tab5_rtc_read(struct tm *value)
{
    if (!value) return ESP_ERR_INVALID_ARG;
    uint8_t data[7], flags;
    ESP_RETURN_ON_ERROR(reg_read(rtc, 0x1d, &flags, 1), TAG, "RTC flags");
    if (flags & 0x02) return ESP_ERR_INVALID_STATE; /* VLF: oscillator stopped. */
    ESP_RETURN_ON_ERROR(reg_read(rtc, 0x10, data, sizeof(data)), TAG, "RTC read");
    struct tm result = {0};
    result.tm_sec = from_bcd(data[0] & 0x7f);
    result.tm_min = from_bcd(data[1] & 0x7f);
    result.tm_hour = from_bcd(data[2] & 0x3f);
    result.tm_mday = from_bcd(data[4] & 0x3f);
    result.tm_mon = from_bcd(data[5] & 0x1f) - 1;
    result.tm_year = from_bcd(data[6]) + 100;
    result.tm_isdst = -1;
    if (result.tm_sec < 0 || result.tm_sec > 59 || result.tm_min < 0 || result.tm_min > 59 ||
        result.tm_hour < 0 || result.tm_hour > 23 || result.tm_mday < 1 || result.tm_mday > 31 ||
        result.tm_mon < 0 || result.tm_mon > 11 || result.tm_year < 100 || result.tm_year > 199) return ESP_ERR_INVALID_RESPONSE;
    struct tm checked = result;
    if (mktime(&checked) == (time_t)-1 || checked.tm_mday != result.tm_mday || checked.tm_mon != result.tm_mon) return ESP_ERR_INVALID_RESPONSE;
    *value = checked;
    return ESP_OK;
}

esp_err_t tab5_rtc_write(const struct tm *value)
{
    if (!rtc || !value || value->tm_year < 100 || value->tm_year > 199 || value->tm_mon < 0 || value->tm_mon > 11 ||
        value->tm_mday < 1 || value->tm_mday > 31 || value->tm_hour < 0 || value->tm_hour > 23 ||
        value->tm_min < 0 || value->tm_min > 59 || value->tm_sec < 0 || value->tm_sec > 59) return ESP_ERR_INVALID_ARG;
    struct tm checked = *value;
    if (mktime(&checked) == (time_t)-1 || checked.tm_mday != value->tm_mday || checked.tm_mon != value->tm_mon) return ESP_ERR_INVALID_ARG;
    uint8_t data[] = {0x10, to_bcd(value->tm_sec), to_bcd(value->tm_min), to_bcd(value->tm_hour),
        (uint8_t)(1U << checked.tm_wday), to_bcd(value->tm_mday), to_bcd(value->tm_mon + 1), to_bcd(value->tm_year - 100)};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(rtc, data, sizeof(data), 100), TAG, "RTC write");
    /* RX8130 flags are write-zero-to-clear; clear VLF only. */
    const uint8_t clear_vlf[] = {0x1d, 0xfd};
    return i2c_master_transmit(rtc, clear_vlf, sizeof(clear_vlf), 100);
}

esp_err_t tab5_battery_read(uint16_t *millivolts, int *percent)
{
    if (!millivolts || !percent) return ESP_ERR_INVALID_ARG;
    tab5_battery_status_t status;
    esp_err_t err = tab5_battery_status_read(&status);
    if (err != ESP_OK) return err;
    if (!status.present) return ESP_ERR_NOT_FOUND;
    *millivolts = status.millivolts;
    *percent = status.estimated_percent;
    return ESP_OK;
}

/* Battery filtering begin. Shared verbatim with the host sensor simulation. */
static bool battery_charging;
static int64_t battery_charge_since_us = -1;
static int64_t battery_charge_low_since_us = -1;
static int64_t battery_discharge_since_us = -1;
static int64_t battery_charge_last_us = -1;
static int64_t battery_level_last_us = -1;
static int64_t battery_level_step_us = -1;
static int64_t battery_level_candidate_us = -1;
static int battery_level_direction;
static int battery_level = -1;
static int64_t battery_filtered_mv_q16;
static uint16_t battery_voltage_window[5];
static unsigned battery_voltage_index;
static tab5_battery_status_t battery_last_status;
static int64_t battery_last_valid_us = -1;

static void battery_charge_reset(void)
{
    battery_charging = false;
    battery_charge_since_us = -1;
    battery_charge_low_since_us = -1;
    battery_discharge_since_us = -1;
    battery_charge_last_us = -1;
}

static void battery_state_reset(void)
{
    battery_charge_reset();
    battery_level = -1;
    battery_level_last_us = -1;
    battery_level_step_us = -1;
    battery_level_candidate_us = -1;
    battery_level_direction = 0;
    battery_last_valid_us = -1;
}

static bool battery_charge_update(int milliamps, int64_t now_us)
{
    /* A scheduling gap is not evidence that charging stopped. Preserve the
     * committed state across a short gap, but never count unobserved time
     * toward a transition. Debounce sustained discharge for 1 s so speaker,
     * radio and display load pulses cannot blink the bolt. At the normal
     * 250 ms cadence a real -202 mA unplug clears in <=1.25 s plus rendering.
     * Near-zero termination uses 3 s; a full idle pack never turns it on. */
    if (now_us < 0) { battery_charge_reset(); return false; }
    if (battery_charge_last_us >= 0 &&
        (now_us < battery_charge_last_us || now_us - battery_charge_last_us > 2000000))
        battery_charge_reset();
    else if (battery_charge_last_us >= 0 && now_us - battery_charge_last_us > 1000000) {
        battery_charge_since_us = -1;
        battery_charge_low_since_us = -1;
        battery_discharge_since_us = -1;
    }
    battery_charge_last_us = now_us;
    if (!battery_charging) {
        if (milliamps >= 20) {
            if (battery_charge_since_us < 0) battery_charge_since_us = now_us;
            if (now_us - battery_charge_since_us >= 750000) battery_charging = true;
        } else battery_charge_since_us = -1;
    } else if (milliamps > 5) {
        battery_charge_low_since_us = -1;
        battery_discharge_since_us = -1;
    } else {
        if (battery_charge_low_since_us < 0) battery_charge_low_since_us = now_us;
        if (milliamps <= -20) {
            if (battery_discharge_since_us < 0) battery_discharge_since_us = now_us;
        } else battery_discharge_since_us = -1;
        if (now_us - battery_charge_low_since_us >= 3000000 ||
            (battery_discharge_since_us >= 0 && now_us - battery_discharge_since_us >= 1000000))
            battery_charge_reset();
    }
    return battery_charging;
}

static int battery_voltage_percent(int mv)
{
    /* Same 2S voltage curve as before; this is not state-of-charge accuracy. */
    int level = (mv / 2 - 3300) * 100 / 800;
    return level < 0 ? 0 : level > 100 ? 100 : level;
}

static int battery_level_update(uint16_t mv, int64_t now_us)
{
    if (battery_level < 0 || battery_level_last_us < 0 || now_us < battery_level_last_us) {
        battery_level = battery_voltage_percent(mv);
        battery_filtered_mv_q16 = (int64_t)mv * 65536;
        for (unsigned i = 0; i < 5; ++i) battery_voltage_window[i] = mv;
        battery_voltage_index = 0;
        battery_level_step_us = battery_level_last_us = now_us;
        battery_level_candidate_us = -1;
        battery_level_direction = 0;
        return battery_level;
    }
    int64_t dt = now_us - battery_level_last_us;
    if (!dt) return battery_level; /* Repeated caller is not a new ADC sample. */
    battery_level_last_us = now_us;
    if (dt > 1000000) {
        /* Restart confirmation after a stall, without jumping the display. */
        dt = 1000000;
        battery_level_candidate_us = -1;
    }
    battery_voltage_window[battery_voltage_index++ % 5] = mv;
    uint16_t sorted[5];
    memcpy(sorted, battery_voltage_window, sizeof(sorted));
    for (unsigned i = 1; i < 5; ++i) {
        uint16_t value = sorted[i]; unsigned j = i;
        while (j && sorted[j - 1] > value) { sorted[j] = sorted[j - 1]; --j; }
        sorted[j] = value;
    }
    /* Median rejects isolated transients. 8 s low-pass + 1.25 percentage
     * point hysteresis rejects load/charger voltage ripple. Never synthesize
     * capacity, compensate using an assumed battery, or modify raw mV/mA. */
    battery_filtered_mv_q16 += ((int64_t)sorted[2] * 65536 - battery_filtered_mv_q16) * dt / (8000000 + dt);
    int centipercent = (int)((battery_filtered_mv_q16 - INT64_C(6600) * 65536) * 10000 / (INT64_C(1600) * 65536));
    if (centipercent < 0) centipercent = 0;
    if (centipercent > 10000) centipercent = 10000;
    int difference = centipercent - battery_level * 100;
    int direction = difference >= 125 ? 1 : difference <= -125 ? -1 : 0;
    if (centipercent >= 9950 && battery_level < 100) direction = 1;
    if (centipercent <= 50 && battery_level > 0) direction = -1;
    if (direction != battery_level_direction || !direction) {
        battery_level_direction = direction;
        battery_level_candidate_us = direction ? now_us : -1;
    }
    if (direction && battery_level_candidate_us < 0) battery_level_candidate_us = now_us;
    /* At most one point per 5 s down / 10 s up, after 3 s confirmation.
     * The raw estimate remains available for diagnostics. This is smoothing,
     * not a promise of an exact remaining battery percentage. */
    int64_t interval = direction > 0 ? 10000000 : 5000000;
    if (direction && now_us - battery_level_candidate_us >= 3000000 && now_us - battery_level_step_us >= interval) {
        battery_level += direction;
        battery_level_step_us = now_us;
    }
    return battery_level;
}

static bool battery_cached_status(tab5_battery_status_t *status, int64_t now_us)
{
    /* One transient I2C timeout must not flash the battery or charging icon.
     * This is the previous real sample, explicitly timestamped, never a
     * fabricated reading. After 1 s the caller reports unavailable instead. */
    if (battery_last_valid_us < 0 || now_us < battery_last_valid_us || now_us - battery_last_valid_us > 1000000)
        return false;
    *status = battery_last_status;
    status->sample_age_ms = (uint32_t)((now_us - battery_last_valid_us + 999) / 1000);
    /* Failed observations must not count toward a pending state change. */
    battery_charge_since_us = -1;
    battery_charge_low_since_us = -1;
    battery_discharge_since_us = -1;
    return true;
}
/* Battery filtering end. */

esp_err_t tab5_battery_status_read(tab5_battery_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    memset(status, 0, sizeof(*status));
    status->estimated_percent = status->raw_estimated_percent = -1;
    if (!battery_ready) { battery_state_reset(); return ESP_ERR_INVALID_STATE; }
    int64_t now_us = esp_timer_get_time();
    uint8_t data[2];
    esp_err_t err = reg_read(battery, 0x02, data, sizeof(data));
    if (err != ESP_OK) {
        if (battery_cached_status(status, now_us)) return ESP_OK;
        battery_charge_reset();
        return err;
    }
    uint32_t mv = (((uint32_t)data[0] << 8) | data[1]) * 5 / 4;
    status->millivolts = (uint16_t)mv;
    status->present = mv >= 5000 && mv <= 9000;
    if (!status->present) { battery_state_reset(); return ESP_OK; }
    status->raw_estimated_percent = battery_voltage_percent((int)mv);
    status->estimated_percent = battery_level_update((uint16_t)mv, now_us);
    status->filtered_millivolts = (uint16_t)((battery_filtered_mv_q16 + 32768) / 65536);
    /* Tab5 shunt = 5 mOhm; INA226 shunt LSB = 2.5 uV. Positive into pack. */
    if (reg_read(battery, 0x01, data, sizeof(data)) == ESP_OK) {
        int16_t raw = (int16_t)(((uint16_t)data[0] << 8) | data[1]);
        status->milliamps = -(int)raw / 2;
        status->charging = battery_charge_update(status->milliamps, now_us);
        status->charging_known = true;
        battery_last_status = *status;
        battery_last_valid_us = now_us;
    } else {
        if (battery_cached_status(status, now_us)) return ESP_OK;
        battery_charge_reset();
    }
    /* Bolt = measured current into pack, not USB presence. CHG_STAT must
     * never override discharge: it stayed HIGH at -202 mA after unplugging. */
    return ESP_OK;
}
