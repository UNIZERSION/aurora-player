#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../native-idf/main/AirPodsProtocol.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
int main() {
  const uint8_t battery[]={4,0,4,0,4,0,3,2,1,100,2,1,4,1,99,1,1,8,1,17,2,1};
  const uint8_t ear[]={4,0,4,0,6,0,0,0};
  for(size_t split=0;split<=sizeof(battery);split++) {
    airpods::Parser p;p.feed(battery,split);p.feed(battery+split,sizeof(battery)-split);
    assert(p.state.left==99&&p.state.right==100&&p.state.chargeCase==17);
    assert(p.state.mode==-1&&p.state.ears==-1);
  }
  airpods::Parser p;uint8_t mode[11];
  assert(!airpods::mode_command(0,mode)&&!airpods::mode_command(5,mode));
  for(unsigned n=1;n<=4;n++) {
    assert(airpods::mode_command(n,mode));
    const auto revision=p.state.revision;
    for(size_t i=0;i<10;i++)p.feed(mode+i,1);
    assert(p.state.revision==revision);p.feed(mode+10,1);assert(p.state.mode==int(n));
  }
  std::vector<uint8_t> combined;
  combined.insert(combined.end(),battery,battery+sizeof(battery));
  combined.insert(combined.end(),ear,ear+sizeof(ear));
  combined.insert(combined.end(),mode,mode+sizeof(mode));
  p.reset();p.feed(combined.data(),combined.size());assert(p.state.ears==2&&p.state.mode==4&&p.state.left==99);
  // Every truncation and invalid battery field is rejected atomically.
  for(size_t length=0;length<sizeof(battery);length++){p.reset();p.feed(battery,length);assert(p.state.revision==0);}
  for(size_t index:{size_t(6),size_t(7),size_t(8),size_t(9),size_t(10),size_t(11)}) {
    std::vector<uint8_t> invalid(battery,battery+sizeof(battery));invalid[index]=255;
    p.reset();p.feed(invalid.data(),invalid.size());assert(p.state.revision==0);
    p.feed(ear,sizeof(ear));assert(p.state.ears==2);
  }
  for(uint8_t a=0;a<=2;a++)for(uint8_t b=0;b<=2;b++) {
    uint8_t ears[]={4,0,4,0,6,0,a,b};p.reset();p.feed(ears,sizeof(ears));
    assert(p.state.ears==int(a==0)+int(b==0));
  }
  // Bounded storage and resynchronization after arbitrary/unknown payloads.
  uint32_t random=0x12345678;
  for(unsigned i=0;i<1000000;i++) {
    random=random*1664525+1013904223;uint8_t byte=uint8_t(random>>24);p.feed(&byte,1);
  }
  p.feed(combined.data(),combined.size());assert(p.state.left==99&&p.state.ears==2&&p.state.mode==4);
  puts("PASS AirPods protocol: all packet splits, concatenation, 4 confirmed modes, 9 ear states, battery component/status/range validation, truncation, one million malformed bytes and recovery; no headphone simulated.");
}
