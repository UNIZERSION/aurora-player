#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
// One owner: the A2DP audio callback. Volume changes ramp over 256 frames.
class PcmGain {
 public:
  void target(int percent) {
    percent = std::max(0, std::min(percent, 100));
    const int next = percent * percent * 32768 / 10000;
    if (next != target_) { target_ = next; remaining_ = 256; }
  }
  void frame(int16_t& left, int16_t& right) {
    if (remaining_) { current_ += (target_ - current_) / remaining_; --remaining_; }
    left = int16_t(int32_t(left) * current_ / 32768);
    right = int16_t(int32_t(right) * current_ / 32768);
  }
  template<class Frame> void block(Frame *frames,size_t count) {
    if(!remaining_ && current_==32768)return;
    if(!remaining_ && !current_){std::memset(frames,0,count*sizeof(Frame));return;}
    for(size_t i=0;i<count;i++) {
      int16_t left=frames[i].channel1,right=frames[i].channel2;
      frame(left,right);frames[i].channel1=left;frames[i].channel2=right;
    }
  }
 private:
  int current_ = 0, target_ = 0, remaining_ = 0;
};
