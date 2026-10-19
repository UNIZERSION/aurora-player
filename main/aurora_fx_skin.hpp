#pragma once
#include "aurora_geometry.h"
#include "aurora_fx_label12.h"
#include <cstdint>
#include <initializer_list>

namespace aurora_fx_skin {
// Retain the approved HTML sprites, including all borders, pressed offsets,
// highlights and active glows. Only the former SPACE/RETRO content is cleared.
// Their 135-degree face gradient is recovered from clean pixels on the same
// row, once after unpacking; no allocation or pixel processing during redraw.
inline uint16_t blend565(uint16_t left,uint16_t right,int at,int span){
    uint16_t result=0;
    for(int shift:{11,5,0}){
        const int mask=shift==5?63:31;
        const int a=(left>>shift)&mask,b=(right>>shift)&mask;
        result|=uint16_t(((a*(span-at)+b*at+span/2)/span)<<shift);
    }
    return result;
}
inline void prepare(uint8_t *cache){
    for(int id:{14,15}){
        const auto &r=aurora_controls[id];
        for(int state=0;state<4;state++){
            auto *data=cache+r.offset+state*r.stride*r.h;
            for(int y=8;y<r.h-8;y++){
                auto *row=reinterpret_cast<uint16_t*>(data+y*r.stride);
                const int left=12,right=r.w-13;
                for(int x=18;x<r.w-18;x++)row[x]=blend565(row[left],row[right],x-left,right-left);
            }
        }
    }
}
inline uint32_t foreground(bool active,bool pressed){
    const uint32_t color=active?0xe0ef72:0xc2c7c0;
    if(!pressed)return color;
    uint32_t result=0;
    for(int shift:{16,8,0}){const unsigned channel=((color>>shift)&255)*5/4;result|=(channel>255?255:channel)<<shift;}
    return result;
}
}
