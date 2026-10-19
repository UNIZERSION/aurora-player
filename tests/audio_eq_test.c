/* Production implementation included to time configuration separately and
 * inspect stability/state; no alternate DSP algorithm is substituted. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../main/audio_eq.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <complex.h>

static const uint32_t rates[]={8000,11025,16000,22050,32000,44100,48000,88200,96000};
static uint32_t random_state=0x641c3ea9;
static uint32_t random_u32(void){random_state=random_state*1664525U+1013904223U;return random_state;}
static int16_t pcm[192000],other[192000],input[192000];

static double actual_response(const audio_eq_bank_t *bank,double rate,double frequency)
{
    const double w=2.0*EQ_PI*frequency/rate;
    const double complex z=cos(w)+I*sin(w);
    double complex response=1;
    for(unsigned i=0;i<bank->count;++i){
        const audio_eq_filter_t *f=&bank->filters[i];
        /* Independent complex state-space inverse, not the production
         * polynomial/half-angle response evaluator. */
        const double a11=2.0*f->t0-1,a12=-2.0*f->t1,a21=2.0*f->t1,a22=1-2.0*f->t2;
        const double b1=2.0*f->t1,b2=2.0*f->t2;
        const double c1=(double)f->mix*f->t0,c2=-(double)f->mix*f->t1;
        const double d=1+(double)f->mix*f->t1;
        response*=d+(c1*((z-a22)*b1+a12*b2)+c2*(a21*b1+(z-a11)*b2))/((z-a11)*(z-a22)-a12*a21);
    }
    return cabs(response);
}
static void check_state(const audio_eq_t *eq)
{
    assert(isfinite(eq->limiter_gain)&&eq->limiter_gain>0&&eq->limiter_gain<=1.0f);
    assert(isfinite(eq->limiter_deficit)&&eq->limiter_deficit>=0&&eq->limiter_deficit<1.0f);
    const audio_eq_bank_t *banks[]={&eq->current,&eq->previous};
    for(unsigned b=0;b<2;++b){
        assert(isfinite(banks[b]->input_gain)&&banks[b]->input_gain>0&&banks[b]->input_gain<=1.00001);
        
        for(unsigned i=0;i<banks[b]->count;++i){
            const audio_eq_filter_t *f=&banks[b]->filters[i];
            /* Strict second-order Schur/Jury stability conditions. */
            assert(fabs(f->a2)<1.0&&1.0+(double)f->a1+f->a2>0&&1.0-(double)f->a1+f->a2>0);
            const double trace=2.0*f->t0-2.0*f->t2;
            const double determinant=(2.0*f->t0-1.0)*(1.0-2.0*f->t2)+4.0*(double)f->t1*f->t1;
            assert(fabs(determinant)<1.0&&1.0-trace+determinant>0&&1.0+trace+determinant>0);
            for(unsigned c=0;c<2;++c)assert(isfinite(f->z1[c])&&isfinite(f->z2[c]));
        }
    }
}
static void fresh_stream_configuration(void)
{
    audio_eq_t eq;audio_eq_config_t config={.preamp_db=-12,.enabled=1};
    audio_eq_reset_config(NULL,48000,&config);
    const int16_t values[]={-32768,-24000,-1,0,1,24000,32767};
    for(unsigned r=0;r<sizeof rates/sizeof *rates;++r){
        memset(&eq,0xa5,sizeof eq);audio_eq_reset_config(&eq,rates[r],&config);
        assert(eq.rate==rates[r]&&!eq.transition&&!eq.current.identity);
        assert(!memcmp(&eq.current,&eq.previous,sizeof eq.current));
        assert(audio_eq_reduction_db(&eq)==0&&eq.limiter_gain==1);
        for(unsigned i=0;i<sizeof values/sizeof *values;++i){
            int16_t sample[2]={values[i],values[i]};audio_eq_process(&eq,sample,1,&config);
            const int expected=(int)lrint(values[i]*pow(10.0,-12.0/20.0));
            assert(abs(sample[0]-expected)<=1&&sample[0]==sample[1]&&!eq.transition);
        }
        for(unsigned i=0;i<1024;++i)pcm[i]=(i&1)?-32768:32767;
        audio_eq_process(&eq,pcm,512,&config);
        for(unsigned i=0;i<1024;++i)assert(abs(pcm[i])<=8232);
        check_state(&eq);
    }
    for(unsigned variant=0;variant<6;++variant){
        audio_eq_config_t invalid={.enabled=1};
        if(variant==1)invalid.enabled=2;
        if(variant==2)invalid.gain_db[0]=13;
        if(variant==3)invalid.preamp_db=1;
        if(variant==4)invalid.preamp_db=-25;
        if(variant==5){invalid.enabled=0;invalid.preamp_db=-12;invalid.gain_db[0]=6;}
        memset(&eq,0xa5,sizeof eq);audio_eq_reset_config(&eq,1,variant?&invalid:NULL);
        assert(eq.rate==44100&&eq.current.identity&&eq.limiter_gain==1&&!eq.transition);
        int16_t sample[2]={-32768,32767};audio_eq_process(&eq,sample,1,variant?&invalid:NULL);
        assert(sample[0]==-32768&&sample[1]==32767&&audio_eq_reduction_db(&eq)==0);
    }
    puts("PASS reset_config: 9 rates, fixed -12 dB from first sample without unity fade; null state/config, invalid gains/preamp/enabled/rate, disabled bypass and stale state cleared.");
}
static void basics(void)
{
    audio_eq_t eq;audio_eq_config_t c={0};c.enabled=1;
    assert(sizeof(c)==13&&audio_eq_valid_config(&c));
    assert(!audio_eq_valid_config(NULL));
    for(unsigned i=0;i<AUDIO_EQ_BANDS;++i){c.gain_db[i]=13;assert(!audio_eq_valid_config(&c));c.gain_db[i]=-13;assert(!audio_eq_valid_config(&c));c.gain_db[i]=0;}
    c.preamp_db=1;assert(!audio_eq_valid_config(&c));c.preamp_db=-25;assert(!audio_eq_valid_config(&c));c.preamp_db=0;
    c.enabled=2;assert(!audio_eq_valid_config(&c));c.enabled=1;
    audio_eq_reset(NULL,44100);audio_eq_reset(&eq,1);assert(eq.rate==44100);
    audio_eq_process(NULL,NULL,0,NULL);audio_eq_process(&eq,NULL,1,&c);
    for(unsigned r=0;r<sizeof rates/sizeof *rates;++r){
        audio_eq_reset(&eq,rates[r]);memset(pcm,0,sizeof pcm);audio_eq_process(&eq,pcm,rates[r]/20,&c);
        for(int n=-32768;n<=32767;++n){int16_t sample[2]={(int16_t)n,(int16_t)(-1-n)};audio_eq_process(&eq,sample,1,&c);assert(sample[0]==n&&sample[1]==-1-n);}
        assert(audio_eq_reduction_db(&eq)==0);
        for(unsigned band=0;band<AUDIO_EQ_BANDS;++band){
            assert(audio_eq_band_available(rates[r],band)==(audio_eq_frequencies[band]<.475*rates[r]));
            if(!audio_eq_band_available(rates[r],band)){
                c.gain_db[band]=12;bank_configure(&eq,&eq.current,&c);assert(eq.current.identity&&eq.current.count==0);c.gain_db[band]=0;
            }
        }
    }
    assert(!audio_eq_band_available(0,0)&&!audio_eq_band_available(44100,11));
    audio_eq_reset(&eq,44100);c.enabled=1;c.preamp_db=-6;
    memset(pcm,0,sizeof pcm);audio_eq_process(&eq,pcm,2205,&c);
    for(int n=-32768;n<=32767;++n){int16_t sample[2]={(int16_t)n,(int16_t)n};audio_eq_process(&eq,sample,1,&c);assert(abs(sample[0]-(int)lrint(n*pow(10,-6.0/20)))<=1);}
    c.gain_db[0]=13;audio_eq_process(&eq,pcm,2205,&c);
    int16_t raw[2]={-32768,32767};audio_eq_process(&eq,raw,1,&c);assert(raw[0]==-32768&&raw[1]==32767);
    puts("PASS flat: 1,179,648 exact stereo PCM values, 9 rates; disabled/invalid/unsupported bands, ranges, null and zero-length API.");
}
static void response_and_preamp(void)
{
    double all_boost_response=0;unsigned configurations=0;
    for(unsigned r=0;r<sizeof rates/sizeof *rates;++r){
        audio_eq_t eq;audio_eq_reset(&eq,rates[r]);
        for(unsigned trial=0;trial<35;++trial){
            audio_eq_config_t c={0};c.enabled=1;
            for(unsigned i=0;i<AUDIO_EQ_BANDS;++i)c.gain_db[i]=trial==0?12:trial==1?-12:(int)(random_u32()%25)-12;
            c.preamp_db=trial==2?-24:trial==3?-12:0;
            bank_configure(&eq,&eq.current,&c);check_state(&eq);++configurations;
            double peak=1;
            for(unsigned p=0;p<=4096;++p){
                const double frequency=p?exp(log(rates[r]*.5)*(p-1)/4095.0):0;
                const double response=actual_response(&eq.current,rates[r],frequency);
                if(response>peak)peak=response;
            }
            assert(isfinite(peak)&&peak<100000);
            assert(fabs(eq.current.input_gain-pow(10.0,c.preamp_db/20.0))<1e-7);
            assert(eq.limiter_gain==1.0f&&audio_eq_reduction_db(&eq)==0);
            if(trial==0&&rates[r]==48000)all_boost_response=20*log10(peak);

        }
        for(unsigned b=0;b<AUDIO_EQ_BANDS;++b)for(int g=-12;g<=12;++g){
            audio_eq_config_t c={0};c.enabled=1;c.gain_db[b]=(int8_t)g;
            bank_configure(&eq,&eq.current,&c);check_state(&eq);
        }
    }
    assert(all_boost_response>17&&all_boost_response<20);
    printf("PASS response: %u cascades x4097 dense double points, stable finite response; all +12 at48k peak %.3f dB, manual preamp only and no automatic attenuation.\n",configurations,all_boost_response);
}
static void tones(void)
{
    double worst_error=0,worst_thdn=0;unsigned cases=0,worst_rate=0,worst_band=0;int worst_sign=0;
    for(unsigned r=0;r<sizeof rates/sizeof *rates;++r)for(unsigned band=0;band<AUDIO_EQ_BANDS;++band){
        if(!audio_eq_band_available(rates[r],band))continue;
        for(int sign=-1;sign<=1;sign+=2){
            audio_eq_t eq;audio_eq_reset(&eq,rates[r]);audio_eq_config_t c={0};c.enabled=1;c.gain_db[band]=(int8_t)(12*sign);
            const unsigned n=rates[r];const double freq=audio_eq_frequencies[band],amplitude=sign>0?6000:12000;
            for(unsigned i=0;i<n;++i)pcm[2*i]=pcm[2*i+1]=(int16_t)lrint(amplitude*sin(2*EQ_PI*freq*i/n));
            memcpy(input,pcm,n*2*sizeof *pcm);audio_eq_process(&eq,pcm,n,&c); /* settle a full second */
            memcpy(pcm,input,n*2*sizeof *pcm);audio_eq_process(&eq,pcm,n,&c);
            double real=0,imag=0,power=0,in_real=0;
            for(unsigned i=0;i<n;++i){const double w=2*EQ_PI*freq*i/n;real+=pcm[2*i]*sin(w);imag+=pcm[2*i]*cos(w);power+=(double)pcm[2*i]*pcm[2*i];in_real+=input[2*i]*sin(w);assert(pcm[2*i]==pcm[2*i+1]);}
            const double magnitude=hypot(real,imag)/in_real;
            const double expected=pow(10.0,sign*12/20.0)*eq.current.input_gain;
            const double error=fabs(20*log10(magnitude/expected));
            const double fundamental=2*(real*real+imag*imag)/n;
            const double thdn=sqrt(fmax(0,power-fundamental)/fundamental);
            if(error>worst_error)worst_error=error;if(thdn>worst_thdn){worst_thdn=thdn;worst_rate=n;worst_band=band;worst_sign=sign;}
            if(error>.02||thdn>.00035)printf("Tone problem rate%u band%u sign%d error%.5f thdn%.6f limiter%.6f\n",n,band,sign,error,thdn,eq.limiter_gain);
            assert(error<.02&&thdn<.00035);check_state(&eq);++cases;
        }
    }
    printf("PASS tones: %u measured center responses; max error %.5f dB, worst THD+N %.6f (%.2f dB), rate%u band%u sign%d.\n",cases,worst_error,worst_thdn,20*log10(worst_thdn),worst_rate,worst_band,worst_sign);
}
static void process_chunks(audio_eq_t *eq,int16_t *dst,size_t offset,size_t count,unsigned chunk,const audio_eq_config_t *c)
{
    while(count){size_t take=count<chunk?count:chunk;audio_eq_process(eq,dst+offset*2,take,c);offset+=take;count-=take;}
}
static void partitions(void)
{
    const size_t lengths[]={211,399,917,1801,17,4096,4011};
    for(unsigned rate_index=0;rate_index<3;++rate_index){
        const unsigned rate=rate_index==0?8000:rate_index==1?44100:96000;
        audio_eq_config_t configs[7]={{0}};
        for(unsigned n=0;n<7;++n){configs[n].enabled=n<6;for(unsigned b=0;b<11;++b)configs[n].gain_db[b]=(int8_t)((int)(random_u32()%25)-12);configs[n].preamp_db=n==4?-9:0;}
        size_t total=0;for(unsigned i=0;i<7;++i)total+=lengths[i];
        for(size_t i=0;i<total*2;++i)input[i]=(int16_t)(random_u32()>>16);
        memcpy(pcm,input,total*2*sizeof *pcm);audio_eq_t reference;audio_eq_reset(&reference,rate);size_t offset=0;
        for(unsigned i=0;i<7;++i){process_chunks(&reference,pcm,offset,lengths[i],1,&configs[i]);offset+=lengths[i];}
        for(unsigned chunk=7;chunk<=1024;chunk=chunk==7?128:chunk==128?1024:1025){
            memcpy(other,input,total*2*sizeof *other);audio_eq_t eq;audio_eq_reset(&eq,rate);offset=0;
            for(unsigned i=0;i<7;++i){process_chunks(&eq,other,offset,lengths[i],chunk,&configs[i]);offset+=lengths[i];}
            assert(!memcmp(pcm,other,total*2*sizeof *pcm));
            /* Meter is explicitly scoped to the latest caller block; DSP
             * state and every output sample must remain partition invariant. */
            audio_eq_t expected=reference;expected.reduction_gain=eq.reduction_gain;
            assert(!memcmp(&expected,&eq,sizeof eq));
        }
    }
    puts("PASS sample-exact block invariance: chunks1/7/128/1024, 3 rates, rapid superseding settings, preamp and bypass.");
}
typedef struct {double b0,b1,b2,a1,a2,z1,z2;} double_filter_t;
static void mixed_reference(void)
{
    double worst_snr=200;unsigned cases=0;
    const unsigned frequencies[]={31,97,331,1031,2473,4471,9011,17777};
    for(unsigned r=0;r<sizeof rates/sizeof *rates;++r)for(unsigned mode=0;mode<3;++mode){
        audio_eq_t eq;audio_eq_reset(&eq,rates[r]);audio_eq_config_t c={0};c.enabled=1;
        for(unsigned b=0;b<11;++b)c.gain_db[b]=mode==0?12:mode==1?-12:b%2?-12:12;
        bank_configure(&eq,&eq.current,&c);double_filter_t reference[11]={{0}};unsigned count=0;
        for(unsigned b=0;b<11;++b)if(audio_eq_band_available(rates[r],b)){
            double_filter_t *f=&reference[count++];
            const double a=pow(10,c.gain_db[b]/40.0),w=2*EQ_PI*audio_eq_frequencies[b]/rates[r];
            const double alpha=sin(w)/(2*sqrt(2.0)),a0=1+alpha/a;
            f->b0=(1+alpha*a)/a0;f->b1=-2*cos(w)/a0;f->b2=(1-alpha*a)/a0;f->a1=f->b1;f->a2=(1-alpha/a)/a0;
        }
        double noise=0,signal=0;float least_gain=1;
        unsigned tones_count=0;for(unsigned i=0;i<8;++i)if(frequencies[i]<.475*rates[r])++tones_count;
        for(unsigned offset=0;offset<2*rates[r];){
            unsigned take=2*rates[r]-offset;if(take>512)take=512;
            for(unsigned i=0;i<take;++i){
                double sample=0;for(unsigned t=0;t<tones_count;++t)sample+=cos(2*EQ_PI*frequencies[t]*(offset+i)/rates[r]);
                input[2*i]=input[2*i+1]=pcm[2*i]=pcm[2*i+1]=(int16_t)lrint((mode==1?32000:3000)*sample/tones_count);
            }
            audio_eq_process(&eq,pcm,take,&c);
            if(eq.limiter_gain<least_gain)least_gain=eq.limiter_gain;
            for(unsigned i=0;i<take;++i){
                double value=input[2*i]*eq.current.input_gain;
                for(unsigned b=0;b<count;++b){double_filter_t *f=&reference[b];double out=f->b0*value+f->z1;f->z1=f->b1*value-f->a1*out+f->z2;f->z2=f->b2*value-f->a2*out;value=out;}
                if(offset+i>=rates[r]){double error=pcm[2*i]-value;noise+=error*error;signal+=value*value;}
            }
            offset+=take;
        }
        const double snr=10*log10(signal/noise);
        if(snr<worst_snr)worst_snr=snr;
        if(snr<60||least_gain<.99999)printf("Mixed reference problem rate%u mode%u SNR%.3f limiter%.5f\n",rates[r],mode,snr,least_gain);
        assert(snr>60&&least_gain>.99999);++cases;
    }
    printf("PASS mixed reference: %u full cascades,8-band multitone peaks3000 boosted/32000 cut; min SNR %.2f dB versus independent double RBJ, no limiter activation.\n",cases,worst_snr);
}
static void overload_and_switching(void)
{
    float least_gain=1;unsigned max_jump=0;
    for(unsigned r=0;r<sizeof rates/sizeof *rates;++r){
        audio_eq_t eq;audio_eq_reset(&eq,rates[r]);audio_eq_config_t c={0};c.enabled=1;c.gain_db[0]=-12;
        /* Abrupt full-scale polarity changes stress cut-filter ringing. Right
         * channel is exactly1/4 left; the limiter must preserve that relation. */
        for(unsigned block=0;block<200;++block){
            for(unsigned i=0;i<256;++i){int16_t value=((block*256+i)/(rates[r]/8))%2?32000:-32000;pcm[2*i]=value;pcm[2*i+1]=value/4;}
            audio_eq_process(&eq,pcm,256,&c);check_state(&eq);
            for(unsigned i=0;i<256;++i){assert(abs((int)pcm[2*i]-4*(int)pcm[2*i+1])<=3);assert(abs((int)pcm[2*i])<=32767);}
            if(eq.limiter_gain<least_gain)least_gain=eq.limiter_gain;
        }
        audio_eq_reset(&eq,rates[r]);
        for(unsigned block=0;block<160;++block){
            if(block%3==0){for(unsigned b=0;b<11;++b)c.gain_db[b]=(int8_t)((int)(random_u32()%25)-12);c.preamp_db=(int8_t)-(int)(random_u32()%25);}
            for(unsigned i=0;i<256;++i){pcm[2*i]=(int16_t)((int)(random_u32()%65535)-32767);pcm[2*i+1]=(int16_t)-pcm[2*i];}
            audio_eq_process(&eq,pcm,256,&c);check_state(&eq);
            for(unsigned i=0;i<256;++i)if(pcm[2*i]!=-32768)assert(abs((int)pcm[2*i]+pcm[2*i+1])<=2);
        }
        audio_eq_reset(&eq,rates[r]);int previous=20000;
        for(unsigned segment=0;segment<12;++segment){
            c.enabled=segment%3!=2;c.preamp_db=segment%2?-24:0;for(unsigned b=0;b<11;++b)c.gain_db[b]=segment%2?12:-12;
            for(unsigned i=0;i<rates[r]/25;++i){int16_t sample[2]={20000,20000};audio_eq_process(&eq,sample,1,&c);unsigned jump=(unsigned)abs(sample[0]-previous);if(jump>max_jump)max_jump=jump;previous=sample[0];}
        }
    }
    assert(least_gain<.99f);assert(max_jump<500);
    printf("PASS overload: full-scale steps/ringing and randomized extremes finite; stereo ratio retained; limiter min %.4f; max constant-input switching jump %u PCM counts.\n",least_gain,max_jump);
}
static void timings(void)
{
    audio_eq_t eq;audio_eq_reset(&eq,48000);audio_eq_config_t c={0};c.enabled=1;
    for(unsigned b=0;b<11;++b)c.gain_db[b]=12;
    const clock_t setup=clock();
    for(unsigned i=0;i<2000;++i){c.gain_db[i%11]=(i%2)?11:12;bank_configure(&eq,&eq.current,&c);}
    const double configure_us=(double)(clock()-setup)*1e6/CLOCKS_PER_SEC/2000;
    const clock_t start=clock();
    for(unsigned i=0;i<2000;++i){memset(pcm,0,512*2*sizeof *pcm);audio_eq_process(&eq,pcm,512,&c);}
    const double process_us=(double)(clock()-start)*1e6/CLOCKS_PER_SEC/2000;
    printf("HOST TIMING (not ESP32-P4): configure %.2f us, steady11 bands stereo512 frames %.2f us; fixed state %zu bytes.\n",configure_us,process_us,sizeof eq);
}
static void bass_independence(void)
{
    double worst_high_error=0,worst_bass_error=0;unsigned cases=0;
    for(unsigned r=0;r<sizeof rates/sizeof *rates;++r)for(unsigned level=0;level<3;++level){
        const unsigned rate=rates[r],high=rate<16000?2000:4000;
        audio_eq_t eq;audio_eq_reset(&eq,rate);audio_eq_config_t c={.enabled=1};
        c.gain_db[1]=6;c.preamp_db=level==0?0:level==1?-6:-24;
        double lo_sin=0,lo_cos=0,hi_sin=0,hi_cos=0,in_lo=0,in_hi=0;
        for(unsigned offset=0;offset<rate*2;){
            unsigned take=rate*2-offset;if(take>512)take=512;
            for(unsigned i=0;i<take;++i){
                const double t=(double)(offset+i)/rate;
                input[2*i]=input[2*i+1]=pcm[2*i]=pcm[2*i+1]=(int16_t)lrint(2000*sin(2*EQ_PI*63*t)+3000*sin(2*EQ_PI*high*t));
            }
            audio_eq_process(&eq,pcm,take,&c);assert(audio_eq_reduction_db(&eq)==0);
            for(unsigned i=0;i<take;++i)if(offset+i>=rate){
                const double t=(double)(offset+i)/rate;
                const double ls=sin(2*EQ_PI*63*t),hs=sin(2*EQ_PI*high*t);
                lo_sin+=pcm[2*i]*ls;lo_cos+=pcm[2*i]*cos(2*EQ_PI*63*t);
                hi_sin+=pcm[2*i]*hs;hi_cos+=pcm[2*i]*cos(2*EQ_PI*high*t);
                in_lo+=input[2*i]*ls;in_hi+=input[2*i]*hs;
            }
            offset+=take;
        }
        const double low_db=20*log10(hypot(lo_sin,lo_cos)/in_lo);
        const double high_db=20*log10(hypot(hi_sin,hi_cos)/in_hi);
        const double high_error=fabs(high_db-c.preamp_db),bass_error=fabs(low_db-c.preamp_db-6);
        if(high_error>worst_high_error)worst_high_error=high_error;
        if(bass_error>worst_bass_error)worst_bass_error=bass_error;
        assert(high_error<.03&&bass_error<.03);++cases;
    }
    printf("PASS bass independence: %u two-tone cases, +6 dB at63 Hz leaves 2/4 kHz within %.5f dB of manual preamp; bass error %.5f dB; no limiter reduction.\n",cases,worst_high_error,worst_bass_error);
}
static void limiter_meter_and_recovery(void)
{
    float smallest_db=100,max_release_step=0;
    for(unsigned r=0;r<sizeof rates/sizeof *rates;++r){
        const unsigned rate=rates[r];audio_eq_t eq;audio_eq_reset(&eq,rate);
        audio_eq_config_t hot={.enabled=1};hot.gain_db[5]=12;
        float maximum_db=0;
        for(unsigned offset=0;offset<rate;){
            unsigned take=rate-offset;if(take>512)take=512;
            for(unsigned i=0;i<take;++i){pcm[2*i]=(int16_t)lrint(30000*sin(2*EQ_PI*1000*(offset+i)/rate));pcm[2*i+1]=pcm[2*i]/4;}
            audio_eq_process(&eq,pcm,take,&hot);check_state(&eq);
            const float db=audio_eq_reduction_db(&eq);if(db>maximum_db)maximum_db=db;
            for(unsigned i=0;i<take;++i)assert(abs((int)pcm[2*i]-4*(int)pcm[2*i+1])<=5);
            offset+=take;
        }
        assert(maximum_db>10&&eq.limiter_gain<.4f);
        if(maximum_db<smallest_db)smallest_db=maximum_db;
        const float before=eq.limiter_gain;
        audio_eq_config_t flat={0};flat.enabled=r%2;
        int16_t one[2]={1000,250};audio_eq_process(&eq,one,1,&flat);
        assert(eq.limiter_gain<=before+.0001f&&audio_eq_reduction_db(&eq)>8);
        /* Recovery continues after bypass/flat, does not reset with the bank.
         * Three seconds covers 50ms hold plus 250ms release to <0.5 PCM. */
        float previous=eq.limiter_gain;
        for(unsigned i=0;i<rate*3;++i){
            one[0]=1000;one[1]=250;audio_eq_process(&eq,one,1,&flat);
            const float step=eq.limiter_gain-previous;
            if(step>max_release_step)max_release_step=step;
            previous=eq.limiter_gain;
        }
        assert(eq.limiter_gain==1&&eq.limiter_deficit==0&&audio_eq_reduction_db(&eq)==0);
        assert(one[0]==1000&&one[1]==250);
        for(int value=-32768;value<=32767;++value){one[0]=(int16_t)value;one[1]=(int16_t)(-1-value);audio_eq_process(&eq,one,1,&flat);assert(one[0]==value&&one[1]==-1-value);}
    }
    assert(max_release_step<.0005f);
    /* One processed block includes both overload and complete recovery: the
     * meter must retain its largest applied reduction, not just the end value. */
    audio_eq_t eq;audio_eq_reset(&eq,8000);audio_eq_config_t hot={.enabled=1};hot.gain_db[5]=12;
    bank_configure(&eq,&eq.current,&hot);
    memset(pcm,0,24000*2*sizeof *pcm);pcm[0]=30000;pcm[1]=30000;
    audio_eq_process(&eq,pcm,24000,&hot);
    assert(eq.limiter_gain==1&&audio_eq_reduction_db(&eq)>1);
    int16_t silence[2]={0,0};audio_eq_process(&eq,silence,1,&hot);assert(audio_eq_reduction_db(&eq)==0);
    printf("PASS actual overload/meter: 9 rates, +12 dB hot tone reports >=%.2f dB reduction, stereo linked, bypass/flat preserves envelope then recovers bit-exact, max release step %.6f; within-block short peak retained.\n",smallest_db,max_release_step);
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    fresh_stream_configuration();basics();response_and_preamp();tones();partitions();overload_and_switching();mixed_reference();bass_independence();limiter_meter_and_recovery();timings();
    puts("ALL EQ TESTS PASS. Sample-peak limiter is not a true-peak/lookahead guarantee; hardware CPU/audio deadlines require physical measurement.");
    return 0;
}
