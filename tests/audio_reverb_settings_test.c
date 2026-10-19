#undef NDEBUG
#include "../main/audio_fx.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static audio_fx_t a,b;
static int16_t pcm[2304],copy[2304];
static uint32_t rng=13241314;
static uint32_t next(void){rng=rng*1664525+1013904223;return rng;}
static const unsigned rates[]={8000,11025,12000,16000,22050,24000,32000,44100,48000,88200,96000};
static void bounded(const audio_fx_t *f){
 const audio_fx_voice_t *v=&f->current;assert(v->reverb_end[3]<=AUDIO_FX_REVERB_FRAMES);
 assert(v->reverb_amount>=0&&v->reverb_amount<=.6001f&&v->gain>0&&v->gain<=1);
 for(unsigned i=0;i<4;i++){assert(v->reverb_feedback[i]>0&&v->reverb_feedback[i]<1);assert(v->reverb_position[i]>=v->reverb_start[i]&&v->reverb_position[i]<v->reverb_end[i]);}
 for(unsigned i=0;i<v->reverb_end[3];i++)assert(isfinite(v->reverb_delay[i])&&fabsf(v->reverb_delay[i])<2000000.f);
}
static void bounds_and_input(void){
 audio_reverb_config_t c=audio_reverb_default_config();assert(audio_reverb_valid_config(&c));assert(!audio_reverb_valid_config(NULL));
 assert(!audio_fx_set_reverb_config(NULL,&c));c.decay_ms=499;assert(!audio_reverb_valid_config(&c));c.decay_ms=4001;assert(!audio_reverb_valid_config(&c));c.decay_ms=1500;
 c.tone_hz=99;assert(!audio_reverb_valid_config(&c));c.tone_hz=20001;assert(!audio_reverb_valid_config(&c));c.tone_hz=1400;c.low_cut_hz=1400;assert(!audio_reverb_valid_config(&c));c.low_cut_hz=0;c.amount=61;assert(!audio_reverb_valid_config(&c));
 for(unsigned r=0;r<11;r++)for(unsigned corner=0;corner<16;corner++){
  audio_fx_reset(&a,rates[r],2);c=(audio_reverb_config_t){.decay_ms=corner&1?4000:500,.tone_hz=corner&2?20000:100,.amount=corner&4?60:0,.low_cut_hz=corner&8?(corner&2?19900:0):0};assert(audio_fx_set_reverb_config(&a,&c));
  for(unsigned block=0;block<160;block++){
   for(unsigned i=0;i<1152;i++){
    if(block<40)pcm[i*2]=pcm[i*2+1]=-32768;else if(block<80)pcm[i*2]=pcm[i*2+1]=32767;
    else if(block<120){pcm[i*2]=(i&1)?32767:-32768;pcm[i*2+1]=-1-pcm[i*2];}
    else {pcm[i*2]=(int16_t)(next()>>16);pcm[i*2+1]=(int16_t)(next()>>16);}
   }
   audio_fx_process(&a,pcm,1152,2);bounded(&a);
   if(c.amount==60&&block>20)for(unsigned i=0;i<2304;i++)assert(abs(pcm[i])<32600);
  }
 }
 puts("PASS 176 rate/parameter corners: full-scale DC, polarity reversal, antiphase and random PCM; finite bounded state and protected wet peak.");
}
static void continuity_and_partition(void){
 unsigned maximum_step=0;
 for(unsigned r=0;r<11;r++){
  audio_fx_reset(&a,rates[r],2);
  for(unsigned n=0;n<rates[r];n++){int16_t frame[]={18000,12000};audio_fx_process(&a,frame,1,2);}
  for(unsigned change=0;change<60;change++){
   b=a;audio_reverb_config_t c={.decay_ms=change&1?4000:500,.tone_hz=change&2?20000:100,.amount=change&4?60:0,.low_cut_hz=change&8?(change&2?19900:0):0};
   assert(audio_fx_set_reverb_config(&a,&c));assert(!memcmp(a.current.reverb_delay,b.current.reverb_delay,sizeof a.current.reverb_delay));
   int16_t altered[]={18000,12000},reference[]={18000,12000};audio_fx_process(&a,altered,1,2);audio_fx_process(&b,reference,1,2);
   for(unsigned ch=0;ch<2;ch++){const unsigned delta=abs(altered[ch]-reference[ch]);if(delta>maximum_step)maximum_step=delta;assert(delta<=80);}
   b=a;assert(audio_fx_set_reverb_config(&b,&c));
   for(unsigned i=0;i<2304;i++)pcm[i]=copy[i]=(int16_t)(next()>>16);
   audio_fx_process(&a,pcm,1152,2);
   for(unsigned n=0;n<1152;){const unsigned count=n+37>1152?1152-n:37;audio_fx_process(&b,copy+n*2,count,2);n+=count;}
   assert(!memcmp(pcm,copy,sizeof pcm)&&!memcmp(&a,&b,sizeof a));bounded(&a);
  }
 }
 printf("PASS 660 live parameter changes: tail retained; first-sample deviation <=%u PCM; state/PCM exact across arbitrary block partitioning.\n",maximum_step);
}
static double impulse_tail(unsigned decay,unsigned tone,double *difference){
 audio_fx_reset(&a,48000,2);const audio_reverb_config_t c={.decay_ms=decay,.tone_hz=tone,.amount=60};audio_fx_set_reverb_config(&a,&c);
 // Settle coefficient ramps before the test impulse, as a real saved setting.
 for(unsigned n=0;n<48000;n++){const float in[]={0,0};float out[2];process_voice(&a,&a.current,in,out);}
 double energy=0,delta=0;float previous=0;
 for(unsigned n=0;n<48000*8;n++){
  const float in[]={n?0:30000,0};float out[2];process_voice(&a,&a.current,in,out);
  if(n>24000&&n<96000)energy+=(double)out[0]*out[0]+(double)out[1]*out[1];
  if(n>2400&&n<12000)delta+=(double)(out[0]-previous)*(out[0]-previous);previous=out[0];
  if(n>48000*7)assert(fabsf(out[0])<.5f&&fabsf(out[1])<.5f);
 }
 bounded(&a);if(difference)*difference=delta;return energy;
}
static void meaningful_controls(void){
 const double short_tail=impulse_tail(500,1400,NULL),long_tail=impulse_tail(4000,1400,NULL);assert(long_tail>short_tail*1000);
 double warm,bright;impulse_tail(1500,600,&warm);impulse_tail(1500,5000,&bright);assert(bright>warm*1.5);
 audio_fx_reset(&a,48000,2);audio_reverb_config_t c=audio_reverb_default_config();c.amount=0;audio_fx_set_reverb_config(&a,&c);
 for(unsigned n=0;n<48000;n++){pcm[0]=10000;pcm[1]=-10000;audio_fx_process(&a,pcm,1,2);}
 for(unsigned i=0;i<2304;i++)pcm[i]=copy[i]=(int16_t)(next()>>16);audio_fx_process(&a,pcm,1152,2);assert(!memcmp(pcm,copy,sizeof pcm));
 printf("PASS Amount=0 settles to bit-exact dry; late-energy long/short ratio %.1f; bright/warm early difference-energy ratio %.2f; maximum tail reaches sub-PCM level by7s.\n",long_tail/short_tail,bright/warm);
}
static void host_timing(void){
 audio_fx_reset(&a,48000,2);clock_t begin=clock();
 for(unsigned block=0;block<3000;block++){
  const audio_reverb_config_t c={.decay_ms=(block&1)?4000:500,.tone_hz=(block&2)?5000:600,.amount=(block&4)?60:0};
  for(unsigned i=0;i<2304;i++)pcm[i]=(int16_t)(next()>>16);
  audio_fx_set_reverb_config(&a,&c);audio_fx_process(&a,pcm,1152,2);
 }
 const double us=(double)(clock()-begin)*1e6/CLOCKS_PER_SEC/3000;
 assert(sizeof a<80000);printf("HOST ONLY worst-update benchmark1152 stereo: %.2fus/block; total DSP state%zu bytes; no audio-thread allocation; not a Tab5 deadline measurement.\n",us,sizeof a);
}
int main(void){bounds_and_input();continuity_and_partition();meaningful_controls();host_timing();return 0;}
