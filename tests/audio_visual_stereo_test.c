#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <windows.h>
#include "audio_engine.h"
#define RATE 48000u
#define FRAMES (RATE*2)
static volatile LONG writes;
static volatile LONG output_peak;
static esp_err_t configure(void *user,uint32_t rate){(void)user;assert(rate==RATE);return ESP_OK;}
static esp_err_t output(void *user,const int16_t *pcm,size_t frames){(void)user;LONG peak=InterlockedCompareExchange(&output_peak,0,0);for(size_t i=0;i<frames*2;++i)if(abs(pcm[i])>peak)peak=abs(pcm[i]);InterlockedExchange(&output_peak,peak);InterlockedIncrement(&writes);Sleep((DWORD)(frames*1000/RATE));return ESP_OK;}
static esp_err_t volume(void *user,uint8_t value,bool muted){(void)user;(void)value;(void)muted;return ESP_OK;}
static void le16(FILE *f,unsigned value){fputc(value&255,f);fputc((value>>8)&255,f);}
static void le32(FILE *f,unsigned value){le16(f,value);le16(f,value>>16);}
static void fixture(const char *path,int left,int right)
{
    FILE *f=fopen(path,"wb");assert(f);fwrite("RIFF",1,4,f);le32(f,36+FRAMES*4);fwrite("WAVEfmt ",1,8,f);
    le32(f,16);le16(f,1);le16(f,2);le32(f,RATE);le32(f,RATE*4);le16(f,4);le16(f,16);
    fwrite("data",1,4,f);le32(f,FRAMES*4);
    for(unsigned i=0;i<FRAMES;i++){int sample=(int)lrint(16000*sin(6.283185307179586*1000*i/RATE));le16(f,(uint16_t)(sample*left));le16(f,(uint16_t)(sample*right));}
    assert(!ferror(f));assert(!fclose(f));
}
static float measure(const char *path,int left,int right)
{
    fixture(path,left,right);LONG before=InterlockedCompareExchange(&writes,0,0);InterlockedExchange(&output_peak,0);
    assert(audio_engine_play(path)==ESP_OK);ULONGLONG start=GetTickCount64();audio_snapshot_t s;
    do{audio_engine_get_snapshot(&s);if(s.state==AUDIO_PLAYING && s.position_ms>=180)break;Sleep(5);}while(GetTickCount64()-start<2000);
    assert(s.state==AUDIO_PLAYING && s.position_ms>=180);assert(InterlockedCompareExchange(&writes,0,0)>before);
    float peak=0;int wave=0;for(unsigned i=0;i<AUDIO_SPECTRUM_BANDS;i++)if(s.spectrum[i]>peak)peak=s.spectrum[i];
    for(unsigned i=0;i<AUDIO_WAVEFORM_SAMPLES;i++)if(abs(s.waveform[i])>wave)wave=abs(s.waveform[i]);
    printf("stereo visual L=%d R=%d: spectrum=%.6f waveform_peak=%d position=%u\n",left,right,peak,wave,s.position_ms);
    const LONG actual=InterlockedCompareExchange(&output_peak,0,0);
    if(left||right){assert(abs(wave-16000)<=4);assert(actual==16000);}
    else {assert(wave==0&&actual==0);}
    assert(audio_engine_stop_and_wait(2000)==ESP_OK);return peak;
}
int main(int argc,char **argv)
{
    (void)argv;audio_output_t out={.configure=configure,.write=output,.volume=volume};assert(audio_engine_init(&out)==ESP_OK);
    assert(audio_engine_set_volume(0,true)==ESP_OK); /* Visuals must ignore volume and mute. */
    char dir[MAX_PATH],path[MAX_PATH];assert(GetTempPathA(sizeof dir,dir));assert(GetTempFileNameA(dir,"t5v",0,path));
    float same=measure(path,1,1),opposed=measure(path,1,-1),left=measure(path,1,0),right=measure(path,0,1),silence=measure(path,0,0);
    assert(DeleteFileA(path));
    if(argc>1){assert(same>.8f && opposed==0);puts("CONFIRMED baseline bug: loud opposite-phase stereo disappears from FFT.");return 0;}
    assert(same>.8f && opposed>.8f && left>.8f && right>.8f);assert(silence==0);
    assert(fabsf(same-opposed)<.01f && fabsf(same-left)<.01f && fabsf(same-right)<.01f);
    puts("PASS: original unity output peak and FFT/waveform sensitivity; no hidden attenuation. In-phase/opposite-phase/single-channel/silence are independent of mute/volume.");return 0;
}
