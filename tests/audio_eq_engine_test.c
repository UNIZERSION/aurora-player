#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "audio_engine.h"
#include "audio_level_policy.h"

#define RATE 48000u
#define START (RATE/4)
#define END (RATE*3/4)
#define FIRST 64
#define TAU 6.283185307179586
/* Reads the real production factory table through a C++ test-only bridge. */
extern unsigned host_eq_factory_count(void);
extern void host_eq_factory_config(unsigned index,audio_eq_config_t *config);
typedef struct { double rms,low,high;uint64_t hash;float reduction;int peak;int16_t first[FIRST]; } result_t;
static CRITICAL_SECTION capture_lock;
static unsigned sample_index,captured,generation;
static double energy,low_sin,low_cos,high_sin,high_cos;
static uint64_t pcm_hash;
static int peak;
static int16_t first_pcm[FIRST];
static esp_err_t configure(void *user,uint32_t rate)
{
    (void)user;assert(rate==RATE);EnterCriticalSection(&capture_lock);
    ++generation;sample_index=captured=0;energy=low_sin=low_cos=high_sin=high_cos=0;
    peak=0;pcm_hash=1469598103934665603ULL;memset(first_pcm,0,sizeof first_pcm);
    LeaveCriticalSection(&capture_lock);return ESP_OK;
}
static esp_err_t output(void *user,const int16_t *pcm,size_t frames)
{
    (void)user;EnterCriticalSection(&capture_lock);
    for(size_t i=0;i<frames;i++,sample_index++){
        const int x=pcm[i*2];if(sample_index<FIRST)first_pcm[sample_index]=(int16_t)x;
        if(sample_index>=START && sample_index<END){
            energy+=(double)x*x;++captured;
            low_sin+=x*sin(TAU*63*sample_index/RATE);low_cos+=x*cos(TAU*63*sample_index/RATE);
            high_sin+=x*sin(TAU*4000*sample_index/RATE);high_cos+=x*cos(TAU*4000*sample_index/RATE);
            for(unsigned ch=0;ch<2;ch++){uint16_t bits=(uint16_t)pcm[i*2+ch];pcm_hash^=bits;pcm_hash*=1099511628211ULL;}
            int magnitude=x<0?-x:x;if(magnitude>peak)peak=magnitude;
        }
    }
    LeaveCriticalSection(&capture_lock);Sleep((DWORD)(frames*1000/RATE));return ESP_OK;
}
static esp_err_t volume(void *user,uint8_t level,bool muted){(void)user;(void)level;(void)muted;return ESP_OK;}
static void le16(FILE *f,unsigned n){fputc(n&255,f);fputc((n>>8)&255,f);}
static void le32(FILE *f,unsigned n){le16(f,n);le16(f,n>>16);}
static void fixture(const char *path,double hz,double amplitude,double high_amplitude)
{
    FILE *f=fopen(path,"wb");assert(f);fwrite("RIFF",1,4,f);le32(f,36+RATE*8);fwrite("WAVEfmt ",1,8,f);
    le32(f,16);le16(f,1);le16(f,2);le32(f,RATE);le32(f,RATE*4);le16(f,4);le16(f,16);
    fwrite("data",1,4,f);le32(f,RATE*8);
    for(unsigned i=0;i<RATE*2;i++){
        double value=amplitude*(hz?sin(TAU*hz*i/RATE):1)+high_amplitude*sin(TAU*4000*i/RATE);
        assert(value>=-32768 && value<=32767);int16_t x=(int16_t)lrint(value);le16(f,(uint16_t)x);le16(f,(uint16_t)x);
    }
    assert(!ferror(f));assert(fclose(f)==0);
}
static result_t await_capture(unsigned after_generation)
{
    audio_snapshot_t a={0};ULONGLONG begin=GetTickCount64();bool ready=false;
    do{
        audio_engine_get_snapshot(&a);assert(a.state!=AUDIO_ERROR && a.output_errors==0);
        EnterCriticalSection(&capture_lock);ready=generation>after_generation && captured==END-START;LeaveCriticalSection(&capture_lock);
        if(ready)break;Sleep(5);
    }while(GetTickCount64()-begin<4000);
    assert(ready);result_t r={0};r.reduction=a.eq_reduction_db;assert(isfinite(r.reduction));
    EnterCriticalSection(&capture_lock);r.rms=sqrt(energy/captured);r.low=2*hypot(low_sin,low_cos)/captured;r.high=2*hypot(high_sin,high_cos)/captured;
    r.hash=pcm_hash;r.peak=peak;memcpy(r.first,first_pcm,sizeof r.first);LeaveCriticalSection(&capture_lock);return r;
}
static result_t measure(const char *path,double hz,double amplitude,double high,unsigned effect,audio_eq_config_t config)
{
    assert(audio_engine_stop_and_wait(2000)==ESP_OK);fixture(path,hz,amplitude,high);
    assert(audio_engine_set_eq(&config)==ESP_OK);assert(audio_engine_set_effect(effect)==ESP_OK);
    EnterCriticalSection(&capture_lock);unsigned before=generation;LeaveCriticalSection(&capture_lock);
    assert(audio_engine_play(path)==ESP_OK);result_t r=await_capture(before);
    assert(audio_engine_stop_and_wait(2000)==ESP_OK);return r;
}
static double db(double value,double reference){assert(value>0 && reference>0);return 20*log10(value/reference);}
static void equal_pcm(result_t a,result_t b){assert(a.hash==b.hash && a.rms==b.rms && a.reduction==b.reduction);}
static uint64_t source_hash(double hz,double amplitude,double high)
{
    uint64_t hash=1469598103934665603ULL;
    for(unsigned i=START;i<END;i++){
        const int16_t sample=(int16_t)lrint(amplitude*(hz?sin(TAU*hz*i/RATE):1)+high*sin(TAU*4000*i/RATE));
        for(unsigned ch=0;ch<2;ch++){hash^=(uint16_t)sample;hash*=1099511628211ULL;}
    }
    return hash;
}
static void first_has_policy(result_t r)
{
    const int expected=(int)lrint(12000*pow(10.,AUDIO_MASTER_GAIN_DB/20.));
    for(unsigned i=0;i<FIRST;i++)assert(r.first[i]==expected);
    assert(fabs(db(r.rms,12000)-AUDIO_MASTER_GAIN_DB)<.002 && r.reduction==0);
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);InitializeCriticalSection(&capture_lock);
    audio_output_t out={.configure=configure,.write=output,.volume=volume};assert(audio_engine_init(&out)==ESP_OK);
    audio_engine_set_visuals_enabled(false);
    char dir[MAX_PATH],path[MAX_PATH];assert(GetTempPathA(sizeof dir,dir));assert(GetTempFileNameA(dir,"t5e",0,path));
    audio_eq_config_t flat={.enabled=1},bass=flat;bass.gain_db[1]=6;
    result_t reference=measure(path,4000,12000,0,0,flat);
    assert(fabs(db(reference.rms,12000/sqrt(2.))-AUDIO_MASTER_GAIN_DB)<.005);
    if(AUDIO_MASTER_GAIN_DB==0)assert(reference.hash==source_hash(4000,12000,0));
    for(unsigned mode=0;mode<3;mode++)for(int legacy=0;legacy>=-24;legacy-=24){
        audio_eq_config_t c=flat;c.preamp_db=(int8_t)legacy;
        unsigned effect=mode?3:0;if(mode==2){c=bass;c.enabled=0;c.preamp_db=(int8_t)legacy;}
        result_t r=measure(path,4000,12000,0,effect,c);equal_pcm(reference,r);
        first_has_policy(measure(path,0,12000,0,effect,c));
    }
    printf("PASS: Normal, flat EQ and EQ OFF share exactly the same %+d dB PCM with legacy preamp 0/-24; unity PCM is sample-exact with source, first 64 new-track samples have policy gain, no startup ramp or double attenuation.\n",AUDIO_MASTER_GAIN_DB);
    result_t low_flat=measure(path,63,12000,0,3,flat),low_boost=measure(path,63,12000,0,3,bass);
    result_t high_boost=measure(path,4000,12000,0,3,bass);
    double bass_db=db(low_boost.rms,low_flat.rms),high_db=db(high_boost.rms,reference.rms);
    assert(fabs(bass_db-6)<.03 && fabs(high_db)<.03 && low_boost.reduction==0 && high_boost.reduction==0);
    bass.preamp_db=-24;equal_pcm(low_boost,measure(path,63,12000,0,3,bass));
    result_t mixed_flat=measure(path,63,4000,4000,0,flat),mixed_boost=measure(path,63,4000,4000,3,bass);
    assert(fabs(db(mixed_boost.low,mixed_flat.low)-6)<.04 && fabs(db(mixed_boost.high,mixed_flat.high))<.04 && mixed_boost.reduction==0);
    printf("PASS: custom +6 dB bass = %+.4f dB; high band %+.4f dB; quiet two-tone high %+.4f dB with LIMIT 0; legacy preamp ignored.\n",bass_db,high_db,db(mixed_boost.high,mixed_flat.high));
    result_t bass_plus=measure(path,63,4000,4000,1,bass);
    assert(db(bass_plus.low,mixed_flat.low)>3 && fabs(db(bass_plus.high,mixed_flat.high))<.08 && bass_plus.reduction==0);
    bass.preamp_db=0;equal_pcm(bass_plus,measure(path,63,4000,4000,1,bass));
    result_t verb=measure(path,63,4000,4000,2,bass);bass.preamp_db=-24;equal_pcm(verb,measure(path,63,4000,4000,2,bass));
    assert(verb.reduction==0 && verb.hash!=mixed_flat.hash);
    printf("PASS: Bass+ quiet mixed high %+.4f dB with LIMIT 0; Reverb changes output PCM; both ignore legacy per-preset preamp.\n",db(bass_plus.high,mixed_flat.high));
    assert(host_eq_factory_count()==8);
    for(unsigned i=0;i<host_eq_factory_count();i++){
        audio_eq_config_t c;host_eq_factory_config(i,&c);c.preamp_db=0;
        result_t a=measure(path,63,8000,8000,3,c);c.preamp_db=-24;
        result_t b=measure(path,63,8000,8000,3,c);equal_pcm(a,b);assert(a.reduction==0);
        if(i==0)equal_pcm(a,measure(path,63,8000,8000,0,c));
    }
    printf("PASS: all eight actual production factory curves use the %+d dB base independent of saved 0/-24 preamp; factory Flat equals Normal PCM.\n",AUDIO_MASTER_GAIN_DB);
    if(AUDIO_MASTER_GAIN_DB==0){
        result_t hot_flat=measure(path,63,14000,14000,0,flat),hot_bass=measure(path,63,14000,14000,3,bass);
        assert(hot_bass.reduction>1 && db(hot_bass.high,hot_flat.high)<-1);
        printf("PASS: hot bass boost correctly reports LIMIT %.3f dB and high-band %+.3f dB at actual overload; unity does not promise unlimited headroom.\n",hot_bass.reduction,db(hot_bass.high,hot_flat.high));
    }
    audio_eq_config_t hot=flat;for(unsigned i=0;i<AUDIO_EQ_BANDS;i++)hot.gain_db[i]=12;
    result_t limited=measure(path,1000,30000,0,3,hot);assert(limited.reduction>1 && limited.peak<=32767);
    printf("PASS: extreme overlapping boosts report real overload (LIMIT %.3f dB), bounded PCM output.\n",limited.reduction);
    fixture(path,0,12000,0);assert(audio_engine_set_eq(&flat)==ESP_OK);assert(audio_engine_set_effect(3)==ESP_OK);
    EnterCriticalSection(&capture_lock);unsigned before=generation;LeaveCriticalSection(&capture_lock);
    assert(audio_engine_play(path)==ESP_OK);first_has_policy(await_capture(before));
    EnterCriticalSection(&capture_lock);before=generation;LeaveCriticalSection(&capture_lock);
    assert(audio_engine_seek_ms(1000)==ESP_OK);first_has_policy(await_capture(before));
    assert(audio_engine_stop_and_wait(2000)==ESP_OK);assert(DeleteFileA(path));
    printf("PASS: seek creates fresh configured DSP state with %+d dB first-sample level and no accumulated attenuation. Host I2S/RTOS timing is simulated.\n",AUDIO_MASTER_GAIN_DB);
    return 0;
}
