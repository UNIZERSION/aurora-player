#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Four packed float feedback lines at <=48 kHz wet processing rate.
 * Dry stays at source rate. Fixed memory, no audio-thread allocation. */
#define AUDIO_FX_REVERB_TAPS 4
#define AUDIO_FX_REVERB_FRAMES 9600
typedef enum { AUDIO_EFFECT_NORMAL=0, AUDIO_EFFECT_BASS_PLUS=1,
               AUDIO_EFFECT_REVERB=2, AUDIO_EFFECT_SPACE=AUDIO_EFFECT_REVERB,
               AUDIO_EFFECT_RETRO=3 } audio_effect_t;
typedef struct {
    uint16_t decay_ms;    /* 500..4000 ms, nominal low-frequency T60. */
    uint16_t tone_hz;     /* 100..20000 Hz upper wet cutoff; legacy field name. */
    uint8_t amount;       /* 0..60 percent wet signal. */
    uint16_t low_cut_hz;  /* 0 disables low cut; otherwise < tone_hz. */
} audio_reverb_config_t;
audio_reverb_config_t audio_reverb_default_config(void);
bool audio_reverb_valid_config(const audio_reverb_config_t *config);
typedef struct {
    uint8_t effect;
    float b0,b1,b2,a1,a2,z1[2],z2[2];
    float lowpass,low[2],low2[2];
    float gain;
    uint32_t hold;
    uint32_t reverb_start[4],reverb_end[4],reverb_position[4];
    float reverb_feedback[4],reverb_low[4],reverb_low2[4],reverb_damping;
    float reverb_delay[AUDIO_FX_REVERB_FRAMES];
    uint32_t reverb_divisor,reverb_phase;
    float reverb_input_low[2][4],reverb_input_alpha,reverb_step;
    float reverb_previous[2],reverb_next[2];
    float reverb_output_low[2][3],reverb_output_alpha;
    float reverb_band_low[2][2],reverb_band_high[2];
    float reverb_low_alpha,reverb_low_alpha_target,reverb_low_mix,reverb_low_mix_target;
    float reverb_high_alpha,reverb_high_alpha_target;
    float reverb_wet[2];
    float reverb_feedback_target[4],reverb_damping_target;
    float reverb_amount,reverb_amount_target,reverb_smooth,reverb_mix_smooth;
} audio_fx_voice_t;
typedef struct {
    uint32_t rate, transition, transition_frames, hold_frames;
    float release;
    audio_reverb_config_t reverb_config;
    audio_fx_voice_t current, previous;
} audio_fx_t;
void audio_fx_reset(audio_fx_t *fx,uint32_t sample_rate,uint8_t effect);
/* New track / seek: initialize the selected saved settings immediately. */
void audio_fx_reset_config(audio_fx_t *fx,uint32_t sample_rate,uint8_t effect,const audio_reverb_config_t *config);
/* Audio-thread only. Changes target coefficients without clearing the tail;
 * interpolation is sample based and independent of PCM block boundaries. */
bool audio_fx_set_reverb_config(audio_fx_t *fx,const audio_reverb_config_t *config);
void audio_fx_process(audio_fx_t *fx,int16_t *stereo,size_t frames,uint8_t effect);
#ifdef __cplusplus
}
#endif
