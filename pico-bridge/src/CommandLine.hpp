#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
class CommandLine {
public:
  enum class Result { More, Ready, Overflow };
  Result put(char value) {
    if(value=='\n') { text_[used_]=0;return overflow_?Result::Overflow:Result::Ready; }
    if(value=='\r')return Result::More;
    if(uint8_t(value)<32 || uint8_t(value)>126){overflow_=true;return Result::More;}
    if(used_>=sizeof(text_)-1)overflow_=true;
    else if(!overflow_)text_[used_++]=value;
    return Result::More;
  }
  const char *text()const{return text_;}
  void reset(){used_=0;overflow_=false;text_[0]=0;}
private:
  char text_[81]{};size_t used_=0;bool overflow_=false;
};
inline bool commandNumber(const char *text,unsigned maximum,unsigned &number) {
  if(!text || !*text)return false;
  number=0;
  for(;*text;text++) {
    if(*text<'0' || *text>'9')return false;
    const unsigned digit=unsigned(*text-'0');
    if(digit>maximum || number>(maximum-digit)/10)return false;
    number=number*10+digit;
  }
  return true;
}
inline bool commandMac(const char *text,uint8_t (&mac)[6]) {
  if(!text || std::strlen(text)!=17)return false;
  for(unsigned i=0;i<6;i++) {
    unsigned value=0;
    for(unsigned j=0;j<2;j++) {
      const char c=text[i*3+j];
      unsigned digit=c>='0'&&c<='9'?unsigned(c-'0'):c>='a'&&c<='f'?unsigned(c-'a'+10):c>='A'&&c<='F'?unsigned(c-'A'+10):16;
      if(digit>15)return false;
      value=value*16+digit;
    }
    mac[i]=uint8_t(value);if(i<5 && text[i*3+2]!=':')return false;
  }
  return true;
}
