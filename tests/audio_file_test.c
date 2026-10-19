#include "audio_file.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void put16(uint8_t *p,unsigned v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t *p,unsigned v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static void putbe32(uint8_t *p,unsigned v){for(unsigned i=0;i<4;i++)p[3-i]=(uint8_t)(v>>(8*i));}
static int probe_bytes(const uint8_t *data,size_t n,audio_file_info_t *o)
{
    FILE *f=tmpfile();assert(f);assert(fwrite(data,1,n,f)==n);assert(!fflush(f));
    int result=audio_probe_stream(f,o);fclose(f);return result;
}
static void wav_tests(void)
{
    uint8_t data[256];audio_file_info_t info;
    for(unsigned bits=16;bits<=32;bits+=8)for(unsigned channels=1;channels<=2;channels++) {
        memset(data,0,sizeof(data));unsigned align=bits/8*channels,bytes=align*4;
        memcpy(data,"RIFF",4);put32(data+4,46+bytes);memcpy(data+8,"WAVE",4);
        memcpy(data+12,"JUNK",4);put32(data+16,1);data[20]=0x7f; /* Odd chunk + pad. */
        memcpy(data+22,"fmt ",4);put32(data+26,16);put16(data+30,1);put16(data+32,channels);
        put32(data+34,48000);put32(data+38,48000*align);put16(data+42,align);put16(data+44,bits);
        memcpy(data+46,"data",4);put32(data+50,bytes);
        assert(probe_bytes(data,54+bytes,&info)==0);
        assert(info.format==AUDIO_FORMAT_WAV && info.channels==channels && info.bits_per_sample==bits);
        assert(info.data_offset==54 && info.total_frames==4 && info.sample_rate==48000 && info.duration_exact);
        assert(probe_bytes(data,53+bytes,&info)==-2); /* Truncated declared RIFF. */
        put16(data+42,1);assert(probe_bytes(data,54+bytes,&info)==-2); /* Bad alignment. */
        put16(data+42,align);put16(data+30,3);assert(probe_bytes(data,54+bytes,&info)==-2); /* IEEE float. */
    }
    memset(data,0,sizeof(data));memcpy(data,"RIFF",4);put32(data+4,64);memcpy(data+8,"WAVEfmt ",8);
    put32(data+16,40);put16(data+20,0xfffe);put16(data+22,1);put32(data+24,44100);
    put32(data+28,176400);put16(data+32,4);put16(data+34,32);put16(data+36,22);put16(data+38,24);
    const uint8_t guid[16]={1,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};memcpy(data+44,guid,16);
    memcpy(data+60,"data",4);put32(data+64,4);
    assert(probe_bytes(data,72,&info)==0 && info.bits_per_sample==32 && info.total_frames==1);
    data[44]=3;assert(probe_bytes(data,72,&info)==-2); /* Non-PCM extensible GUID. */
    data[44]=1;put16(data+38,33);assert(probe_bytes(data,72,&info)==-2);
}
static void mp3_tests(void)
{
    const uint8_t header[4]={0xff,0xfb,0x90,0x00}; /* MPEG1 L3 128k, 44.1 stereo. */
    uint32_t rate;uint16_t channels,samples,br;
    int frame=audio_mp3_header(header,&rate,&channels,&samples,&br);
    assert(frame==417 && rate==44100 && channels==2 && samples==1152 && br==128);
    const uint8_t low[4]={0xff,0xe3,0x88,0xc0}; /* MPEG2.5 L3, 64k, 8k mono. */
    assert(audio_mp3_header(low,&rate,&channels,&samples,&br)==576 && rate==8000 && channels==1 && samples==576);
    uint8_t invalid[4]={0xff,0xfb,0x00,0x00};assert(!audio_mp3_header(invalid,NULL,NULL,NULL,NULL));
    invalid[2]=0xfc;assert(!audio_mp3_header(invalid,NULL,NULL,NULL,NULL));
    uint8_t data[1000]={0};audio_file_info_t info;
    memcpy(data,header,4);memcpy(data+frame,header,4);
    assert(probe_bytes(data,frame*2,&info)==0 && !info.duration_exact && info.duration_ms==52);
    memcpy(data+36,"Xing",4);putbe32(data+40,1);putbe32(data+44,1000);
    assert(probe_bytes(data,frame*2,&info)==0 && info.duration_exact && info.duration_ms==26122);
    memset(data+36,0,20);memcpy(data+36,"VBRI",4);putbe32(data+50,500);
    assert(probe_bytes(data,frame*2,&info)==0 && info.total_frames==576000 && info.duration_ms==13061);
    memset(data,0,sizeof(data));
    memcpy(data,"ID3\3\0\0\0\0\0\20",10); /* 16-byte tag: TIT2 frame + 6-byte UTF16 text. */
    memcpy(data+10,"TIT2",4);putbe32(data+14,6);
    const uint8_t text[6]={1,0xff,0xfe,0x41,0,0};memcpy(data+20,text,6);
    memcpy(data+26,header,4);memcpy(data+26+frame,header,4);
    assert(probe_bytes(data,26+frame*2,&info)==0 && !strcmp(info.title,"A") && info.data_offset==26);
    memset(data,0,sizeof(data));assert(probe_bytes(data,sizeof(data),&info)==-2);
}
static void conversion_tests(void)
{
    uint32_t rng=1;int16_t output[8];
    const uint8_t mono16[]={0,0x80,0xff,0x7f,0,0,0x34,0x12};
    audio_pcm_to_stereo16(mono16,4,16,1,&rng,output);
    assert(output[0]==-32768 && output[1]==-32768 && output[2]==32767 && output[3]==32767);
    assert(output[4]==0 && output[5]==0 && output[6]==0x1234 && output[7]==0x1234);
    const uint8_t stereo24[]={0,0,0x80,0xff,0xff,0x7f,0,0,0x40,0,0,0xc0};
    audio_pcm_to_stereo16(stereo24,2,24,2,&rng,output);
    assert(output[0]<=-32767 && output[1]>=32766 && abs(output[2]-16384)<=1 && abs(output[3]+16384)<=1);
    const uint8_t stereo32[]={0,0,0,0x80,0xff,0xff,0xff,0x7f};
    audio_pcm_to_stereo16(stereo32,1,32,2,&rng,output);
    assert(output[0]<=-32767 && output[1]>=32766);
    int64_t sum=0;unsigned noisy=0;uint8_t zeros[8]={0};
    for(unsigned i=0;i<50000;i++) {
        audio_pcm_to_stereo16(zeros,1,32,2,&rng,output);
        assert(abs(output[0])<=1);sum+=output[0];noisy+=output[0]!=0;
    }
    assert(llabs(sum)<1000 && noisy>5000); /* Dither, no material DC bias. */
}
static void extra_format_tests(void)
{
    uint8_t data[512]={0};audio_file_info_t info;
    /* Two complete 32-byte AAC-LC ADTS frames, 44.1 kHz stereo. */
    const uint8_t adts[7]={255,0xf1,0x50,0x80,4,0x1f,0xfc};
    uint32_t rate=0;uint16_t channels=0;
    assert(audio_aac_header(adts,&rate,&channels)==32 && rate==44100 && channels==2);
    memcpy(data,adts,7);memcpy(data+32,adts,7);
    assert(probe_bytes(data,64,&info)==0 && info.format==AUDIO_FORMAT_AAC && info.total_frames==2048 && !info.duration_exact);
    data[2]|=0xc0;assert(!audio_aac_header(data,NULL,NULL)); /* Unsupported AAC profile. */
    memcpy(data,adts,7);data[6]|=1;assert(!audio_aac_header(data,NULL,NULL)); /* Multiple raw blocks. */
    memset(data,0,sizeof data);memcpy(data,"ID3\3\0\0\0\0\0\0",10);
    memcpy(data+10,adts,7);memcpy(data+42,adts,7);
    assert(probe_bytes(data,74,&info)==0 && info.data_offset==10 && info.format==AUDIO_FORMAT_AAC);
    for(unsigned bits=16;bits<=32;bits+=8)for(unsigned ch=1;ch<=2;ch++) {
        memset(data,0,sizeof data);memcpy(data,"fLaC",4);data[4]=0x80;data[7]=34;
        data[8]=data[10]=0x10; /* min/max 4096 samples per FLAC block. */
        uint64_t packed=(uint64_t)48000<<44|(uint64_t)(ch-1)<<41|(uint64_t)(bits-1)<<36|96000;
        for(unsigned i=0;i<8;i++)data[18+i]=(uint8_t)(packed>>(56-i*8));
        data[42]=0xff;data[43]=0xf8;
        assert(probe_bytes(data,50,&info)==0 && info.format==AUDIO_FORMAT_FLAC && info.duration_exact);
        assert(info.sample_rate==48000 && info.bits_per_sample==bits && info.channels==ch && info.duration_ms==2000 && info.data_offset==42);
        data[4]=0;data[42]=0x84;data[43]=data[44]=0;data[45]=22;put32(data+46,0);put32(data+50,1);put32(data+54,10);memcpy(data+58,"TITLE=Song",10);
        assert(probe_bytes(data,75,&info)==0 && !strcmp(info.title,"Song") && info.data_offset==68);
        data[45]=200;assert(probe_bytes(data,75,&info)==-2);
    }
    memset(data,0,sizeof data);memcpy(data,"fLaC",4);data[4]=0x81;data[7]=34;
    assert(probe_bytes(data,50,&info)==-2); /* STREAMINFO must be first. */
}
int main(int argc,char **argv)
{
    wav_tests();mp3_tests();conversion_tests();extra_format_tests();
    if(argc==3) {
        audio_file_info_t flac,aac;
        assert(audio_probe_file(argv[1],&flac)==0 && flac.format==AUDIO_FORMAT_FLAC && flac.sample_rate==48000 && flac.channels==2 && flac.bits_per_sample==24 && flac.total_frames==2048);
        assert(audio_probe_file(argv[2],&aac)==0 && aac.format==AUDIO_FORMAT_AAC && aac.sample_rate==44100 && aac.channels==1 && aac.total_frames==5120);
    }
    puts("audio_file tests: WAV/MP3, FLAC STREAMINFO/comments, AAC-LC ADTS/ID3, malformed files and PCM dither passed");
    return 0;
}
