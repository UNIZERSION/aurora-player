#pragma once

// Temporary hardware diagnostics only. No recording, RAM, or logging in the
// release build. Declare a STATIC instance: the ring must not use task stack.
#if defined(AURORA_TAP_TRACE) && AURORA_TAP_TRACE
#include <cmath>
#include <cstdint>
#include <cstdio>
#include "esp_log.h"

namespace player {
class TapTraceDebug {
public:
    void reset() {
        start_=count_=dump_at_=0;
        seeded_=capturing_=dumping_=false;
        // Cooldown intentionally survives a session reset / bus interruption.
    }

    void feed(uint32_t now,float x,float y,float z) {
        if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)){reset();return;}
        if(dumping_){dump_batch();return;}
        if(cooling_){if(now-cooldown_at_<10000)return;cooling_=false;}
        if(seeded_&&now-last_ms_>80)reset();
        const float dx=std::fabs(x-px_),dy=std::fabs(y-py_),dz=std::fabs(z-pz_);
        const bool impact=seeded_&&(dx>0.18f||dy>0.18f||dz>0.18f);
        const uint16_t dt=seeded_?uint16_t(now-last_ms_):0;
        last_ms_=now;px_=x;py_=y;pz_=z;seeded_=true;
        if(!count_)base_ms_=now;
        if(!capturing_&&!impact&&count_==Pretrigger){
            start_=(start_+1)%Capacity;--count_;
            base_ms_+=records_[start_].dt;
        }
        records_[(start_+count_)%Capacity]={dt,mg(x),mg(y),mg(z)};
        ++count_;
        if(!capturing_&&impact){capturing_=true;trigger_ms_=now;trigger_index_=count_-1;}
        if(capturing_&&(now-trigger_ms_>=3000||count_==Capacity)){
            capturing_=false;dumping_=true;dump_at_=0;++sequence_;
            full_=count_==Capacity&&now-trigger_ms_<3000;
            cooldown_at_=now;cooling_=true;
            // No log in this sample: flush begins on a later feed call.
        }
    }

private:
    struct Record {uint16_t dt;int16_t x,y,z;};
    static_assert(sizeof(Record)==8,"Fixed trace wire record");
    static constexpr unsigned Capacity=640,Pretrigger=64;
    Record records_[Capacity]{};
    unsigned start_=0,count_=0,dump_at_=0,trigger_index_=0,sequence_=0;
    uint32_t base_ms_=0,last_ms_=0,trigger_ms_=0,cooldown_at_=0;
    float px_=0,py_=0,pz_=0;
    bool seeded_=false,capturing_=false,dumping_=false,cooling_=false,full_=false;

    static int16_t mg(float value){
        const float scaled=value*1000.0f;
        return scaled>32767?32767:scaled<-32768?-32768:int16_t(std::lround(scaled));
    }
    void dump_batch(){
        if(!dump_at_)ESP_LOGI("tap_trace","TAP_TRACE BEGIN id=%u base_ms=%lu count=%u trigger=%u reason=%s",
            sequence_,static_cast<unsigned long>(base_ms_),count_,trigger_index_,full_?"capacity":"window");
        char hex[16*16+1];unsigned written=0;
        const unsigned remaining=count_-dump_at_;
        const unsigned batch=remaining>16?16:remaining;
        static constexpr char digits[]="0123456789abcdef";
        for(unsigned i=0;i<batch;++i){
            const auto &r=records_[(start_+dump_at_+i)%Capacity];
            const uint16_t words[]={dump_at_+i?r.dt:uint16_t(0),uint16_t(r.x),uint16_t(r.y),uint16_t(r.z)};
            for(auto word:words)for(unsigned byte=0;byte<2;++byte){
                const unsigned value=(word>>(8*byte))&255;
                hex[written++]=digits[value>>4];hex[written++]=digits[value&15];
            }
        }
        hex[written]=0;
        ESP_LOGI("tap_trace","TAP_TRACE DATA id=%u offset=%u hex=%s",sequence_,dump_at_,hex);
        dump_at_+=batch;
        if(dump_at_==count_){
            ESP_LOGI("tap_trace","TAP_TRACE END id=%u",sequence_);
            reset();
        }
    }
};
} // namespace player
#endif
