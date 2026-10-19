#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "audio_file.h"
#include "audio_fx.h"
#include "audio_eq.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_PATH_MAX 512
#define AUDIO_SPECTRUM_BANDS 32
#define AUDIO_WAVEFORM_SAMPLES 128

typedef enum {
    AUDIO_STOPPED, AUDIO_LOADING, AUDIO_PLAYING, AUDIO_PAUSED,
    AUDIO_SEEKING, AUDIO_ENDED, AUDIO_ERROR
} audio_state_t;
typedef enum { AUDIO_EVENT_NONE, AUDIO_EVENT_LOADED, AUDIO_EVENT_ENDED,
               AUDIO_EVENT_ERROR } audio_event_t;

/* Callbacks are serialized on audio task. write blocks until all frames are
 * accepted by I2S/DMA. Input buffer is temporary; callback must not retain it.
 * configure selects 16-bit stereo at the source sample rate. */
typedef struct {
    void *user;
    esp_err_t (*configure)(void *user, uint32_t sample_rate);
    esp_err_t (*write)(void *user, const int16_t *stereo, size_t frames);
    esp_err_t (*volume)(void *user, uint8_t percent, bool mute);
} audio_output_t;

typedef struct {
    audio_state_t state;
    audio_event_t event;
    uint32_t event_sequence, position_ms, duration_ms;
    uint32_t sample_rate, bitrate_kbps;
    uint32_t eq_last_us, eq_max_us;
    uint32_t fx_last_us, fx_max_us;
    float eq_reduction_db; /* Positive peak-limiter reduction in the latest PCM block; no automatic EQ gain. */
    uint16_t bits_per_sample, channels;
    uint8_t volume,effect,speed_percent;
    bool muted, seek_supported, duration_exact, seek_approximate;
    audio_format_t format;
    uint64_t file_size;
    uint32_t output_errors;
    /* Since init. A starvation is one continuous empty software PCM queue
     * while playing, not a hardware-confirmed I2S underrun. Decode timing
     * excludes file read time; all timings are elapsed wall microseconds. */
    uint32_t buffer_starvations, max_read_us, max_decode_us;
    uint8_t buffered_blocks; /* Ready PCM blocks, 0..10; advisory snapshot. */
    char path[AUDIO_PATH_MAX], title[128], artist[128], album[128], error[96];
    float spectrum[AUDIO_SPECTRUM_BANDS]; /* -72..0 dBFS mapped to 0..1 */
    int16_t waveform[AUDIO_WAVEFORM_SAMPLES]; /* real decoded mono samples */
} audio_snapshot_t;

esp_err_t audio_engine_init(const audio_output_t *output);
/* Async commands. ESP_OK means queued, completion/error is in snapshot.
 * Return ESP_ERR_TIMEOUT when command queue is full; caller may retry. */
esp_err_t audio_engine_play(const char *path);
/* Load without any PCM output when transport was paused. */
esp_err_t audio_engine_play_paused(const char *path, bool paused);
esp_err_t audio_engine_pause(bool paused);
/* Fresh-command barrier: ESP_OK requires PAUSED (or preserves an already idle
 * STOPPED/ENDED/ERROR state), completion of every in-flight output callback,
 * and a successful volume(..., true) from the output task. Keeps path/position;
 * no further PCM writes until explicit resume/play.
 * Call outside display lock and serialize transport changes at the caller.
 * Timeout does not cancel the queued pause. A superseding transport command
 * returns INVALID_STATE; a missing volume callback returns NOT_SUPPORTED. */
esp_err_t audio_engine_pause_and_wait(uint32_t timeout_ms);
esp_err_t audio_engine_stop(void);
/* Queue stop and wait for that exact command to close the decoder FILE.
 * Call outside the LVGL/display lock; useful before catalog rescans. */
esp_err_t audio_engine_stop_and_wait(uint32_t timeout_ms);
esp_err_t audio_engine_seek_ms(uint32_t position_ms);
/* Coalesced without queue growth. Calls the hardware volume callback. */
esp_err_t audio_engine_set_volume(uint8_t percent, bool muted);
/* Coalesced, applies to the next output PCM block; 0 NORMAL, 1 BASS+, 2 SPACE,
 * 3 CUSTOM EQ. One DSP stage feeds both local output and Bluetooth. */
esp_err_t audio_engine_set_effect(uint8_t effect);
esp_err_t audio_engine_set_eq(const audio_eq_config_t *config);
/* Coalesced, allocation-free. The output task slews reverb parameters without
 * clearing its tail; the same processed PCM reaches local and BT outputs. */
esp_err_t audio_engine_set_reverb(const audio_reverb_config_t *config);
/* 75..125%, with pitch following speed, like a turntable. */
esp_err_t audio_engine_set_speed(uint8_t percent);
void audio_engine_set_visuals_enabled(bool enabled);
void audio_engine_get_snapshot(audio_snapshot_t *out);

#ifdef __cplusplus
}
#endif
