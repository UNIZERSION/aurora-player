#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#ifdef ESP_PLATFORM
#include "miniz.h"
namespace aurora_asset_zlib {
using ::tinfl_decompressor;
using ::tinfl_decompress;
using ::TINFL_FLAG_PARSE_ZLIB_HEADER;
using ::TINFL_FLAG_HAS_MORE_INPUT;
using ::TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
using ::TINFL_STATUS_DONE;
}
#else
#include "vendor/tinfl_host.h"
#endif
// APZ1: four-byte magic, exact little-endian raw length, then one zlib stream.
// The existing destination is also the inflater dictionary: no second image.
// Only the ~11KB ROM state is temporarily allocated, never on the task stack.
inline bool aurora_unpack_zlib(const uint8_t *src,size_t size,uint8_t *out,size_t bytes) {
 if(!src||!out||!bytes||size<14||memcmp(src,"APZ1",4))return false;
 uint32_t declared=0;for(int i=0;i<4;i++)declared|=uint32_t(src[4+i])<<(i*8);
 if(declared!=bytes)return false;
 using namespace aurora_asset_zlib;
 auto *state=static_cast<tinfl_decompressor *>(std::calloc(1,sizeof(tinfl_decompressor)));
 if(!state)return false;
 size_t input=size-8,output=bytes;
 // HAS_MORE_INPUT prevents legacy tinfl's implicit zero-padding on truncation.
 // This is a one-shot call: every NEEDS_MORE_INPUT/HAS_MORE_OUTPUT is rejected.
 const auto status=tinfl_decompress(state,src+8,&input,out,out,&output,
     TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF|TINFL_FLAG_HAS_MORE_INPUT);
 const bool valid=status==TINFL_STATUS_DONE&&input==size-8&&output==bytes;
 std::free(state);return valid;
}
// Lossless run packets. Decoded once at boot; renderers use the original bytes.
inline bool aurora_unpack(const uint8_t *src,size_t size,uint8_t *out,size_t bytes) {
 if(src&&size>=4&&!memcmp(src,"APZ1",4))return aurora_unpack_zlib(src,size,out,bytes);
 if(!src||!out||size<9||memcmp(src,"APK1",4))return false;
 const unsigned unit=src[4];uint32_t declared=0;for(int i=0;i<4;i++)declared|=uint32_t(src[5+i])<<(i*8);
 if(unit<1||unit>3||declared!=bytes||bytes%unit)return false;
 size_t in=9,pos=0;
 while(in<size && pos<bytes){unsigned token=src[in++],count=(token&127)+1;size_t span=size_t(count)*unit;
  if(span>bytes-pos)return false;
  if(token&128){if(size-in<unit)return false;for(unsigned n=0;n<count;n++)memcpy(out+pos+n*unit,src+in,unit);in+=unit;}
  else{if(size-in<span)return false;memcpy(out+pos,src+in,span);in+=span;}
  pos+=span;
 }
 return pos==bytes && in==size;
}
