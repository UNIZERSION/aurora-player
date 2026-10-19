#include "audio_eq.h"
#include <math.h>
#include <string.h>

#define EQ_PI 3.14159265358979323846
#define EQ_LIMIT 32700.0f

const uint16_t audio_eq_frequencies[AUDIO_EQ_BANDS]={31,63,125,250,500,1000,2000,4000,8000,16000,20000};

bool audio_eq_valid_config(const audio_eq_config_t *c)
{
    if(!c || c->enabled>1 || c->preamp_db>0 || c->preamp_db<-24)return false;
    for(unsigned i=0;i<AUDIO_EQ_BANDS;++i)if(c->gain_db[i]<-12 || c->gain_db[i]>12)return false;
    return true;
}

bool audio_eq_band_available(uint32_t rate,unsigned band)
{
    return rate>=8000 && rate<=96000 && band<AUDIO_EQ_BANDS &&
        (uint32_t)audio_eq_frequencies[band]*1000U<rate*475U;
}

void audio_eq_reset(audio_eq_t *e,uint32_t rate)
{
    if(!e)return;
    memset(e,0,sizeof(*e));
    e->rate=rate>=8000 && rate<=96000?rate:44100;
    e->transition_frames=e->rate/50; /* 20 ms; never shorter due to block size. */
    e->hold_frames=e->rate/20;
    e->release=1.0f-expf(-1.0f/(0.250f*e->rate));
    e->current.identity=e->previous.identity=1;
    e->current.input_gain=e->previous.input_gain=1.0f;
    e->limiter_gain=e->reduction_gain=1.0f;
}

/* RBJ peaking EQ, Q=sqrt(2) (approximately one octave at modest gains).
 * Coefficient design uses double to avoid cancellation at 31 Hz / 96 kHz;
 * sample processing and state remain float on the P4 FPU.
 * https://www.w3.org/TR/audio-eq-cookbook/#peaking-eqn */
static void filter_configure(audio_eq_filter_t *f,uint32_t rate,unsigned band,int gain)
{
    const double a=pow(10.0,gain/40.0);
    const double w=2.0*EQ_PI*audio_eq_frequencies[band]/rate;
    const double sine=sin(w),cosine=cos(w);
    const double alpha=sine/(2.0*1.4142135623730951);
    const double a0=1.0+alpha/a;
    f->b0=(float)((1.0+alpha*a)/a0);
    f->b1=(float)(-2.0*cosine/a0);
    f->b2=(float)((1.0-alpha*a)/a0);
    f->a1=f->b1;
    f->a2=(float)((1.0-alpha/a)/a0);
    /* Algebraically equivalent trapezoidal state-variable realization. It
     * avoids subtracting nearly equal full-scale feedback samples at31 Hz.
     * Five multiplies/channel/section, like the direct-form biquad.
     * Derivation: cytomic.com/files/dsp/SvfLinearTrapOptimised2.pdf */
    const double g=sine/(1.0+cosine),k=1.0/(1.4142135623730951*a);
    const double t=1.0/(1.0+g*(g+k));
    f->t0=(float)t;f->t1=(float)(g*t);f->t2=(float)(g*g*t);
    f->mix=(float)(k*(a*a-1.0));
}

static void bank_configure(audio_eq_t *e,audio_eq_bank_t *b,const audio_eq_config_t *c)
{
    memset(b,0,sizeof(*b));b->config=*c;b->input_gain=1.0f;
    if(!c->enabled){b->identity=1;return;}
    for(unsigned i=0;i<AUDIO_EQ_BANDS;++i)
        if(c->gain_db[i] && audio_eq_band_available(e->rate,i))
            filter_configure(&b->filters[b->count++],e->rate,i,c->gain_db[i]);
    if(!b->count && !c->preamp_db){b->identity=1;return;}
    /* Manual preamp is the only static level adjustment. A bass boost must
     * not turn down unrelated frequencies when the signal has enough room. */
    b->input_gain=(float)pow(10.0,(double)c->preamp_db/20.0);
}

void audio_eq_reset_config(audio_eq_t *e,uint32_t rate,const audio_eq_config_t *config)
{
    if(!e)return;
    audio_eq_reset(e,rate);
    if(audio_eq_valid_config(config)){
        bank_configure(e,&e->current,config);
        e->previous=e->current;
    }
}

float audio_eq_reduction_db(const audio_eq_t *e)
{
    return !e || e->reduction_gain>=1.0f?0.0f:
        -20.0f*log10f(fmaxf(e->reduction_gain,1e-12f));
}

