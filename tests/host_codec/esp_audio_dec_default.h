#pragma once
#include "esp_audio_simple_dec.h"
typedef struct {int32_t sample_rate;uint8_t channel,bits_per_sample;bool no_adts_header,aac_plus_enable;} esp_aac_dec_cfg_t;
esp_audio_err_t esp_audio_dec_register_default(void);
