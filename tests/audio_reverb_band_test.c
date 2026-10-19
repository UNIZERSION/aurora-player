#undef NDEBUG
#include "../main/audio_fx.c"
#include <assert.h>
#include <stdio.h>

static audio_fx_t fx;
static double filter_rms(unsigned rate,unsigned hz,unsigned cut,bool highpass)
{
    float state[2]={0};const float alpha=reverb_band_alpha(cut,rate);
    double sum=0;
    for(unsigned n=0;n<rate*2;n++){
        float value=sinf(2.f*PI*hz*n/rate);
        for(unsigned stage=0;stage<(highpass?2u:1u);stage++){
            const float low=reverb_lowpass(value,alpha,&state[stage]);
            value=highpass?value-low:low;
        }
        if(n>=rate)sum+=(double)value*value;
    }
    return sqrt(sum/rate);
}
static double actual_wet_rms(unsigned rate,unsigned hz,unsigned lower,unsigned upper)
{
    audio_reverb_config_t c=audio_reverb_default_config();c.tone_hz=upper;c.low_cut_hz=lower;
    audio_fx_reset_config(&fx,rate,AUDIO_EFFECT_REVERB,&c);
    double sum=0;
    for(unsigned n=0;n<rate*3;n++){
        const float in[2]={2000.f*sinf(2.f*PI*hz*n/rate),1500.f*cosf(2.f*PI*hz*n/rate)};float out[2];
        process_voice(&fx,&fx.current,in,out);
        for(unsigned ch=0;ch<2;ch++){
            /* This identity checks the actual dry contribution at every
             * frequency and cutoff, including any wet peak protection. */
            assert(fabsf(out[ch]-(.61f*in[ch]+.38f*fx.current.gain*fx.current.reverb_wet[ch]))<.01f);
            assert(isfinite(out[ch]));if(n>=rate*2)sum+=(double)fx.current.reverb_wet[ch]*fx.current.reverb_wet[ch];
        }
    }
    return sqrt(sum/(2*rate));
}
static void filter_response(void)
{
    const unsigned rates[]={8000,11025,16000,22050,32000,44100,48000,96000};
    for(unsigned i=0;i<sizeof rates/sizeof *rates;i++){
        const unsigned rate=rates[i];
        const double lp=filter_rms(rate,1000,1000,false),hp=filter_rms(rate,1000,1000,true);
        assert(fabs(20*log10(lp*sqrt(2.))+3.0103)<.01);
        assert(fabs(20*log10(hp*sqrt(2.))+6.0206)<.01);
    }
    const double h20=filter_rms(48000,20000,20000,false);
    assert(fabs(20*log10(h20*sqrt(2.))+3.0103)<.02);
    printf("PASS independent analytic response: high cut -3.0103dB and two-pole low cut -6.0206dB at1k across8 rates; genuine20kHz cutoff %.4fdB at48k.\n",20*log10(h20*sqrt(2.)));
}
static void actual_band(void)
{
    for(unsigned rate=44100;rate<=48000;rate+=3900){
        const double open=actual_wet_rms(rate,63,0,20000),cut=actual_wet_rms(rate,63,1000,20000);
        const double wide=actual_wet_rms(rate,10000,0,20000),warm=actual_wet_rms(rate,10000,0,1400);
        const double upper=actual_wet_rms(rate,18000,0,20000);
        assert(20*log10(cut/open)<-40);assert(20*log10(wide/warm)>15);assert(upper>100);
        printf("PASS actual wet path rate%u: Low cut1k reduces63Hz %.2fdB; wideningHigh cut1.4k->20k increases10k %.2fdB;18k RMS%.2fPCM with2000/1500PCM excitation; dry gain unchanged.\n",rate,20*log10(cut/open),20*log10(wide/warm),upper);
    }
}
static void nyquist_and_bypass(void)
{
    unsigned largest=0;
    for(unsigned rate=8000;rate<=96000;rate++){
        audio_reverb_config_t c={.decay_ms=4000,.tone_hz=20000,.amount=60,.low_cut_hz=19900};
        audio_fx_reset_config(&fx,rate,AUDIO_EFFECT_REVERB,&c);
        const audio_fx_voice_t *v=&fx.current;
        assert(v->reverb_end[3]<=AUDIO_FX_REVERB_FRAMES);if(v->reverb_end[3]>largest)largest=v->reverb_end[3];
        assert(v->reverb_low_alpha>0&&v->reverb_low_alpha<v->reverb_high_alpha&&v->reverb_high_alpha<1);
        assert((float)rate/v->reverb_divisor<=48000);
    }
    audio_reverb_config_t c=audio_reverb_default_config();c.tone_hz=20000;c.low_cut_hz=1000;
    audio_fx_reset_config(&fx,48000,2,&c);
    for(unsigned n=0;n<48000;n++){const float in[]={10000,7000};float out[2];process_voice(&fx,&fx.current,in,out);}
    c.low_cut_hz=0;assert(audio_fx_set_reverb_config(&fx,&c));
    for(unsigned n=0;n<48000*4;n++){const float in[]={10000,7000};float out[2];process_voice(&fx,&fx.current,in,out);}
    assert(fx.current.reverb_low_mix==0);
    /* Bypassed low filter does not retain/subtract a hidden DC component. */
    assert(fabsf(fx.current.reverb_wet[0])>1000);
    printf("PASS all88001 integer rates8..96k: finite ordered Nyquist-safe cutoffs, packed delay max%u/%u; Low cut0 settles to exact bypass, including held DC.\n",largest,AUDIO_FX_REVERB_FRAMES);
}
int main(void){setvbuf(stdout,NULL,_IONBF,0);filter_response();actual_band();nyquist_and_bypass();return 0;}
