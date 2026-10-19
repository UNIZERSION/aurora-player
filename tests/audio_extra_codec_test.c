#ifdef NDEBUG
#undef NDEBUG
#endif
#include "audio_extra_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
extern int mock_codec_mode,mock_codec_calls,mock_codec_closes;
int main(void)
{
    uint8_t raw[65536]={1},header[42]="fLaC";size_t used=0;uint32_t rng=1;
    for(int mode=0;mode<8;mode++) {
        mock_codec_mode=mode;audio_extra_codec_t *c=audio_extra_codec_open(AUDIO_FORMAT_AAC,NULL);assert(c);
        int result=audio_extra_codec_decode(c,raw,32,false,&used);
        if(mode==0 || mode==1) {
            assert(result==1 && used==32);int16_t pcm[512];size_t total=0,n;
            while((n=audio_extra_codec_read(c,pcm,256,&rng))) {
                for(size_t i=0;i<n*2;i++)assert(pcm[i]==0x1234);
                total+=n;
            }
            assert(total==(mode==1?20000u:4u));
            int before=mock_codec_calls;assert(audio_extra_codec_decode(c,NULL,0,true,&used)==0 && mock_codec_calls==before);
        } else if(mode==7) {
            assert(result==2);unsigned count=0;
            while(result==2 && count++<20)result=audio_extra_codec_decode(c,raw,sizeof raw,false,&used);
            assert(result==-1 && count<=17);
        } else assert(result==-1);
        audio_extra_codec_close(c);
    }
    mock_codec_mode=0;audio_extra_codec_t *c=audio_extra_codec_open(AUDIO_FORMAT_FLAC,header);assert(c);audio_extra_codec_close(c);
    assert(mock_codec_closes==9);
    puts("Extra codec adapter tests: bounded PCM growth, same-input retry, chunk conversion, EOS, stalls/malformed output and cleanup passed (vendor binary not executed on host)");
    return 0;
}
