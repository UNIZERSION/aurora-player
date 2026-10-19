#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <windows.h>
#include "audio_engine.h"
static volatile LONG writes,configured,fail_output;
static uint32_t current_rate;
void host_audio_delay_next_decode(unsigned ms);
static esp_err_t configure(void *user,uint32_t rate){(void)user;current_rate=rate;InterlockedIncrement(&configured);return ESP_OK;}
static esp_err_t write_pcm(void *user,const int16_t *pcm,size_t frames)
{
    (void)user;(void)pcm;
    if(InterlockedCompareExchange(&fail_output,0,0))return ESP_FAIL;
    InterlockedIncrement(&writes);Sleep((DWORD)(frames*1000/current_rate));return ESP_OK;
}
static esp_err_t volume(void *user,uint8_t percent,bool muted){(void)user;(void)muted;assert(percent<=100);return ESP_OK;}
static audio_snapshot_t snapshot(void){audio_snapshot_t a;audio_engine_get_snapshot(&a);return a;}
static audio_snapshot_t wait_state(audio_state_t target,unsigned ms)
{
    ULONGLONG start=GetTickCount64();audio_snapshot_t a;
    do{a=snapshot();if(a.state==target)return a;Sleep(5);}while(GetTickCount64()-start<ms);
    fprintf(stderr,"Wanted state %d, got %d (%s) at %u ms\n",target,a.state,a.error,a.position_ms);assert(a.state==target);return a;
}
static audio_snapshot_t wait_position(uint32_t lower,uint32_t upper)
{
    ULONGLONG started=GetTickCount64();audio_snapshot_t a;
    do {
        a=snapshot();
        if(a.state==AUDIO_PLAYING && a.position_ms>=lower && a.position_ms<upper)return a;
        Sleep(5);
    } while(GetTickCount64()-started<2000);
    fprintf(stderr,"Wanted playing position %u..%u, got state %d at %u ms (%s)\n",lower,upper,a.state,a.position_ms,a.error);
    assert(a.state==AUDIO_PLAYING && a.position_ms>=lower && a.position_ms<upper);return a;
}
int main(int argc,char **argv)
{
    assert(argc==3);
    audio_output_t output={.configure=configure,.write=write_pcm,.volume=volume};
    assert(audio_engine_init(&output)==ESP_OK);
    assert(snapshot().speed_percent==100);
    assert(audio_engine_set_speed(74)==ESP_ERR_INVALID_ARG&&audio_engine_set_speed(126)==ESP_ERR_INVALID_ARG);
    assert(audio_engine_set_speed(75)==ESP_OK&&snapshot().speed_percent==75);
    assert(audio_engine_set_speed(100)==ESP_OK);
    assert(audio_engine_set_volume(40,false)==ESP_OK);
    assert(audio_engine_set_volume(101,false)==ESP_ERR_INVALID_ARG);
    assert(audio_engine_set_effect(4)==ESP_ERR_INVALID_ARG);
    assert(audio_engine_set_effect(AUDIO_EFFECT_SPACE)==ESP_OK && snapshot().effect==AUDIO_EFFECT_SPACE);
    assert(audio_engine_set_effect(AUDIO_EFFECT_NORMAL)==ESP_OK);
    /* Native Next/Previous must load while paused without a sound burst. */
    assert(audio_engine_play_paused(argv[1],true)==ESP_OK);wait_state(AUDIO_PAUSED,2000);Sleep(100);
    assert(snapshot().seek_supported && snapshot().event==AUDIO_EVENT_LOADED);
    assert(InterlockedCompareExchange(&writes,0,0)==0);
    assert(audio_engine_pause(false)==ESP_OK);wait_state(AUDIO_PLAYING,2000);
    assert(audio_engine_stop_and_wait(2000)==ESP_OK);
    /* An old STOPPED snapshot must not acknowledge a later queued stop. */
    assert(audio_engine_play(argv[1])==ESP_OK);
    assert(audio_engine_stop_and_wait(2000)==ESP_OK);
    assert(snapshot().state==AUDIO_STOPPED && !snapshot().seek_supported);
    assert(audio_engine_play(argv[1])==ESP_OK);wait_state(AUDIO_PLAYING,2000);
    Sleep(150);audio_snapshot_t a=snapshot();float peak=0;
    for(unsigned i=0;i<AUDIO_SPECTRUM_BANDS;i++)if(a.spectrum[i]>peak)peak=a.spectrum[i];
    assert(a.format==AUDIO_FORMAT_MP3 && a.sample_rate==44100 && peak>0.3f && a.seek_supported);
    assert(audio_engine_pause(true)==ESP_OK);wait_state(AUDIO_PAUSED,2000);Sleep(50);
    LONG before=InterlockedCompareExchange(&writes,0,0);Sleep(100);assert(before==InterlockedCompareExchange(&writes,0,0));
    assert(audio_engine_seek_ms(1400)==ESP_OK);Sleep(50);
    /* A paused seek reports its target at once: UI, repeated seeks and the
     * saved resume point read position_ms while no block is played. */
    a=snapshot();assert(a.state==AUDIO_PAUSED && a.position_ms==1400);
    assert(audio_engine_seek_ms(1700)==ESP_OK);Sleep(50);a=snapshot();assert(a.state==AUDIO_PAUSED && a.position_ms==1700);
    assert(audio_engine_seek_ms(1400)==ESP_OK);Sleep(50);
    assert(audio_engine_pause(false)==ESP_OK);a=wait_position(1400,1600);
    assert(audio_engine_seek_ms(200)==ESP_OK);Sleep(100);a=snapshot();assert(a.position_ms>=200 && a.position_ms<500);
    uint32_t gaps=a.buffer_starvations;
    host_audio_delay_next_decode(450);Sleep(550);
    a=snapshot();assert(a.buffer_starvations==gaps+1 && a.max_read_us>0 && a.max_decode_us>0);
    a=wait_state(AUDIO_ENDED,4000);assert(a.position_ms>=2000 && a.position_ms<2200 && a.event==AUDIO_EVENT_ENDED);
    uint32_t seq=a.event_sequence;Sleep(50);assert(snapshot().event_sequence==seq);
    /* Screen off suspends FFT, while decoded PCM and playback continue. */
    audio_engine_set_visuals_enabled(false);assert(audio_engine_play(argv[1])==ESP_OK);wait_state(AUDIO_PLAYING,2000);Sleep(100);
    a=snapshot();assert(a.position_ms>0);for(unsigned i=0;i<AUDIO_SPECTRUM_BANDS;i++)assert(a.spectrum[i]==0);
    audio_engine_set_visuals_enabled(true);assert(audio_engine_set_volume(0,true)==ESP_OK);Sleep(100);a=snapshot();peak=0;
    for(unsigned i=0;i<AUDIO_SPECTRUM_BANDS;i++)if(a.spectrum[i]>peak)peak=a.spectrum[i];assert(peak>.3f); /* independent of volume/mute */
    assert(audio_engine_set_volume(40,false)==ESP_OK);assert(audio_engine_stop_and_wait(2000)==ESP_OK);

    assert(audio_engine_play(argv[2])==ESP_OK);a=wait_state(AUDIO_PLAYING,2000);assert(a.format==AUDIO_FORMAT_WAV && a.bits_per_sample==24 && a.channels==1);
    assert(audio_engine_seek_ms(700)==ESP_OK);a=wait_position(700,900);
    assert(audio_engine_stop()==ESP_OK);wait_state(AUDIO_STOPPED,2000);Sleep(50);
    before=InterlockedCompareExchange(&writes,0,0);Sleep(100);assert(before==InterlockedCompareExchange(&writes,0,0));
    assert(audio_engine_play(argv[1])==ESP_OK);wait_state(AUDIO_PLAYING,2000);
    InterlockedExchange(&fail_output,1);a=wait_state(AUDIO_ERROR,2000);assert(a.output_errors==1 && a.error[0]);
    InterlockedExchange(&fail_output,0);assert(audio_engine_play(argv[2])==ESP_OK);wait_state(AUDIO_PLAYING,2000);
    assert(audio_engine_play("missing-file.mp3")==ESP_OK);a=wait_state(AUDIO_ERROR,2000);assert(a.error[0]);
    /* Less than the four-block prebuffer still completes; EOF releases it. */
    char temp_dir[MAX_PATH],short_path[MAX_PATH];
    assert(GetTempPathA(sizeof temp_dir,temp_dir));assert(GetTempFileNameA(temp_dir,"t5a",0,short_path));
    FILE *tiny=fopen(short_path,"wb");assert(tiny);
    const uint8_t one_sample[46]={
        'R','I','F','F',38,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
        1,0,1,0,0x80,0xbb,0,0,0,0x77,1,0,2,0,16,0,'d','a','t','a',2,0,0,0,0,0
    };
    assert(fwrite(one_sample,1,sizeof one_sample,tiny)==sizeof one_sample);fclose(tiny);
    assert(audio_engine_play(short_path)==ESP_OK);wait_state(AUDIO_ENDED,2000);
    assert(audio_engine_stop_and_wait(2000)==ESP_OK);assert(DeleteFileA(short_path));
    puts("audio_engine tests: MP3/WAV, FFT, pause/seek/EOF/stop/error recovery, short-file prebuffer and starvation metrics passed");
    return 0;
}
