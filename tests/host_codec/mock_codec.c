#include "esp_audio_dec_default.h"
#include <stdlib.h>
#include <string.h>
int mock_codec_mode,mock_codec_calls,mock_codec_closes;
static const uint8_t *retry_input;
static uint32_t retry_len;
esp_audio_err_t esp_audio_dec_register_default(void){return 0;}
esp_audio_err_t esp_audio_simple_dec_register_default(void){return 0;}
esp_audio_err_t esp_audio_simple_dec_open(esp_audio_simple_dec_cfg_t *cfg,esp_audio_simple_dec_handle_t *handle)
{(void)cfg;*handle=malloc(1);retry_input=NULL;return *handle?0:-2;}
void esp_audio_simple_dec_close(esp_audio_simple_dec_handle_t handle){free(handle);++mock_codec_closes;}
esp_audio_err_t esp_audio_simple_dec_process(esp_audio_simple_dec_handle_t handle,esp_audio_simple_dec_raw_t *raw,esp_audio_simple_dec_out_t *frame)
{
    (void)handle;++mock_codec_calls;raw->consumed=0;frame->decoded_size=0;
    if(raw->len==42 && !memcmp(raw->buffer,"fLaC",4)){raw->consumed=42;return 0;}
    if(mock_codec_mode==3)return 0;
    if(mock_codec_mode==4){raw->consumed=raw->len+1;return 0;}
    if(mock_codec_mode==6){frame->decoded_size=frame->len+1;return 0;}
    if(mock_codec_mode==7){raw->consumed=raw->len;return ESP_AUDIO_ERR_DATA_LACK;}
    if(mock_codec_mode==2){frame->needed_size=1024*1024;return ESP_AUDIO_ERR_BUFF_NOT_ENOUGH;}
    if(mock_codec_mode==1 && frame->len<40000) {
        retry_input=raw->buffer;retry_len=raw->len;frame->needed_size=40000;return ESP_AUDIO_ERR_BUFF_NOT_ENOUGH;
    }
    if(retry_input && (retry_input!=raw->buffer || retry_len!=raw->len))return -1;
    frame->decoded_size=mock_codec_mode==1?40000:8;
    for(unsigned i=0;i<frame->decoded_size;i+=2){frame->buffer[i]=0x34;frame->buffer[i+1]=0x12;}
    raw->consumed=raw->len;return 0;
}
esp_audio_err_t esp_audio_simple_dec_get_info(esp_audio_simple_dec_handle_t handle,esp_audio_simple_dec_info_t *info)
{(void)handle;*info=(esp_audio_simple_dec_info_t){.sample_rate=44100,.bits_per_sample=16,.channel=mock_codec_mode==5?8:1};return 0;}