static void transition_begin(audio_eq_t *e,const audio_eq_config_t *c)
{
    e->previous=e->current;
    bank_configure(e,&e->current,c);
    e->transition=e->transition_frames;
}

static void bank_process(audio_eq_bank_t *b,const float in[2],float out[2])
{
    out[0]=in[0];out[1]=in[1];
    if(b->identity)return;
    out[0]*=b->input_gain;out[1]*=b->input_gain;
    for(unsigned i=0;i<b->count;++i){
        audio_eq_filter_t *f=&b->filters[i];
        for(unsigned ch=0;ch<2;++ch){
            const float x=out[ch],delta=x-f->z2[ch];
            const float band=f->t0*f->z1[ch]+f->t1*delta;
            const float low=f->z2[ch]+f->t1*f->z1[ch]+f->t2*delta;
            f->z1[ch]=2.0f*band-f->z1[ch];
            f->z2[ch]=2.0f*low-f->z2[ch];
            out[ch]=x+f->mix*band;
        }
    }
}

static void limit_output(audio_eq_t *e,float out[2])
{
    /* PCM16 is asymmetric: legitimate -32768 and +32767 are both unity.
     * Trigger only on actual mixed-output overload, with a small safety
     * margin after triggering. The two channels always share gain. */
    const float peak=fmaxf(fmaxf(out[0],out[1]),-fminf(out[0],out[1])*(32767.0f/32768.0f));
    const float needed=peak>32767.0f?EQ_LIMIT/peak:1.0f;
    if(needed<e->limiter_gain){
        e->limiter_gain=needed;e->limiter_deficit=1.0f-needed;e->hold=e->hold_frames;
    }
    else if(needed<1.0f && needed<e->limiter_gain*1.001f)e->hold=e->hold_frames;
    else if(e->hold)--e->hold;
    else{
        /* Decaying the deficit independently avoids float cancellation
         * stalling recovery below unity, particularly at96 kHz. */
        e->limiter_deficit*=1.0f-e->release;
        e->limiter_gain=1.0f-e->limiter_deficit;
        if(e->limiter_gain>needed){e->limiter_gain=needed;e->limiter_deficit=1.0f-needed;}
        if(e->limiter_deficit<1.0f/65536.0f && needed==1.0f){
            e->limiter_deficit=0;e->limiter_gain=1.0f;
        }
    }
    out[0]*=e->limiter_gain;out[1]*=e->limiter_gain;
}

static int16_t quantize(float value)
{
    if(value>32767.0f)value=32767.0f;
    if(value<-32768.0f)value=-32768.0f;
    return (int16_t)lrintf(value);
}

void audio_eq_process(audio_eq_t *e,int16_t *pcm,size_t frames,const audio_eq_config_t *config)
{
    if(!e || !pcm || !frames)return;
    audio_eq_config_t wanted={0};
    if(audio_eq_valid_config(config) && config->enabled)wanted=*config;
    bool pending=memcmp(&wanted,&e->current.config,sizeof wanted)!=0;
    if(!e->transition && pending){transition_begin(e,&wanted);pending=false;}
    if(e->current.identity && !e->transition && e->limiter_gain==1.0f){e->reduction_gain=1.0f;return;}
    float least_gain=1.0f;
    for(size_t i=0;i<frames;++i){
        /* A superseding setting starts exactly when the preceding fade ends,
         * even inside a decoder block. Latest request wins without restarting
         * a live fade, so block partitioning does not alter the result. */
        if(!e->transition && pending){transition_begin(e,&wanted);pending=false;}
        const float in[2]={pcm[2*i],pcm[2*i+1]};float out[2];
        bank_process(&e->current,in,out);
        if(e->transition){
            float old[2];bank_process(&e->previous,in,old);
            const float mix=(float)(e->transition_frames-e->transition+1)/e->transition_frames;
            out[0]=old[0]+(out[0]-old[0])*mix;out[1]=old[1]+(out[1]-old[1])*mix;
            --e->transition;
        }
        /* One persistent limiter follows the mixed signal. Setting changes
         * cannot reset its envelope or limit an inaudible destination bank. */
        limit_output(e,out);
        if(e->limiter_gain<least_gain)least_gain=e->limiter_gain;
        pcm[2*i]=quantize(out[0]);pcm[2*i+1]=quantize(out[1]);
    }
    e->reduction_gain=least_gain;
}
