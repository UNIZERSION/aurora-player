#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace airpods {
// Independently implemented from the documented AACP packet layout:
// https://github.com/librepods-org/librepods/blob/main/docs/AAP%20Definitions.md
inline constexpr uint8_t handshake[]={0,0,4,0,1,0,2,0,0,0,0,0,0,0,0,0};
inline constexpr uint8_t features[]={4,0,4,0,0x4d,0,0xff,0,0,0,0,0,0,0};
inline constexpr uint8_t notifications[]={4,0,4,0,0x0f,0,0xff,0xff,0xff,0xff};
struct State {
  int mode=-1,left=-1,right=-1,chargeCase=-1,ears=-1;
  uint32_t revision=0;
};
inline bool mode_command(unsigned mode,uint8_t (&out)[11]) {
  if(mode<1 || mode>4) return false;
  constexpr uint8_t base[]={4,0,4,0,9,0,0x0d,0,0,0,0};
  memcpy(out,base,sizeof(base));out[7]=uint8_t(mode);return true;
}
class Parser {
 public:
  State state;
  void reset(){used_=0;state={};}
  void feed(const uint8_t* data,size_t size) {
    if(!data)return;
    for(size_t i=0;i<size;i++) {
      if(used_==sizeof(buffer_))drop(1);
      buffer_[used_++]=data[i];parse();
    }
  }
 private:
  uint8_t buffer_[64]{};size_t used_=0;
  void drop(size_t size){used_-=size;memmove(buffer_,buffer_+size,used_);}
  void parse() {
    while(used_>=6) {
      if(buffer_[0]!=4||buffer_[1]||buffer_[2]!=4||buffer_[3]||buffer_[5]){drop(1);continue;}
      size_t needed=0;
      switch(buffer_[4]) {
        case 4:
          if(used_<7)return;
          if(buffer_[6]<1 || buffer_[6]>3){drop(1);continue;}
          needed=7+5*buffer_[6];break;
        case 6:needed=8;break;
        case 9:needed=11;break;
        default:drop(1);continue;
      }
      if(used_<needed)return;
      State candidate=state;bool valid=false;
      if(buffer_[4]==4) {
        valid=true;unsigned seen=0;
        for(unsigned i=0;i<buffer_[6];i++) {
          const uint8_t* part=buffer_+7+5*i;
          unsigned bit=part[0]==2?1:part[0]==4?2:part[0]==8?4:0;
          if(!bit || (seen&bit) || part[1]!=1 || part[4]!=1 || part[2]>100 ||
             !(part[3]==0||part[3]==1||part[3]==2||part[3]==4)){valid=false;break;}
          seen|=bit;const int level=part[3]==1||part[3]==2?part[2]:-1;
          if(bit==1)candidate.right=level;
          if(bit==2)candidate.left=level;
          if(bit==4)candidate.chargeCase=level;
        }
      } else if(buffer_[4]==6 && buffer_[6]<=2 && buffer_[7]<=2) {
        candidate.ears=int(buffer_[6]==0)+int(buffer_[7]==0);valid=true;
      } else if(buffer_[4]==9 && buffer_[6]==0x0d && buffer_[7]>=1 &&
                buffer_[7]<=4 && !buffer_[8] && !buffer_[9] && !buffer_[10]) {
        candidate.mode=buffer_[7];valid=true;
      }
      if(valid){candidate.revision=state.revision+1;state=candidate;}
      drop(needed);
    }
  }
};
} // namespace airpods
