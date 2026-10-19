#include "audio_fx.h"
#include <math.h>
#include <string.h>
#define PI 3.14159265358979323846f

static uint8_t valid_effect(uint8_t effect)
{
    return effect<=AUDIO_EFFECT_RETRO?effect:AUDIO_EFFECT_NORMAL;
}
audio_reverb_config_t audio_reverb_default_config(void)
{
    return (audio_reverb_config_t){.decay_ms=1500,.tone_hz=1400,.amount=38,.low_cut_hz=0};
}
bool audio_reverb_valid_config(const audio_reverb_config_t *c)
{
    return c && c->decay_ms>=500 && c->decay_ms<=4000 &&
           c->tone_hz>=100 && c->tone_hz<=20000 && c->low_cut_hz+100<=c->tone_hz && c->amount<=60;
}
/* Topology-preserving one-pole coefficient. Bounding below Nyquist keeps
 * both the integrator and coefficient interpolation strictly stable. */
static float reverb_band_alpha(float hz,float rate)
{
    const float g=tanf(PI*fminf(hz,.45f*rate)/rate);
    return g/(1.f+g);
}
static void reverb_targets(audio_fx_voice_t *v,uint32_t rate,const audio_reverb_config_t *c,bool immediate)
{
    if(v->effect!=AUDIO_EFFECT_REVERB)return;
    const float wet_rate=(float)rate/v->reverb_divisor;
    for(unsigned i=0;i<AUDIO_FX_REVERB_TAPS;++i){
        const unsigned length=v->reverb_end[i]-v->reverb_start[i];
        v->reverb_feedback_target[i]=powf(10.f,-3000.f*length/(c->decay_ms*wet_rate));
        if(immediate)v->reverb_feedback[i]=v->reverb_feedback_target[i];
    }
    const float upper=fminf(c->tone_hz,wet_rate*.45f);
    const float lower=fminf(c->low_cut_hz,upper*.99f);
    v->reverb_damping_target=1-expf(-2*PI*upper/wet_rate);
    v->reverb_low_alpha_target=reverb_band_alpha(fmaxf(lower,1.f),rate);
    v->reverb_low_mix_target=c->low_cut_hz?1.f:0.f;
    v->reverb_high_alpha_target=reverb_band_alpha(upper,rate);
    v->reverb_amount_target=c->amount*.01f;
    if(immediate){
        v->reverb_damping=v->reverb_damping_target;v->reverb_amount=v->reverb_amount_target;
        v->reverb_low_alpha=v->reverb_low_alpha_target;v->reverb_low_mix=v->reverb_low_mix_target;
        v->reverb_high_alpha=v->reverb_high_alpha_target;
    }
}
static void configure(audio_fx_voice_t *f,uint32_t rate,uint8_t effect,const audio_reverb_config_t *config)
{
    memset(f,0,sizeof(*f));f->effect=effect;f->gain=1.0f;
    if(effect==AUDIO_EFFECT_REVERB) {
        /* Unequal short paths make a dense room; orthogonal cross-coupling
         * retains late energy rather than losing it in every mixing step.
         * Background: Julius O. Smith, Physical Audio Signal Processing,
         * https://www.dsprelated.com/freebooks/pasp/FDN_Reverberation.html */
        f->reverb_divisor=(rate+47999)/48000;
        const float wet_rate=(float)rate/f->reverb_divisor;
        f->reverb_step=1.0f/f->reverb_divisor;
        const uint32_t lengths_ms[4]={37,43,53,61};
        uint32_t offset=0;
        for(unsigned i=0;i<AUDIO_FX_REVERB_TAPS;++i){
            const uint32_t denominator=1000*f->reverb_divisor;
            uint32_t length=(rate*lengths_ms[i]+denominator/2)/denominator;
            length|=1; /* Avoid common even divisors when changing rates. */
            f->reverb_start[i]=f->reverb_position[i]=offset;
            offset+=length;f->reverb_end[i]=offset;
        }
        /* ~50 ms coefficient slewing preserves the existing delay energy.
         * Feedback gains stay strictly below one throughout interpolation. */
        f->reverb_smooth=1-expf(-1.f/(.050f*wet_rate));
        f->reverb_mix_smooth=1-expf(-1.f/(.050f*rate));
        reverb_targets(f,rate,config,true);
        /* Native-rate wet processing through48k removes the former5kHz
         * bottleneck. Higher source rates use anti-alias/reconstruction
         * poles around decimation; dry PCM always stays at source rate. */
        f->reverb_input_alpha=1-expf(-2*PI*(wet_rate*.4f)/rate);
        f->reverb_output_alpha=f->reverb_input_alpha;
    } else if(effect==AUDIO_EFFECT_BASS_PLUS) {
        /* RBJ low shelf: +6 dB below 120 Hz, unity well above it.
         * Headroom is managed after the linear filter, never by a waveshaper. */
        const float a=powf(10.0f,6.0f/40.0f),w=2*PI*120/rate;
        const float c=cosf(w),beta=sqrtf(2*a)*sinf(w);
        const float a0=(a+1)+(a-1)*c+beta;
        f->b0=a*((a+1)-(a-1)*c+beta)/a0;
        f->b1=2*a*((a-1)-(a+1)*c)/a0;
        f->b2=a*((a+1)-(a-1)*c-beta)/a0;
        f->a1=-2*((a-1)+(a+1)*c)/a0;
        f->a2=((a+1)+(a-1)*c-beta)/a0;
    } else if(effect==AUDIO_EFFECT_RETRO) {
        /* Warm tape-like bandwidth, without the old 10-bit quantization.
         * Each one-pole stage is a convex combination: bounded for arbitrary
         * PCM, including full-scale steps and polarity reversals. */
        f->lowpass=1-expf(-2*PI*fminf(6500.0f,rate*0.35f)/rate);
    }
}
void audio_fx_reset(audio_fx_t *fx,uint32_t rate,uint8_t effect)
{
    audio_fx_reset_config(fx,rate,effect,NULL);
}
void audio_fx_reset_config(audio_fx_t *fx,uint32_t rate,uint8_t effect,const audio_reverb_config_t *config)
{
    if(!fx)return;
    memset(fx,0,sizeof(*fx));
    fx->rate=rate>=8000 && rate<=96000?rate:44100;
    fx->transition_frames=fx->rate/100; /* 10 ms at every supported rate. */
    fx->hold_frames=fx->rate/20;
    fx->release=1-expf(-1.0f/(0.250f*fx->rate));
    fx->reverb_config=audio_reverb_valid_config(config)?*config:audio_reverb_default_config();
    configure(&fx->current,fx->rate,valid_effect(effect),&fx->reverb_config);
}
bool audio_fx_set_reverb_config(audio_fx_t *fx,const audio_reverb_config_t *c)
{
    if(!fx || !audio_reverb_valid_config(c))return false;
    if(fx->reverb_config.decay_ms==c->decay_ms && fx->reverb_config.tone_hz==c->tone_hz && fx->reverb_config.amount==c->amount && fx->reverb_config.low_cut_hz==c->low_cut_hz)return true;
    fx->reverb_config=*c;
    reverb_targets(&fx->current,fx->rate,c,false);
    reverb_targets(&fx->previous,fx->rate,c,false);
    return true;
}
static int16_t quantize(float sample)
{
    if(sample>32767)sample=32767;
    if(sample<-32768)sample=-32768;
    return (int16_t)lrintf(sample);
}
static void begin_transition(audio_fx_t *f,uint8_t effect)
{
    f->previous=f->current;
    configure(&f->current,f->rate,effect,&f->reverb_config);
    f->transition=f->transition_frames;
}
static float reverb_lowpass(float input,float alpha,float *state)
{
    const float delta=alpha*(input-*state),output=*state+delta;
    *state=output+delta;
    if(fabsf(*state)<1e-12f)*state=0;
    return output;
}
static void process_voice(audio_fx_t *f,audio_fx_voice_t *v,const float in[2],float out[2])
{
    out[0]=in[0];out[1]=in[1];
    if(v->effect==AUDIO_EFFECT_BASS_PLUS) {
        for(unsigned ch=0;ch<2;ch++) {
            out[ch]=v->b0*in[ch]+v->z1[ch];
            v->z1[ch]=v->b1*in[ch]-v->a1*out[ch]+v->z2[ch];
            v->z2[ch]=v->b2*in[ch]-v->a2*out[ch];
        }
        /* Stereo-linked, sample-peak gain control. The previous per-sample
         * soft clip flattened loud bass and generated strong harmonics.
         * A 50 ms hold spans bass cycles; a 250 ms release avoids pumping.
         * Unity gain when headroom permits; no fixed attenuation of mids.
         * This is not a true-peak/lookahead limiter: an unexpected overload
         * can still change gain immediately, but cannot clip PCM output. */
        const float peak=fmaxf(fabsf(out[0]),fabsf(out[1]));
        const float needed=peak>32700.0f?32700.0f/peak:1.0f;
        if(needed<v->gain) {
            v->gain=needed;v->hold=f->hold_frames;
        } else if(needed<1.0f && needed<v->gain*1.001f) {
            v->hold=f->hold_frames;
        } else if(v->hold) {
            --v->hold;
        } else {
            v->gain+=f->release*(1-v->gain);
            if(v->gain>needed)v->gain=needed;
        }
        out[0]*=v->gain;out[1]*=v->gain;
    } else if(v->effect==AUDIO_EFFECT_REVERB) {
        float filtered[2];
        for(unsigned ch=0;ch<2;++ch){
            float value=in[ch];
            for(unsigned stage=0;v->reverb_divisor>1 && stage<4;++stage){
                float *low=&v->reverb_input_low[ch][stage];
                *low+=v->reverb_input_alpha*(value-*low);
                if(fabsf(*low)<1e-12f)*low=0;
                value=*low;
            }
            filtered[ch]=value;
        }
        if(!v->reverb_phase){
            float tap[4];
            v->reverb_damping+=v->reverb_smooth*(v->reverb_damping_target-v->reverb_damping);
            for(unsigned i=0;i<AUDIO_FX_REVERB_TAPS;++i){
                v->reverb_feedback[i]+=v->reverb_smooth*(v->reverb_feedback_target[i]-v->reverb_feedback[i]);
                tap[i]=v->reverb_delay[v->reverb_position[i]];
                v->reverb_low[i]+=v->reverb_damping*(tap[i]-v->reverb_low[i]);
                if(fabsf(v->reverb_low[i])<1e-12f)v->reverb_low[i]=0;
                v->reverb_low2[i]+=v->reverb_damping*(v->reverb_low[i]-v->reverb_low2[i]);
                if(fabsf(v->reverb_low2[i])<1e-12f)v->reverb_low2[i]=0;
            }
            const float *l=v->reverb_low2;
            /* Q = J/2 - I is orthogonal. Gains strictly below1, together
             * with passive delay/filter paths, give an energy-stable loop.
             * This preserves energy, not an arbitrary-input peak bound;
             * resonant peaks are handled after wet reconstruction below. */
            const float sum=.5f*(l[0]+l[1]+l[2]+l[3]);
            const float inject[4]={filtered[0],filtered[1],-filtered[0],-filtered[1]};
            for(unsigned i=0;i<AUDIO_FX_REVERB_TAPS;++i){
                const float gain=v->reverb_feedback[i];
                float value=.50f*inject[i]+gain*(sum-l[i]);
                if(fabsf(value)<1e-12f)value=0; /* Avoid subnormal silent tails. */
                v->reverb_delay[v->reverb_position[i]]=value;
                if(++v->reverb_position[i]==v->reverb_end[i])v->reverb_position[i]=v->reverb_start[i];
            }
            for(unsigned ch=0;ch<2;++ch){
                v->reverb_previous[ch]=v->reverb_next[ch];
                v->reverb_next[ch]=.5f*(tap[ch]-tap[ch+2]);
            }
            v->reverb_phase=v->reverb_divisor;
        }
        const float mix=(v->reverb_divisor-v->reverb_phase+1)*v->reverb_step;
        --v->reverb_phase;
        /* Wide sweeps use ~200ms smoothing, including Low cut bypass.
         * This also bounds DC-state movement at the lowest source rates. */
        const float band_smooth=v->reverb_mix_smooth*.25f;
        v->reverb_low_alpha+=band_smooth*(v->reverb_low_alpha_target-v->reverb_low_alpha);
        v->reverb_high_alpha+=band_smooth*(v->reverb_high_alpha_target-v->reverb_high_alpha);
        v->reverb_low_mix+=band_smooth*(v->reverb_low_mix_target-v->reverb_low_mix);
        if(fabsf(v->reverb_low_mix_target-v->reverb_low_mix)<1e-5f)v->reverb_low_mix=v->reverb_low_mix_target;
        /* Dry remains independent of room feedback and wet peak protection. */
        float room[2];
        for(unsigned ch=0;ch<2;++ch){
            float wet=v->reverb_previous[ch]+mix*(v->reverb_next[ch]-v->reverb_previous[ch]);
            /* Three convex reconstruction poles suppress the quiet images
             * left by linear interpolation. They affect only the wet path. */
            for(unsigned stage=0;v->reverb_divisor>1 && stage<3;++stage){
                float *low=&v->reverb_output_low[ch][stage];
                *low+=v->reverb_output_alpha*(wet-*low);
                if(fabsf(*low)<1e-12f)*low=0;
                wet=*low;
            }
            /* A two-pole high-pass and one-pole low-pass limit only the
             * wet signal. The dry mix and master gain never depend on a
             * cutoff. A faded bypass makes Low cut0 truly full-band,
             * without stranding a DC state when the knob reaches zero. */
            for(unsigned stage=0;stage<2;++stage){
                const float low=reverb_lowpass(wet,v->reverb_low_alpha,&v->reverb_band_low[ch][stage]);
                wet-=v->reverb_low_mix*low;
            }
            wet=reverb_lowpass(wet,v->reverb_high_alpha,&v->reverb_band_high[ch]);
            room[ch]=wet;
            /* Final wet sample is also exposed to diagnostics/tests. */
            v->reverb_wet[ch]=wet;
        }
        /* Dense lossless diffusion can build resonant wet peaks. A linked
         * envelope gain protects only the room, leaving dry PCM unchanged.
         * A 50 ms hold spans bass cycles and 250 ms release avoids repeated
         * wave-top clipping. Independent injection preserves room level as
         * decay length increases. This is a sample-peak safety stage. */
        const float peak=fmaxf(fabsf(room[0]),fabsf(room[1]));
        const float needed=peak>32700.0f?32700.0f/peak:1.0f;
        if(needed<v->gain){v->gain=needed;v->hold=f->hold_frames;}
        else if(needed<1.0f&&needed<v->gain*1.001f)v->hold=f->hold_frames;
        else if(v->hold)--v->hold;
        else {v->gain+=f->release*(1-v->gain);if(v->gain>needed)v->gain=needed;}
        v->reverb_amount+=v->reverb_mix_smooth*(v->reverb_amount_target-v->reverb_amount);
        /* Preserve the released 61% dry + 38% wet default exactly. At zero
         * Amount the dry path is unity. The sum stays <=1 at every setting,
         * so the bounded wet path cannot drive full-scale dry into clipping. */
        const float wet=v->reverb_amount,dry=1.f-(39.f/38.f)*wet;
        out[0]=dry*in[0]+wet*v->gain*room[0];
        out[1]=dry*in[1]+wet*v->gain*room[1];
    } else if(v->effect==AUDIO_EFFECT_RETRO) {
        for(unsigned ch=0;ch<2;ch++) {
            v->low[ch]+=v->lowpass*(in[ch]-v->low[ch]);
            v->low2[ch]+=v->lowpass*(v->low[ch]-v->low2[ch]);
            out[ch]=0.995f*v->low2[ch];
        }
    }
}
void audio_fx_process(audio_fx_t *f,int16_t *pcm,size_t frames,uint8_t effect)
{
    if(!f || !pcm || !frames)return;
    effect=valid_effect(effect);
    /* Complete a live crossfade before applying another rapid request. The
     * caller supplies its latest selected effect each block, so no request
     * queue or discontinuous restart is needed. Maximum fade is 10 ms. */
    if(effect!=f->current.effect && !f->transition) {
        begin_transition(f,effect);
    }
    /* NORMAL is zero-latency, bit-exact once its 10 ms transition completes. */
    if(f->current.effect==AUDIO_EFFECT_NORMAL && !f->transition)return;
    for(size_t i=0;i<frames;i++) {
        /* A superseding request starts on the first sample after the previous
         * fade, independent of how the decoder partitions its PCM blocks. */
        if(!f->transition && effect!=f->current.effect)begin_transition(f,effect);
        const float in[2]={pcm[i*2],pcm[i*2+1]};float out[2];
        process_voice(f,&f->current,in,out);
        if(f->transition) {
            float old[2];process_voice(f,&f->previous,in,old);
            const float mix=(float)(f->transition_frames-f->transition+1)/f->transition_frames;
            for(unsigned ch=0;ch<2;ch++)out[ch]=old[ch]+(out[ch]-old[ch])*mix;
            --f->transition;
        }
        pcm[i*2]=quantize(out[0]);pcm[i*2+1]=quantize(out[1]);
    }
}
