/* Actual production DSP; inspect float tail as well as public PCM processing. */
#undef NDEBUG
#include "../main/audio_fx.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const unsigned rates[]={8000,11025,12000,16000,22050,24000,32000,44100,48000,88200,96000};
static audio_fx_t fx,reference;
static int16_t pcm[8192],whole[8192];
static double energy[96000*3];
static uint32_t rng=0xbaba1201;
static uint32_t random_value(void){rng=rng*1664525+1013904223;return rng;}
static float noise(void){return ((int)(random_value()>>16)-32768)*.3f;}
static void state_bound(const audio_fx_t *f)
{
    const audio_fx_voice_t *v=&f->current;
    assert(v->reverb_end[3]<=AUDIO_FX_REVERB_FRAMES);
    assert(v->reverb_divisor>=1&&v->reverb_divisor<=4&&v->reverb_phase<=v->reverb_divisor);
    assert(v->reverb_damping>0&&v->reverb_damping<1);
    assert(v->reverb_input_alpha>0&&v->reverb_input_alpha<1);
    assert(v->reverb_output_alpha>0&&v->reverb_output_alpha<1);
    for(unsigned i=0;i<4;++i){
        assert(v->reverb_position[i]>=v->reverb_start[i]&&v->reverb_position[i]<v->reverb_end[i]);
        assert(v->reverb_feedback[i]>0&&v->reverb_feedback[i]<.85f);
        assert(isfinite(v->reverb_low[i])&&fabsf(v->reverb_low[i])<262144.0f);
        assert(isfinite(v->reverb_low2[i])&&fabsf(v->reverb_low2[i])<262144.0f);
    }
    for(unsigned i=0;i<v->reverb_end[3];++i)assert(isfinite(v->reverb_delay[i])&&fabsf(v->reverb_delay[i])<262144.0f);
    for(unsigned ch=0;ch<2;++ch){
        assert(isfinite(v->reverb_previous[ch])&&fabsf(v->reverb_previous[ch])<262144.0f);
        assert(isfinite(v->reverb_next[ch])&&fabsf(v->reverb_next[ch])<262144.0f);
        for(unsigned i=0;i<4;++i)assert(isfinite(v->reverb_input_low[ch][i])&&fabsf(v->reverb_input_low[ch][i])<262144.0f);
        for(unsigned i=0;i<3;++i)assert(isfinite(v->reverb_output_low[ch][i])&&fabsf(v->reverb_output_low[ch][i])<262144.0f);
    }
}
static void impulse_decay(void)
{
    double min_t60=10,max_t60=0,max_hf=0,min_side=10,min_density=1;
    for(unsigned ri=0;ri<11;++ri)for(unsigned channel=0;channel<4;++channel){
        const unsigned rate=rates[ri];audio_fx_reset(&fx,rate,AUDIO_EFFECT_REVERB);
        const float impulse[2]={channel!=1?32000:0,channel==1||channel==2?32000:channel==3?-32000:0};
        double sum=0,early=0,early_difference=0,late=0,late_difference=0,side=0,windows[50]={0};
        float previous[2]={0};unsigned occupied=0;
        for(unsigned n=0;n<rate*3;++n){
            const float in[2]={n?0:impulse[0],n?0:impulse[1]};float out[2];process_voice(&fx,&fx.current,in,out);
            if(!n){assert(fabsf(out[0]-.61f*in[0])<.01f&&fabsf(out[1]-.61f*in[1])<.01f);out[0]=out[1]=0;}
            if(n<rate*.036)assert(out[0]==0&&out[1]==0); /* No dominant short early taps. */
            energy[n]=(double)out[0]*out[0]+(double)out[1]*out[1];sum+=energy[n];
            if(n<rate)windows[n*50/rate]+=energy[n];
            side+=(double)(out[0]-out[1])*(out[0]-out[1]);
            if(n>rate*.05&&n<rate*.4&&energy[n]>.0001)++occupied;
            const double delta=(double)(out[0]-previous[0])*(out[0]-previous[0])+(double)(out[1]-previous[1])*(out[1]-previous[1]);
            if(n>rate*.05&&n<rate*.15){early+=energy[n];early_difference+=delta;}
            if(n>rate*.25&&n<rate*.45){late+=energy[n];late_difference+=delta;}
            previous[0]=out[0];previous[1]=out[1];
        }
        assert(sum>10000&&late>0&&occupied>150);
        double remaining=sum;unsigned t60=0;
        for(unsigned n=0;n<rate*3;++n){if(!t60&&remaining/sum<1e-6)t60=n;remaining-=energy[n];}
        const double seconds=(double)t60/rate,hf=(late_difference/late)/(early_difference/early);
        if(seconds<min_t60)min_t60=seconds;if(seconds>max_t60)max_t60=seconds;
        if(hf>max_hf)max_hf=hf;if(side/sum<min_side)min_side=side/sum;
        if(seconds<1.1||seconds>1.5||hf>.8)printf("impulse r%u mode%u T60%.4f HF%.4f side%.4f\n",rate,channel,seconds,hf,side/sum);
        assert(seconds>1.1&&seconds<1.5&&hf<.8&&side/sum>.01);
        /* A long aggregate decay can hide a sparse delay with 100 ms holes.
         * Require energy in every 20 ms window after the first reflection,
         * and no deep hole relative to the two adjacent tail windows. */
        for(unsigned w=3;w<49;++w){
            const double density=2*windows[w]/(windows[w-1]+windows[w+1]);
            if(density<min_density)min_density=density;
            if(density<.02)printf("density r%u mode%u window%u %.8f\n",rate,channel,w,density);
            assert(windows[w]>0&&density>.02);
        }
        state_bound(&fx);
    }
    printf("PASS complete wet impulse: 44 L/R/mono/antiphase cases, 11 rates; 61%% dry, no wet before36ms; all-wet integrated-energy T60 %.4f..%.4fs, late/early HF proxy <=%.4f, side ratio >=%.4f.\n",min_t60,max_t60,max_hf,min_side);
    printf("PASS room density: every20ms window60..980ms occupied; minimum energy/adjacent-window mean %.5f; impulse integration spans3s.\n",min_density);
}
static void stopped_tail(void)
{
    const float ends[9]={.05f,.1f,.15f,.2f,.3f,.5f,.75f,1.0f,1.5f};
    double low_db[2][9],high_db[2][9],wet_low=0,wet_high=-100,mono_low=0;
    for(unsigned type=0;type<2;++type)for(unsigned w=0;w<9;++w){low_db[type][w]=0;high_db[type][w]=-200;}
    for(unsigned ri=0;ri<11;++ri)for(unsigned mode=0;mode<4;++mode){
        const unsigned rate=rates[ri],active=mode==3?rate/10:rate;audio_fx_reset(&fx,rate,AUDIO_EFFECT_REVERB);
        float low[2]={0},dc[2]={0};double dry=0,wet=0,mono_dry=0,window[9]={0},mono_window[9]={0};unsigned dry_count=0,count[9]={0};
        const float lp=1-expf(-2*PI*fminf(2200,rate*.2f)/rate),hp=1-expf(-2*PI*100/rate);
        for(unsigned n=0;n<active+rate*3/2+2;++n){
            float raw[2],in[2]={0},out[2];
            for(unsigned ch=0;ch<2;++ch){low[ch]+=lp*(noise()-low[ch]);dc[ch]+=hp*(low[ch]-dc[ch]);raw[ch]=low[ch]-dc[ch];}
            if(n<active){const float envelope=mode==3?(1-(float)n/active)*(1-(float)n/active):1;
                in[0]=(int16_t)(raw[0]*envelope);in[1]=(int16_t)((mode==0?raw[1]:mode==2?-raw[0]:raw[0])*envelope);}
            process_voice(&fx,&fx.current,in,out);
            if(n<active&&(mode==3||n>=active/2)){
                dry+=.3721*(in[0]*in[0]+in[1]*in[1]);mono_dry+=.3721*(in[0]+in[1])*(in[0]+in[1]);++dry_count;
                const double a=out[0]-.61f*in[0],b=out[1]-.61f*in[1];wet+=a*a+b*b;
            }
            if(n>=active)for(unsigned w=0;w<9;++w)if(n-active<rate*ends[w]){
                window[w]+=(double)out[0]*out[0]+(double)out[1]*out[1];mono_window[w]+=(double)(out[0]+out[1])*(out[0]+out[1]);++count[w];break;
            }
        }
        const unsigned type=mode==3;const double dry_rms2=dry/dry_count;
        if(!type){const double ratio=10*log10(wet/dry);if(ratio<wet_low)wet_low=ratio;if(ratio>wet_high)wet_high=ratio;if(!(ratio>-16&&ratio<-11))fprintf(stderr,"wet r%u mode%u %.4fdB\n",rate,mode,ratio);assert(ratio>-16&&ratio<-11);}
        for(unsigned w=0;w<9;++w){
            const double db=10*log10(window[w]/count[w]/dry_rms2);
            if(db<low_db[type][w])low_db[type][w]=db;if(db>high_db[type][w])high_db[type][w]=db;
            if(w==1)assert(db>(!type?-21:-26));
            if(w==4)assert(db>(!type?-31:-35));
            if(w==5)assert(db>(!type?-39:-44));
            if(w&&mode!=3)assert(window[w]/count[w]<window[w-1]/count[w-1]);
        }
        if(mode<2){const double ratio=10*log10(mono_window[1]/count[1]/(mono_dry/dry_count));if(ratio<mono_low)mono_low=ratio;assert(ratio>-23);}
        state_bound(&fx);
    }
    printf("PASS audible stopped tail: 33 broadband stereo/mono/antiphase and11 percussive cases; steady wet/dry %.2f..%.2fdB; mono-summed50..100ms >=%.2fdB.\n",wet_low,wet_high,mono_low);
    for(unsigned type=0;type<2;++type){printf("%s stop RMS/dry dB in0..50/50..100/100..150/150..200/200..300/300..500/500..750/750..1000/1000..1500ms:",type?"Percussive":"Broadband");for(unsigned w=0;w<9;++w)printf(" [%.2f,%.2f]",low_db[type][w],high_db[type][w]);puts("");}
}
static void overload_and_silence(void)
{
    int maximum=0;unsigned frames=0;
    for(unsigned ri=0;ri<11;++ri){
        audio_fx_reset(&fx,rates[ri],AUDIO_EFFECT_REVERB);
        for(unsigned block=0;block<240;++block){
            for(unsigned i=0;i<1024;++i){int16_t left,right;
                if(block<60)left=right=-32768;else if(block<120)left=right=32767;
                else if(block<180){left=(i&1)?32767:-32768;right=(int16_t)(-1-left);}
                else{left=(int16_t)(random_value()>>16);right=(int16_t)(random_value()>>16);}
                pcm[2*i]=left;pcm[2*i+1]=right;
            }
            audio_fx_process(&fx,pcm,1024,AUDIO_EFFECT_REVERB);
            for(unsigned i=0;i<2048;++i){int value=abs(pcm[i]);if(value>maximum)maximum=value;assert(value<=32441);}
            if(!(block%16))state_bound(&fx);frames+=1024;
        }
        for(unsigned t=0;t<rates[ri]*10;){unsigned count=rates[ri]*10-t;if(count>1024)count=1024;
            memset(pcm,0,count*2*sizeof *pcm);audio_fx_process(&fx,pcm,count,AUDIO_EFFECT_REVERB);
            if(t>rates[ri]*2)for(unsigned i=0;i<count*2;++i)assert(pcm[i]==0);t+=count;
        }
        for(unsigned i=0;i<fx.current.reverb_end[3];++i)assert(fx.current.reverb_delay[i]==0);
        for(unsigned i=0;i<4;++i)assert(fx.current.reverb_low[i]==0&&fx.current.reverb_low2[i]==0);
        for(unsigned ch=0;ch<2;++ch){for(unsigned i=0;i<4;++i)assert(fx.current.reverb_input_low[ch][i]==0);for(unsigned i=0;i<3;++i)assert(fx.current.reverb_output_low[ch][i]==0);assert(fx.current.reverb_previous[ch]==0&&fx.current.reverb_next[ch]==0);}
    }
    printf("PASS overload/silence: %u full-scale stereo frames, maxabs%d <=32441; finite feedback state, bounded PCM, independent dry; PCM silent by2s, state exact zero by10s.\n",frames,maximum);
}
static void partition_and_modes(void)
{
    const unsigned ends[]={97,121,123,401,603,1107,1901,3001,4096};const uint8_t effects[]={2,0,2,1,2,3,0,2,0};
    for(unsigned ri=0;ri<11;++ri){
        for(unsigned i=0;i<8192;++i)pcm[i]=whole[i]=(int16_t)(random_value()>>16);
        audio_fx_reset(&fx,rates[ri],0);audio_fx_reset(&reference,rates[ri],0);unsigned start=0;
        for(unsigned segment=0;segment<sizeof ends/sizeof *ends;++segment){
            audio_fx_process(&reference,whole+2*start,ends[segment]-start,effects[segment]);
            for(unsigned n=start;n<ends[segment];++n)audio_fx_process(&fx,pcm+2*n,1,effects[segment]);start=ends[segment];
        }
        assert(!memcmp(pcm,whole,sizeof pcm)&&!memcmp(&fx,&reference,sizeof fx));
        unsigned drain=2*fx.transition_frames+1;memset(pcm,0,drain*2*sizeof *pcm);memset(whole,0,drain*2*sizeof *whole);
        audio_fx_process(&reference,whole,drain,0);for(unsigned n=0;n<drain;++n)audio_fx_process(&fx,pcm+2*n,1,0);
        assert(!memcmp(pcm,whole,drain*2*sizeof *pcm)&&!memcmp(&fx,&reference,sizeof fx)&&fx.current.effect==0&&!fx.transition);
    }
    unsigned maximum_jump=0;
    for(unsigned ri=0;ri<11;++ri){audio_fx_reset(&fx,rates[ri],0);int previous=20000;
        for(unsigned n=0;n<rates[ri]/100;++n){int16_t sample[2]={20000,20000};audio_fx_process(&fx,sample,1,2);
            unsigned jump=(unsigned)abs(sample[0]-previous);if(jump>maximum_jump)maximum_jump=jump;previous=sample[0];}
        assert(previous==12200&&!fx.transition);
    }
    assert(maximum_jump<=98);printf("PASS sample/state-exact block invariance,11 rates, rapid latest-wins10ms fades; constant-input maxstep%u PCM.\n",maximum_jump);
}
static void timing(void)
{
    for(unsigned i=0;i<1152*2;++i)whole[i]=(int16_t)(random_value()>>16);
    audio_fx_reset(&fx,48000,AUDIO_EFFECT_REVERB);clock_t begin=clock();
    for(unsigned i=0;i<3000;++i){memcpy(pcm,whole,1152*2*sizeof *pcm);audio_fx_process(&fx,pcm,1152,AUDIO_EFFECT_REVERB);}
    double block=(double)(clock()-begin)*1e6/CLOCKS_PER_SEC/3000;begin=clock();
    for(unsigned i=0;i<3000;++i)begin_transition(&fx,(i&1)?AUDIO_EFFECT_REVERB:AUDIO_EFFECT_NORMAL);
    double change=(double)(clock()-begin)*1e6/CLOCKS_PER_SEC/3000;
    assert(sizeof fx<80000);printf("HOST ONLY: active stereo1152 %.2fus, copy/configure %.2fus, state%zuB, copy+clear%zuB; not P4/PSRAM deadlines.\n",block,change,sizeof fx,2*sizeof fx.current);
}
static void wet_headroom(void)
{
    const unsigned checks[]={44100,48000,96000};
    const float tones[]={20,31,50,73,100,137,200,317,500,733,1000,2000,4000,8000};
    double maximum=0,minimum_gain=1;unsigned protected=0,total=0;
    for(unsigned ri=0;ri<3;++ri)for(unsigned t=0;t<14;++t)for(unsigned anti=0;anti<2;++anti){
        const unsigned rate=checks[ri];audio_fx_reset(&fx,rate,AUDIO_EFFECT_REVERB);
        for(unsigned n=0;n<2*rate;++n){
            const float sample=32000*sinf(2*PI*tones[t]*n/rate),in[2]={sample,anti?-sample:sample};float out[2];
            process_voice(&fx,&fx.current,in,out);
            for(unsigned ch=0;ch<2;++ch){const double wet=fabs(fx.current.reverb_wet[ch]);
                if(wet>maximum)maximum=wet;if(fx.current.gain<.999f)++protected;
                if(fx.current.gain<minimum_gain)minimum_gain=fx.current.gain;
                assert(fabsf(out[ch]-.61f*in[ch])<=.38f*32700+.01f);++total;
            }
        }
        state_bound(&fx);
    }
    printf("PASS wet headroom:84 loud mono/antiphase20Hz..8kHz renders, pre-protection peak %.2fPCM, gain-active %u/%u channel samples, min wet gain%.4f; linked wet protection, dry unchanged.\n",maximum,protected,total,minimum_gain);
}
static void resonant_quality(void)
{
    const unsigned checks[]={44100,48000,96000};
    const unsigned tones[]={31,50,66,73,100,121,137,200,254,317,500,733,1000};
    double maximum=0,music_before=0,music_after=0;unsigned music_protected=0;float music_peak=0,music_gain=1;
    for(unsigned ri=0;ri<3;++ri)for(unsigned t=0;t<13;++t){
        const unsigned rate=checks[ri];audio_fx_reset(&fx,rate,AUDIO_EFFECT_REVERB);
        double energy_out=0,si=0,co=0,dc=0;
        /* Three seconds of settling separates long room decay from actual
         * steady sine distortion, including the low resonances that drive
         * the wet-only envelope gain. The existing1k quality stays intact. */
        for(unsigned n=0;n<4*rate;++n){
            const double phase=6.28318530717958647692*tones[t]*n/rate;
            int16_t sample[2]={(int16_t)lrint(29204*sin(phase)),(int16_t)lrint(29204*sin(phase))};
            audio_fx_process(&fx,sample,1,AUDIO_EFFECT_REVERB);
            if(n>=3*rate){const double y=sample[0];energy_out+=y*y;si+=y*sin(phase);co+=y*cos(phase);dc+=y;}
        }
        const double fundamental=2*(si*si+co*co)/rate;
        const double thdn=100*sqrt(fmax(0,energy_out-fundamental-dc*dc/rate)/fundamental);
        if(thdn>maximum)maximum=thdn;
        if(thdn>.05)printf("resonance r%u hz%u THDN%.6f gain%.4f\n",rate,tones[t],thdn,fx.current.gain);
        assert(thdn<.05);
    }
    audio_fx_reset(&fx,48000,AUDIO_EFFECT_REVERB);
    for(unsigned n=0;n<48000*6;++n){
        const double time=(double)n/48000;float in[2],out[2];
        for(unsigned ch=0;ch<2;++ch)in[ch]=(float)(6200*sin(2*PI*66*time+ch*.4)+5000*sin(2*PI*121*time)+4200*sin(2*PI*254*time+ch)+3200*sin(2*PI*733*time)+2200*sin(2*PI*1901*time+ch*.7));
        process_voice(&fx,&fx.current,in,out);
        if(fx.current.gain<.999f)++music_protected;
        if(fx.current.gain<music_gain)music_gain=fx.current.gain;
        for(unsigned ch=0;ch<2;++ch){
            const float wet=fx.current.reverb_wet[ch];
            if(fabsf(wet)>music_peak)music_peak=fabsf(wet);
            music_before+=(double)wet*wet;music_after+=(double)(wet*fx.current.gain)*(wet*fx.current.gain);
            assert(fabsf(out[ch]-(.61f*in[ch]+.38f*fx.current.gain*wet))<.01f);
        }
    }
    printf("PASS39 resonant -1dBFS tones31..1000Hz at44.1/48/96k: maxTHD+N %.6f%% <.05%%; six-second adversarial multitone wet peak%.2f, gain-active frames%u/288000, min wet gain%.4f, wet RMS change%.3fdB, dry unchanged.\n",maximum,music_peak,music_protected,music_gain,10*log10(music_after/music_before));
    assert(music_gain>.9f&&10*log10(music_after/music_before)>-1);
    for(unsigned n=0;n<48000;++n){const float in[2]={0,0};float out[2];process_voice(&fx,&fx.current,in,out);}
    assert(fx.current.gain>.99f);
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);assert(AUDIO_EFFECT_REVERB==2&&AUDIO_EFFECT_SPACE==AUDIO_EFFECT_REVERB);
    impulse_decay();stopped_tail();overload_and_silence();partition_and_modes();wet_headroom();resonant_quality();timing();
    puts("ALL REVERB TESTS PASS. Measured tail is not a substitute for final user listening or hardware timing.");return 0;
}
