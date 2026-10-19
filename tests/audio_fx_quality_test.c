#ifdef NDEBUG
#undef NDEBUG
#endif
#include "audio_fx.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TAU 6.28318530717958647692
static double residual(unsigned effect,unsigned rate,double hz,double amplitude)
{
    audio_fx_t f;audio_fx_reset(&f,rate,(uint8_t)effect);
    double energy=0,si=0,co=0,dc=0;int16_t samples[256*2];
    for(unsigned first=0;first<rate*2;first+=256) {
        unsigned n=rate*2-first;if(n>256)n=256;
        for(unsigned i=0;i<n;i++)samples[2*i]=samples[2*i+1]=(int16_t)lrint(amplitude*sin(TAU*(first+i)*hz/rate));
        audio_fx_process(&f,samples,n,(uint8_t)effect);
        for(unsigned i=0;i<n;i++)if(first+i>=rate) {
            double phase=TAU*(first+i)*hz/rate,y=samples[2*i];
            energy+=y*y;dc+=y;si+=y*sin(phase);co+=y*cos(phase);
        }
    }
    double fundamental=2*(si*si+co*co)/rate;
    double noise=fmax(0,energy-fundamental-dc*dc/rate);
    return sqrt(noise/fundamental)*100;
}
static void transition_invariance(void)
{
    const unsigned n=4000,rate=48000;int16_t *a=malloc(n*2*sizeof(*a)),*b=malloc(n*2*sizeof(*b));
    assert(a&&b);uint32_t random=1234;audio_fx_t f,g;
    for(unsigned i=0;i<n*2;i++){random=random*1664525+1013904223;a[i]=b[i]=(int16_t)(random>>16);}
    audio_fx_reset(&f,rate,0);audio_fx_reset(&g,rate,0);
    const unsigned ends[]={100,200,210,220,300,460,470,800,1800,4000};
    const uint8_t modes[]={0,1,2,0,3,1,0,2,3,0};unsigned start=0;
    for(unsigned j=0;j<sizeof ends/sizeof ends[0];j++) {
        audio_fx_process(&f,a+start*2,ends[j]-start,modes[j]);
        for(unsigned k=start;k<ends[j];k++)audio_fx_process(&g,b+k*2,1,modes[j]);
        start=ends[j];
    }
    assert(!memcmp(a,b,n*2*sizeof(*a)));assert(g.current.effect==0);assert(!g.transition);
    free(a);free(b);
}
static void linked_stereo(void)
{
    audio_fx_t f;audio_fx_reset(&f,48000,1);int16_t p[2];
    for(unsigned i=0;i<96000;i++) {
        p[0]=(int16_t)lrint(30000*sin(TAU*i*50/48000));p[1]=(int16_t)lrint(p[0]/2.0);
        audio_fx_process(&f,p,1,1);
        if(i>24000)assert(abs((int)p[0]-2*(int)p[1])<=3);
    }
}
int main(void)
{
    audio_fx_reset(NULL,0,0);audio_fx_process(NULL,NULL,0,0);
    double bass=residual(1,48000,50,30000),mid=residual(1,48000,1000,30000);
    double retro=residual(3,48000,1000,12000),quiet=residual(3,48000,1000,120);
    const double reverb=residual(AUDIO_EFFECT_REVERB,48000,1000,30000);
    /* Guard reconstruction images at 44.1k and /4 wet processing rates,
     * in addition to the existing 48k check. */
    const double reverb44=residual(AUDIO_EFFECT_REVERB,44100,1000,30000);
    const double reverb96=residual(AUDIO_EFFECT_REVERB,96000,1000,30000);
    assert(bass<.02);assert(mid<.01);assert(retro<.02);assert(quiet<1.0);assert(reverb<.01);
    assert(reverb44<.01&&reverb96<.01);
    const unsigned rates[]={8000,11025,12000,16000,22050,24000,32000,44100,48000,88200,96000};
    for(unsigned i=0;i<sizeof rates/sizeof rates[0];i++)assert(residual(1,rates[i],50,30000)<.05);
    transition_invariance();linked_stereo();
    audio_fx_t f;audio_fx_reset(&f,0,255);assert(f.rate==44100);assert(f.current.effect==0);
    int16_t extremes[]={-32768,32767,0,-1,1,17};int16_t copy[6];memcpy(copy,extremes,sizeof extremes);
    audio_fx_process(&f,extremes,3,255);assert(!memcmp(copy,extremes,sizeof extremes));
    printf("FX quality: loud bass THD+N %.5f%%, loud mid %.5f%%, Reverb %.5f%%, Retro %.5f%% / quiet %.3f%%; 11 rates, stereo link, sample-exact rapid preset changes, invalid settings PASS\n",bass,mid,reverb,retro,quiet);
    printf("Reverb reconstruction 1k THD+N at44.1/48/96k: %.6f%% / %.6f%% / %.6f%%; unchanged threshold <0.01%% PASS\n",reverb44,reverb,reverb96);
    return 0;
}
