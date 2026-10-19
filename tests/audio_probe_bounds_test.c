#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Count actual parser I/O, not wall time on a particular host disk. The old
 * parser performs 100,000+ tiny seeks in these small malformed/tag-heavy files. */
static unsigned seeks,reads,end_seeks;
static long furthest_seek;
static int counted_seek(FILE *file,long offset,int origin)
{
    ++seeks;
    if(origin==SEEK_END)++end_seeks;
    if(origin==SEEK_SET && offset>furthest_seek)furthest_seek=offset;
    return fseek(file,offset,origin);
}
static size_t counted_read(void *data,size_t size,size_t count,FILE *file)
{
    ++reads;return fread(data,size,count,file);
}
#define fseek counted_seek
#define fread counted_read
#include "../main/audio_file.c"
#undef fseek
#undef fread

static void u32le(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
static void u32be(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[3-i]=(uint8_t)(v>>(i*8));}
static void sync32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[3-i]=(uint8_t)((v>>(i*7))&127);}
static void put(FILE *f,const void *data,size_t bytes){assert(fwrite(data,1,bytes,f)==bytes);}
static FILE *temporary(void){FILE *f=tmpfile();assert(f);return f;}
static int inspect(FILE *f,audio_file_info_t *info)
{
    assert(!fflush(f));seeks=reads=end_seeks=0;furthest_seek=0;
    int result=audio_probe_stream(f,info);
    assert(reads<=PROBE_MAX_READS && seeks<=PROBE_MAX_READS && !end_seeks);
    return result;
}
static void mp3_frames(FILE *f)
{
    uint8_t frame[417]={0xff,0xfb,0x90,0};put(f,frame,sizeof frame);put(f,frame,sizeof frame);
}
static void many_id3_frames(void)
{
    FILE *f=temporary();const unsigned frames=100000;
    uint8_t header[10]={'I','D','3',3,0,0};sync32(header+6,frames*11);put(f,header,10);
    uint8_t entry[11]={'P','R','I','V'};u32be(entry+4,1);
    for(unsigned i=0;i<frames;i++)put(f,entry,sizeof entry);
    mp3_frames(f);audio_file_info_t info;
    assert(inspect(f,&info)==0 && info.format==AUDIO_FORMAT_MP3 && info.data_offset==10+frames*11);
    assert(reads<80);fclose(f);
}
static void sync_pattern_storm(void)
{
    FILE *f=temporary();const uint8_t false_header[4]={0xff,0xfb,0x90,0};
    for(unsigned i=0;i<100000;i++)put(f,false_header,sizeof false_header);
    audio_file_info_t info;assert(inspect(f,&info)==-2);assert(reads<24);fclose(f);
}
static void wav_header(FILE *f,uint32_t bytes)
{
    uint8_t header[12]={'R','I','F','F',0,0,0,0,'W','A','V','E'};u32le(header+4,bytes-8);put(f,header,12);
}
static void wav_format_and_data(FILE *f,uint32_t data_bytes)
{
    uint8_t chunk[32]={'f','m','t',' ',16,0,0,0,1,0,1,0};
    u32le(chunk+12,48000);u32le(chunk+16,96000);chunk[20]=2;chunk[22]=16;
    memcpy(chunk+24,"data",4);u32le(chunk+28,data_bytes);put(f,chunk,sizeof chunk);
}
static void many_riff_chunks(void)
{
    FILE *f=temporary();const unsigned count=100000;
    wav_header(f,12+count*8+32+2);
    const uint8_t chunk[8]={'J','U','N','K'};
    for(unsigned i=0;i<count;i++)put(f,chunk,sizeof chunk);
    wav_format_and_data(f,2);put(f,"\0\0",2);
    audio_file_info_t info;assert(inspect(f,&info)==-2);fclose(f);
}
static void many_list_entries(void)
{
    FILE *f=temporary();const unsigned count=100000;
    wav_header(f,12+12+count*8+32+2);
    uint8_t list[12]={'L','I','S','T',0,0,0,0,'I','N','F','O'};u32le(list+4,4+count*8);put(f,list,12);
    const uint8_t entry[8]={'I','C','M','T'};
    for(unsigned i=0;i<count;i++)put(f,entry,sizeof entry);
    wav_format_and_data(f,2);put(f,"\0\0",2);
    audio_file_info_t info;assert(inspect(f,&info)==0 && info.format==AUDIO_FORMAT_WAV);
    assert(reads<80);fclose(f);
}
static void large_regular_file_without_tail_seek(void)
{
    const uint32_t size=64u*1024u*1024u;
    FILE *f=temporary();mp3_frames(f);assert(!fseek(f,(long)size-1,SEEK_SET));put(f,"\0",1);
    audio_file_info_t info;assert(inspect(f,&info)==0 && info.file_size==size);
    assert(furthest_seek==0 && reads<=3);fclose(f);
    f=temporary();wav_header(f,size);wav_format_and_data(f,size-44);
    assert(!fseek(f,(long)size-1,SEEK_SET));put(f,"\0",1);
    assert(inspect(f,&info)==0 && info.data_size==size-44);
    assert(furthest_seek<=36 && reads<=5);fclose(f);
}
static void sync_across_read_boundary(void)
{
    FILE *f=temporary();uint8_t junk[16383]={0};put(f,junk,sizeof junk);mp3_frames(f);
    audio_file_info_t info;assert(inspect(f,&info)==0 && info.data_offset==sizeof junk);fclose(f);
}
int main(void)
{
    many_id3_frames();sync_pattern_storm();many_riff_chunks();many_list_entries();
    large_regular_file_without_tail_seek();sync_across_read_boundary();
    puts("audio probe bounds: huge tag/chunk lists, sync storms, large-file no-tail-seek and boundary sync passed");
    return 0;
}
