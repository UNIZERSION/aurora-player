#pragma once
#include <cstddef>
#include <cstdint>
#include <algorithm>
class StreamPreroll {
public:
  bool ready(size_t available,size_t requested,uint32_t epoch) {
    if(epoch!=epoch_){epoch_=epoch;primed_=false;}
    if(!primed_ && available>=std::max<size_t>(768,requested))primed_=true;
    return primed_;
  }
  void underflow(){primed_=false;}
private:
  bool primed_=false;uint32_t epoch_=0;
};
