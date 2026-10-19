#include "audio_extra_codec.h"
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec_default.h"
#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "codec_selftest_fixtures.h"
#endif

#define MAX_PCM_BYTES (512u*1024u)
#define MAX_UNPRODUCTIVE_BYTES (1024u*1024u)
struct audio_extra_codec {
    esp_audio_simple_dec_handle_t handle;
    esp_audio_simple_dec_info_t info;
    uint8_t *pcm;
    size_t capacity,size,used,unproductive;
};
static bool registered;
void audio_extra_codec_close(audio_extra_codec_t *c)
{
    if(!c)return;
    if(c->handle)esp_audio_simple_dec_close(c->handle);
    free(c->pcm);free(c);
}
audio_extra_codec_t *audio_extra_codec_open(audio_format_t format,const uint8_t header[42])
{
    if(format!=AUDIO_FORMAT_FLAC && format!=AUDIO_FORMAT_AAC)return NULL;
    if(!registered) {
        if(esp_audio_dec_register_default()!=ESP_AUDIO_ERR_OK ||
           esp_audio_simple_dec_register_default()!=ESP_AUDIO_ERR_OK)return NULL;
        registered=true;
    }
    audio_extra_codec_t *c=calloc(1,sizeof(*c));if(!c)return NULL;
    c->capacity=32768;c->pcm=heap_caps_malloc(c->capacity,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    esp_aac_dec_cfg_t aac={0};aac.aac_plus_enable=false;
    esp_audio_simple_dec_cfg_t cfg={.dec_type=format==AUDIO_FORMAT_FLAC?ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC:ESP_AUDIO_SIMPLE_DEC_TYPE_AAC,
        .dec_cfg=format==AUDIO_FORMAT_AAC?&aac:NULL,.cfg_size=format==AUDIO_FORMAT_AAC?sizeof(aac):0,.use_frame_dec=false};
    if(!c->pcm || esp_audio_simple_dec_open(&cfg,&c->handle)!=ESP_AUDIO_ERR_OK)goto fail;
    if(format==AUDIO_FORMAT_FLAC) {
        size_t consumed=0;
        if(!header || audio_extra_codec_decode(c,header,42,false,&consumed)<0 || consumed!=42 || c->size)goto fail;
    }
    return c;
fail:
    audio_extra_codec_close(c);return NULL;
}
int audio_extra_codec_decode(audio_extra_codec_t *c,const uint8_t *input,size_t bytes,bool eos,size_t *consumed)
{
    if(!c || !consumed || bytes>65536 || (!input && bytes))return -1;
    *consumed=0;if(c->used<c->size)return 1;
    c->used=c->size=0;
    /* EOS was supplied with the final nonempty input. Espressif's simple
     * decoder flushes while consuming that buffer; zero-length calls are not
     * part of its documented streaming loop and some parsers reject them. */
    if(!bytes)return eos?0:-1;
    esp_audio_simple_dec_raw_t raw={.buffer=(uint8_t*)input,.len=(uint32_t)bytes,.eos=eos};
    esp_audio_simple_dec_out_t frame={.buffer=c->pcm,.len=(uint32_t)c->capacity};
    esp_audio_err_t result=esp_audio_simple_dec_process(c->handle,&raw,&frame);
    if(result==ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
        /* Retry the SAME raw input, as required by Espressif's API. A decoder
         * cannot force unbounded allocation or a reallocation retry loop. */
        if(frame.needed_size<=c->capacity || frame.needed_size>MAX_PCM_BYTES)return -1;
        uint8_t *larger=heap_caps_malloc(frame.needed_size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!larger)return -1;
        free(c->pcm);c->pcm=larger;c->capacity=frame.needed_size;
        raw.consumed=0;frame=(esp_audio_simple_dec_out_t){.buffer=c->pcm,.len=(uint32_t)c->capacity};
        result=esp_audio_simple_dec_process(c->handle,&raw,&frame);
    }
    if(raw.consumed>bytes || frame.decoded_size>c->capacity)return -1;
    if(result!=ESP_AUDIO_ERR_OK && result!=ESP_AUDIO_ERR_DATA_LACK && result!=ESP_AUDIO_ERR_CONTINUE)return -1;
    *consumed=raw.consumed;
    if(frame.decoded_size) {
        if(esp_audio_simple_dec_get_info(c->handle,&c->info)!=ESP_AUDIO_ERR_OK)return -1;
        if(c->info.sample_rate<8000 || c->info.sample_rate>96000 ||
           (c->info.channel!=1 && c->info.channel!=2) ||
           (c->info.bits_per_sample!=16 && c->info.bits_per_sample!=24 && c->info.bits_per_sample!=32))return -1;
        const unsigned align=c->info.channel*(c->info.bits_per_sample/8);
        if(frame.decoded_size%align)return -1;
        c->size=frame.decoded_size;c->unproductive=0;return 1;
    }
    c->unproductive+=raw.consumed;
    if(c->unproductive>MAX_UNPRODUCTIVE_BYTES)return -1;
    if(!bytes && eos)return 0;
    /* No progress with input is an error, not a spin on the decoder core. */
    return raw.consumed?2:-1;
}
size_t audio_extra_codec_read(audio_extra_codec_t *c,int16_t *stereo,size_t frames,uint32_t *rng)
{
    if(!c || !stereo || !rng || c->used>=c->size)return 0;
    const unsigned align=c->info.channel*(c->info.bits_per_sample/8);
    const size_t available=(c->size-c->used)/align;if(frames>available)frames=available;
    audio_pcm_to_stereo16(c->pcm+c->used,frames,c->info.bits_per_sample,c->info.channel,rng,stereo);
    c->used+=frames*align;return frames;
}
bool audio_extra_codec_info(audio_extra_codec_t *c,uint32_t *rate,uint16_t *bits,uint16_t *channels)
{
    if(!c || !c->info.sample_rate)return false;
    *rate=c->info.sample_rate;*bits=c->info.bits_per_sample;*channels=c->info.channel;return true;
}
#ifdef ESP_PLATFORM
static bool self_test_one(audio_format_t format,const uint8_t *fixture,size_t size,uint32_t expected_rate,unsigned expected_channels,unsigned expected_frames)
{
    uint8_t *input=heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!input)return false;
    memcpy(input,fixture,size);
    audio_extra_codec_t *c=audio_extra_codec_open(format,format==AUDIO_FORMAT_FLAC?input:NULL);
    if(!c){free(input);ESP_LOGE("audio","CODEC_SELFTEST %s open failed",format==AUDIO_FORMAT_FLAC?"FLAC":"AAC");return false;}
    size_t offset=format==AUDIO_FORMAT_FLAC?42:0;unsigned frames=0;uint64_t energy=0;
    uint32_t rng=1,rate=0;uint16_t bits=0,channels=0;bool done=false,failed=false;
    int16_t pcm[256*2];
    for(unsigned calls=0;calls<128;calls++) {
        const size_t n=audio_extra_codec_read(c,pcm,256,&rng);
        if(n) {
            frames+=(unsigned)n;
            for(size_t i=0;i<n*2;i++){const int32_t v=pcm[i];energy+=(uint32_t)(v*v);}
            audio_extra_codec_info(c,&rate,&bits,&channels);continue;
        }
        const size_t left=size-offset,chunk=left<257?left:257;size_t consumed=0;
        const int result=audio_extra_codec_decode(c,input+offset,chunk,offset+chunk==size,&consumed);
        if(result<0){failed=true;break;}
        offset+=consumed;
        if(!result){done=true;break;}
    }
    const bool passed=done && !failed && frames==expected_frames && rate==expected_rate && channels==expected_channels &&
        energy>(uint64_t)frames*2*1000000 && energy<(uint64_t)frames*2*200000000;
    ESP_LOGI("audio","CODEC_SELFTEST %s %s rate=%lu channels=%u bits=%u frames=%u bytes=%u/%u energy=%llu",
             format==AUDIO_FORMAT_FLAC?"FLAC":"AAC",passed?"PASS":"FAIL",(unsigned long)rate,channels,bits,frames,
             (unsigned)offset,(unsigned)size,(unsigned long long)energy);
    audio_extra_codec_close(c);free(input);return passed;
}
bool audio_extra_codec_self_test(void)
{
    const bool flac=self_test_one(AUDIO_FORMAT_FLAC,codec_selftest_flac,sizeof codec_selftest_flac,48000,2,2048);
    const bool aac=self_test_one(AUDIO_FORMAT_AAC,codec_selftest_aac,sizeof codec_selftest_aac,44100,1,5120);
    return flac && aac;
}
#else
bool audio_extra_codec_self_test(void){return false;} /* Host mocks are not decoder verification. */
#endif
