#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tab5bt {
/* Bounded polyphase FIR: 32 taps, 128 fractional phases, stereo history.
 * A 0.90-Nyquist Blackman low-pass limits aliasing when reducing 48/96 kHz.
 * Coefficients are prepared only on rate changes. Native 44.1 kHz is copied
 * exactly. The conversion has a 16-source-frame delay and no heap activity. */
class Resampler {
public:
    static constexpr uint32_t outputRate=44100;
    bool configure(uint32_t rate) {
        if(rate<8000 || rate>96000)return false;
        const bool changed=rate_!=rate;
        rate_=rate;reset();
        if(!changed || rate==outputRate)return true;
        const double cutoff=0.90*std::min(1.0,double(outputRate)/rate);
        constexpr double pi=3.14159265358979323846;
        for(unsigned p=0;p<phases;p++) {
            double sum=0;
            for(unsigned k=0;k<taps;k++) {
                const double x=double(k)-15.0-double(p)/phases;
                const double sinc=std::abs(x)<1e-10?cutoff:std::sin(pi*cutoff*x)/(pi*x);
                const double window=0.42-0.5*std::cos(2*pi*k/(taps-1))+0.08*std::cos(4*pi*k/(taps-1));
                weights_[p][k]=float(sinc*window);sum+=weights_[p][k];
            }
            for(unsigned k=0;k<taps;k++)weights_[p][k]/=float(sum);
        }
        return true;
    }
    void reset(){phase_=write_=0;std::memset(history_,0,sizeof(history_));}
    template<class Emit> bool push(const int16_t *input,size_t frames,Emit emit) {
        if(!rate_ || !input)return false;
        if(rate_==outputRate)return emit(input,frames);
        int16_t block[256*2];size_t used=0;
        for(size_t n=0;n<frames;n++) {
            history_[write_][0]=input[n*2];history_[write_][1]=input[n*2+1];
            phase_+=outputRate;
            while(phase_>=rate_) {
                phase_-=rate_;
                const unsigned p=unsigned(uint64_t(phase_)*phases/outputRate);
                float left=0,right=0;
                for(unsigned k=0;k<taps;k++) {
                    const unsigned index=(write_+taps-k)%taps;
                    left+=history_[index][0]*weights_[p][k];
                    right+=history_[index][1]*weights_[p][k];
                }
                block[used*2]=quantize(left);block[used*2+1]=quantize(right);
                if(++used==256){if(!emit(block,used))return false;used=0;}
            }
            write_=(write_+1)%taps;
        }
        return !used || emit(block,used);
    }
private:
    static constexpr unsigned taps=32,phases=128;
    uint32_t rate_=0,phase_=0;
    unsigned write_=0;
    int16_t history_[taps][2]{};
    float weights_[phases][taps]{};
    static int16_t quantize(float sample) {
        return int16_t(std::max(-32768L,std::min(32767L,std::lround(sample))));
    }
};
}
