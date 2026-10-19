#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TAB5_SD_PATH "/sdcard"
typedef struct {
    uint64_t total_bytes, free_bytes;
    uint64_t identity; /* Stable CID/capacity fingerprint for this physical SD. */
    bool exfat;
    bool free_known; /* false means free_bytes is unavailable, not a full card */
} tab5_storage_info_t;

/* Pass 0 to keep startup hidden until tab5_display_reveal() presents its frame. */
lv_display_t *tab5_display_init(uint8_t initial_brightness);
bool tab5_touch_ready(void);
/* All screen power/snapshot calls require the LVGL lock. Touch polling stays alive. */
/* Flush the active startup screen and wait for scanout before enabling light.
 * Intended for the first boot frame; a failure leaves the backlight unchanged. */
esp_err_t tab5_display_reveal(uint8_t brightness);
esp_err_t tab5_display_sleep(bool sleep, uint8_t brightness);
bool tab5_display_wake_requested(void);
bool tab5_display_is_sleeping(void);
esp_err_t tab5_display_snapshot_rgb565(uint16_t *out, size_t pixels);
const char *tab5_display_last_error(void);
bool tab5_display_lock(uint32_t timeout_ms);
void tab5_display_unlock(void);
esp_err_t tab5_brightness_set(uint8_t percent);
/* Mount is non-destructive: a failed card is never formatted. Call outside LVGL. */
esp_err_t tab5_storage_mount(void);
/* Caller must first close every file and quiesce scan, audio and artwork I/O.
 * Idempotent when unmounted. Does not substitute for saving/syncing files. */
esp_err_t tab5_storage_unmount(void);
/* Reads volume metadata and an already cached free count; never scans the FAT
 * or exFAT allocation bitmap merely to populate the status display. */
esp_err_t tab5_storage_info(tab5_storage_info_t *info);

/* Hardware power latch pulse, same signal as M5Unified/Launcher. Call only
 * after saving and stopping all workers. Successful cutoff cannot return;
 * any return is an I2C/init error or ESP_ERR_TIMEOUT if power stayed on.
 * Does not fall back to deep sleep, reset or silently dim the display. */
esp_err_t tab5_power_off(void);
/* Release unused retained RTC wake IRQs without changing time or VLF flags.
 * Requires an initialized BSP I2C bus. Also performed before power-off. */
esp_err_t tab5_rtc_release_irq(void);

/* Sink callbacks for audio_output_t. 16-bit stereo; calls are serialized. */
esp_err_t tab5_audio_configure(void *user, uint32_t sample_rate);
esp_err_t tab5_audio_write(void *user, const int16_t *stereo, size_t frames);
esp_err_t tab5_audio_volume(void *user, uint8_t percent, bool muted);
esp_err_t tab5_headphones_init(void);
esp_err_t tab5_headphones_poll(void);
bool tab5_headphones_status(bool *inserted);
esp_err_t tab5_speaker_gate(bool active);
esp_err_t tab5_speaker_gate_restore(void);
esp_err_t tab5_charger_init(void);

/* Optional BMI270, owned by one worker: serialize init/enable/read calls and
 * never call them under the LVGL lock. Init leaves every sensing block off;
 * enable runs ONLY the accelerometer at 400 Hz, +/-8 g, into the sensor FIFO
 * (headerless, accelerometer frames only), and empties the FIFO. Samples are
 * therefore spaced by the sensor clock (TAB5_MOTION_RATE_HZ), not by when the
 * caller polls. Drain at least every 0.5 s: a backlog of 1536 bytes (0.64 s)
 * is reported as overflow, before the 2048-byte FIFO could drop a frame.
 * read_fifo returns raw chip axes (TAB5_MOTION_LSB_PER_G), oldest first.
 * overflow=true means frames were or may have been lost: the FIFO has been
 * emptied, and the caller must treat the stream as discontinuous.
 * Any read error invalidates the samples; discard the gesture, disable and
 * back off. A transient read timeout retains the loaded sensor configuration;
 * the next enable validates it. Other errors latch this adapter unavailable,
 * and set_enabled(true) retries full initialization after caller backoff.
 * No GPIO interrupt, wake source, RTC register or shared I2C reset is changed.
 * set_enabled(false) is also the shutdown operation (bounded I2C, no deletion
 * of the shared bus). Other users may safely transact on the BSP I2C bus. */
#define TAB5_MOTION_RATE_HZ 400
#define TAB5_MOTION_LSB_PER_G 4096
typedef struct { int16_t x, y, z; } tab5_motion_sample_t;
esp_err_t tab5_motion_init(void);
esp_err_t tab5_motion_set_enabled(bool enabled);
esp_err_t tab5_motion_read_fifo(tab5_motion_sample_t *samples, size_t capacity, size_t *count, bool *overflow);

/* Optional sensors return errors, never fabricated values. RTC stores local time. */
esp_err_t tab5_sensors_init(void);
esp_err_t tab5_rtc_read(struct tm *local_time);
esp_err_t tab5_rtc_write(const struct tm *local_time);
typedef struct {
    uint16_t millivolts;
    int milliamps; /* Positive into battery, negative while discharging. */
    int estimated_percent; /* Filtered voltage estimate, not a fuel gauge. */
    int raw_estimated_percent; /* Unfiltered voltage estimate for diagnostics. */
    uint16_t filtered_millivolts;
    uint32_t sample_age_ms; /* 0=fresh, <=1000=last valid sample after I/O fault. */
    bool present;
    bool charging;
    bool charging_known;
} tab5_battery_status_t;
esp_err_t tab5_battery_status_read(tab5_battery_status_t *status);
esp_err_t tab5_battery_read(uint16_t *millivolts, int *estimated_percent);

#ifdef __cplusplus
}
#endif
