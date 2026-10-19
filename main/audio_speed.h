#pragma once
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "audio_speed_table.h"

/* Turntable speed, including pitch. One streaming FIR before local/BT routing;
 * no clock changes, decoder restart or allocation in the output loop. */
#define SPEED_TAPS 24
#define SPEED_PHASES 64
#define SPEED_MIN 75
#define SPEED_MAX 125
typedef struct {
    unsigned percent, phase, write, transition;
    float last[2],anchor[2];
    int16_t history[SPEED_TAPS][2];
    const int16_t (*weights)[SPEED_TAPS];
} audio_speed_t;
static inline void audio_speed_reset(audio_speed_t *s) {
    s->phase=s->write=s->transition=0;memset(s->history,0,sizeof s->history);memset(s->last,0,sizeof s->last);
}
static inline void audio_speed_configure(audio_speed_t *s,unsigned percent) {
    if(percent<SPEED_MIN||percent>SPEED_MAX)percent=100;
    if(s->percent==percent)return;
    const bool changing=s->percent!=0;
    const float left=s->last[0],right=s->last[1];
    s->percent=percent;audio_speed_reset(s);
    s->anchor[0]=left;s->anchor[1]=right;s->transition=changing?64:0;
    if(percent==100)return;
    s->weights=audio_speed_weights[percent<=100?0:percent-100];
}
/* Capacity must be at least frames * 100 / 75 + 2 stereo frames. */
static inline size_t audio_speed_process(audio_speed_t *s,const int16_t *in,size_t frames,int16_t *out) {
    if(s->percent==100){
        memcpy(out,in,frames*4);
        for(size_t n=0;n<frames&&s->transition;n++,s->transition--)
            for(unsigned ch=0;ch<2;ch++)out[n*2+ch]=(int16_t)lrintf(s->anchor[ch]+(out[n*2+ch]-s->anchor[ch])*(65-s->transition)/64);
        if(frames){s->last[0]=out[(frames-1)*2];s->last[1]=out[(frames-1)*2+1];}return frames;
    }
    size_t used=0;
    for(size_t n=0;n<frames;n++) {
        s->history[s->write][0]=in[n*2];s->history[s->write][1]=in[n*2+1];s->phase+=100;
        while(s->phase>=s->percent) {
            s->phase-=s->percent;
            unsigned p=s->phase*SPEED_PHASES/100;
            for(unsigned ch=0;ch<2;ch++) {
                float value=0;
                for(unsigned k=0;k<SPEED_TAPS;k++)value+=(float)((int32_t)s->history[(s->write+SPEED_TAPS-k)%SPEED_TAPS][ch]*s->weights[p][k]);
                value*=1.f/32768.f;
                if(s->transition)value=s->anchor[ch]+(value-s->anchor[ch])*(65-s->transition)/64;
                out[used*2+ch]=(int16_t)lrintf(fmaxf(-32768,fminf(32767,value)));
                s->last[ch]=out[used*2+ch];
            }
            if(s->transition)--s->transition;
            ++used;
        }
        s->write=(s->write+1)%SPEED_TAPS;
    }
    return used;
}
