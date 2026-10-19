#ifdef NDEBUG
#undef NDEBUG
#endif
#include "audio_fx.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static double tone(unsigned effect,unsigned rate,double hz)
{
    audio_fx_t fx;audio_fx_reset(&fx,rate,(uint8_t)effect);
    int16_t block[256*2];double energy=0;unsigned count=0;
    for(unsigned i=0;i<rate;i+=256) {
        unsigned n=rate-i<256?rate-i:256;
        for(unsigned j=0;j<n;j++)block[j*2]=block[j*2+1]=(int16_t)lrint(12000*sin(6.283185307179586*(i+j)*hz/rate));
        audio_fx_process(&fx,block,n,(uint8_t)effect);
        if(i>rate/4)for(unsigned j=0;j<n;j++){energy+=(double)block[j*2]*block[j*2];++count;}
    }
    return sqrt(energy/count);
}
int main(void)
{
    audio_fx_t a,b;int16_t input[8192],whole[8192],split[8192];
    uint32_t rng=123;
    for(unsigned i=0;i<8192;i++){rng=rng*1664525+1013904223;input[i]=(int16_t)(rng>>16);}
    for(unsigned rate=8000;rate<=96000;rate+=8000)for(unsigned effect=0;effect<4;effect++) {
        memcpy(whole,input,sizeof input);memcpy(split,input,sizeof input);
        audio_fx_reset(&a,rate,(uint8_t)effect);audio_fx_reset(&b,rate,(uint8_t)effect);
        audio_fx_process(&a,whole,4096,(uint8_t)effect);
        for(unsigned pos=0;pos<4096;) {
            unsigned n=(pos*13+17)%127+1;if(n>4096-pos)n=4096-pos;
            audio_fx_process(&b,split+pos*2,n,(uint8_t)effect);pos+=n;
        }
        assert(!memcmp(whole,split,sizeof whole));
        if(effect==0)assert(!memcmp(whole,input,sizeof input));
        else for(unsigned i=0;i<8192;i++)assert(whole[i]!=32767 && whole[i]!=-32768);
    }
    const double bass=tone(1,48000,50),treble=tone(1,48000,4000);
    assert(bass/treble>1.8 && bass/treble<2.1);
    const double reference=tone(0,48000,4000);
    assert(fabs(20*log10(treble/reference))<.05);
    assert(fabs(20*log10(tone(1,48000,1000)/tone(0,48000,1000)))<.05);
    assert(tone(3,48000,1000)>tone(3,48000,12000)*3);
    /* Full-scale DC polarity reversals exercise filter/headroom transients
     * that random samples and ordinary sine tests miss. */
    for(unsigned effect=1;effect<4;effect++) {
        audio_fx_reset(&a,96000,(uint8_t)effect);
        for(unsigned pass=0;pass<64;pass++) {
            for(unsigned i=0;i<8192;i++)whole[i]=pass<32?-32768:32767;
            audio_fx_process(&a,whole,4096,(uint8_t)effect);
            for(unsigned i=0;i<8192;i++)assert(whole[i]!=32767 && whole[i]!=-32768);
        }
    }
    audio_fx_reset(&a,48000,AUDIO_EFFECT_REVERB);memset(whole,0,sizeof whole);whole[0]=12000;
    audio_fx_process(&a,whole,4096,AUDIO_EFFECT_REVERB);
    assert(whole[0]==7320&&whole[1]==0&&whole[1778*2]>0);
    unsigned reflections=0;
    for(unsigned i=1600;i<4096;++i)if(whole[i*2]||whole[i*2+1])++reflections;
    assert(reflections>32); /* Many late samples, not one stereo echo. */
    audio_fx_reset(&a,48000,0);memcpy(whole,input,sizeof input);audio_fx_process(&a,whole,4096,0);
    audio_fx_process(&a,whole,4096,1);audio_fx_process(&a,whole,4096,0);
    memcpy(whole,input,sizeof input);audio_fx_process(&a,whole,4096,0);assert(!memcmp(whole,input,sizeof input));
    printf("FX tests: bit-exact NORMAL, 8..96k block invariance/headroom, BASS+ %.2f dB shelf, REVERB impulse/tail, RETRO bandwidth and NORMAL restoration passed\n",20*log10(bass/treble));
    return 0;
}
