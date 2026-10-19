#ifdef NDEBUG
#undef NDEBUG
#endif
#include "audio_speed.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void){
 enum{N=48000};int16_t *input=malloc(N*4),*a=malloc((N*2+4)*4),*b=malloc((N*2+4)*4);
 assert(input&&a&&b);
 for(unsigned i=0;i<N;i++){input[i*2]=(int16_t)lrint(12000*sin(i*6.283185307179586*1000/48000));input[i*2+1]=-input[i*2];}
 for(unsigned speed=75;speed<=125;speed++){
  audio_speed_t whole={0},split={0};audio_speed_configure(&whole,speed);audio_speed_configure(&split,speed);
  size_t n=audio_speed_process(&whole,input,N,a),used=0;
  for(size_t pos=0;pos<N;){size_t block=(pos*17+53)%1152+1;if(block>N-pos)block=N-pos;used+=audio_speed_process(&split,input+pos*2,block,b+used*2);pos+=block;}
  assert(n==N*100/speed&&used==n&&!memcmp(a,b,n*4));
  if(speed==100)assert(!memcmp(input,a,N*4));
  double power=0;unsigned crossings=0;
  for(size_t i=128;i<n;i++){assert(abs(a[i*2]+a[i*2+1])<=1);power+=(double)a[i*2]*a[i*2];if(a[(i-1)*2]<=0&&a[i*2]>0)crossings++;}
  double hz=crossings*48000./(n-128),expected=1000.*speed/100;
  assert(fabs(hz-expected)<3);assert(sqrt(power/(n-128))>8200&&sqrt(power/(n-128))<8700);
  audio_speed_reset(&whole);audio_speed_reset(&split);assert(audio_speed_process(&whole,input,1152,a)==audio_speed_process(&split,input,1152,b));assert(!memcmp(a,b,1152*100/speed*4));
 }
 audio_speed_t changing={0};audio_speed_configure(&changing,100);audio_speed_process(&changing,input,1152,a);
 int16_t anchor=a[1151*2];audio_speed_configure(&changing,75);audio_speed_process(&changing,input+1152*2,1152,a);
 assert(abs(a[0]-anchor)<600&&changing.transition==0);
 audio_speed_configure(&changing,100);audio_speed_process(&changing,input,1152,a);assert(!memcmp(a+128,input+128,(1152-64)*4));
 free(input);free(a);free(b);puts("PASS: all 51 speeds 75..125%, frame count/pitch, unity passband, stereo, chunk invariance, reset, 64-frame transition and bit-exact settled 100%");return 0;
}
