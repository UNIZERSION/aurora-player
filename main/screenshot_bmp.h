#pragma once
#include <cstdio>
#include <cstdint>
#include <cstddef>
// Top-down, uncompressed RGB565 with explicit channel masks; native pixels.
inline bool screenshot_bmp(FILE *file,const uint16_t *pixels,unsigned w,unsigned h) {
 if(!file||!pixels||!w||!h||w>1280||h>720)return false;
 const uint32_t stride=(w*2+3)&~3U,size=66+stride*h;uint8_t header[66]{};
 auto put=[&](int at,uint32_t v){for(int n=0;n<4;n++)header[at+n]=uint8_t(v>>(n*8));};
 header[0]='B';header[1]='M';put(2,size);put(10,66);put(14,40);put(18,w);put(22,uint32_t(-int32_t(h)));
 header[26]=1;header[28]=16;put(30,3);put(34,stride*h);put(54,0xf800);put(58,0x07e0);put(62,0x001f);
 if(fwrite(header,1,sizeof header,file)!=sizeof header)return false;
 const uint8_t zero[2]{};
 for(unsigned y=0;y<h;y++)if(fwrite(pixels+y*w,2,w,file)!=w || (stride>w*2 && fwrite(zero,1,stride-w*2,file)!=stride-w*2))return false;
 return fflush(file)==0;
}
