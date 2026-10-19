#pragma once
#include <stdint.h>
#include <stdbool.h>
/* ABI-shaped test substitutes; no vendor codec implementation is emulated. */
typedef int esp_audio_err_t;
enum {ESP_AUDIO_ERR_OK=0,ESP_AUDIO_ERR_CONTINUE=1,ESP_AUDIO_ERR_DATA_LACK=-3,ESP_AUDIO_ERR_BUFF_NOT_ENOUGH=-8};
enum {ESP_AUDIO_SIMPLE_DEC_TYPE_AAC=1,ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC=2};
typedef void *esp_audio_simple_dec_handle_t;
typedef struct {int dec_type;void *dec_cfg;int cfg_size;bool use_frame_dec;} esp_audio_simple_dec_cfg_t;
typedef struct {uint8_t *buffer;uint32_t len;bool eos;uint32_t consumed;int frame_recover;} esp_audio_simple_dec_raw_t;
typedef struct {uint8_t *buffer;uint32_t len,needed_size,decoded_size;} esp_audio_simple_dec_out_t;
typedef struct {uint32_t sample_rate;uint8_t bits_per_sample,channel;uint32_t bitrate,frame_size;} esp_audio_simple_dec_info_t;
esp_audio_err_t esp_audio_simple_dec_open(esp_audio_simple_dec_cfg_t*,esp_audio_simple_dec_handle_t*);
esp_audio_err_t esp_audio_simple_dec_process(esp_audio_simple_dec_handle_t,esp_audio_simple_dec_raw_t*,esp_audio_simple_dec_out_t*);
esp_audio_err_t esp_audio_simple_dec_get_info(esp_audio_simple_dec_handle_t,esp_audio_simple_dec_info_t*);
void esp_audio_simple_dec_close(esp_audio_simple_dec_handle_t);
