#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_EQ_BANDS 11

typedef struct {
    int8_t gain_db[AUDIO_EQ_BANDS];
    int8_t preamp_db;
    uint8_t enabled;
} audio_eq_config_t;

typedef struct {
    float b0,b1,b2,a1,a2;
    float t0,t1,t2,mix;
    float z1[2],z2[2];
} audio_eq_filter_t;

typedef struct {
    audio_eq_config_t config;
    audio_eq_filter_t filters[AUDIO_EQ_BANDS];
    float input_gain;
    uint8_t count,identity;
} audio_eq_bank_t;

/* Fixed memory. Reset once at a new sample rate, then call only from the audio
 * thread. No allocation, trig/pow/exp/log or coefficient updates per sample.
 * Settings changes are prepared once, then crossfaded over 20 ms. */
typedef struct {
    uint32_t rate,transition,transition_frames,hold_frames;
    float release,limiter_gain,limiter_deficit,reduction_gain;
    uint32_t hold;
    audio_eq_bank_t current,previous;
} audio_eq_t;

extern const uint16_t audio_eq_frequencies[AUDIO_EQ_BANDS];
bool audio_eq_valid_config(const audio_eq_config_t *config);
bool audio_eq_band_available(uint32_t sample_rate,unsigned band);
/* Maximum actual linked-limiter reduction over the last processed block,
 * positive dB. Excludes manual preamp; zero after settled bypass recovery.
 * No automatic gain compensation: boosts retain the manual preamp level
 * unless the mixed output actually overloads. */
float audio_eq_reduction_db(const audio_eq_t *eq);
void audio_eq_reset(audio_eq_t *eq,uint32_t sample_rate);
/* Initialize a fresh stream at its intended level, without a fade from unity. */
void audio_eq_reset_config(audio_eq_t *eq,uint32_t sample_rate,const audio_eq_config_t *config);
/* Interleaved stereo PCM16. NULL/invalid config means bypass. Disabled and
 * effective-flat settings are bit-exact after the transition and any limiter
 * recovery from a preceding overload. Gains on bands
 * at or above 0.475*sample_rate are stored but do not affect processing. */
void audio_eq_process(audio_eq_t *eq,int16_t *stereo,size_t frames,
                      const audio_eq_config_t *config);

#ifdef __cplusplus
}
#endif
