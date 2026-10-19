#include "audio_file.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Metadata probing must not become a full-file scan on a fragmented SD card.
 * Limits apply to optional tags/chunks as well as malformed input. There is no
 * dynamic allocation proportional to a tag or chunk length. */
#define PROBE_MAX_READS 192u
#define PROBE_MAX_TAG_FRAMES 64u
#define PROBE_MAX_HEADER_BYTES (16u * 1024u * 1024u)
#define PROBE_MP3_SCAN_BYTES (256u * 1024u)
#define PROBE_MP3_LOOKAHEAD 2048u
typedef struct {
    FILE *file;
    unsigned reads_left;
} probe_t;

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24; }
static uint32_t syncsafe(const uint8_t *p) { return (uint32_t)(p[0]&127) << 21 | (uint32_t)(p[1]&127) << 14 | (uint32_t)(p[2]&127) << 7 | (p[3]&127); }
static int at(probe_t *probe, uint64_t pos, void *dst, size_t n)
{
    if (!probe->reads_left || pos > LONG_MAX ||
        pos > (uint64_t)PROBE_MAX_HEADER_BYTES + PROBE_MP3_SCAN_BYTES + PROBE_MP3_LOOKAHEAD) return -1;
    --probe->reads_left;
    if (fseek(probe->file, (long)pos, SEEK_SET)) return -1;
    return fread(dst, 1, n, probe->file) == n ? 0 : -1;
}
static uint32_t milliseconds(uint64_t frames, uint32_t rate)
{
    uint64_t v = rate ? frames * 1000 / rate : 0;
    return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}
static size_t utf8_put(char *dst, size_t used, size_t cap, uint32_t c)
{
    if (c < 32 && c != 9) return used;
    unsigned n = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    if (used + n >= cap || c > 0x10ffff) return used;
    if (n == 1) dst[used++] = (char)c;
    else {
        dst[used++] = (char)((n == 2 ? 0xc0 : n == 3 ? 0xe0 : 0xf0) | (c >> (6 * (n - 1))));
        for (int shift = (int)(n - 2) * 6; shift >= 0; shift -= 6) dst[used++] = (char)(0x80 | ((c >> shift) & 63));
    }
    return used;
}
static void id3_text(const uint8_t *p, size_t n, char *dst, size_t cap)
{
    size_t j = 0;
    if (!n || !cap) return;
    uint8_t enc = *p++; --n;
    if (enc == 0) {
        for (size_t i = 0; i < n && p[i]; ++i) j = utf8_put(dst, j, cap, p[i]);
    } else if (enc == 3) {
        /* Keep only complete UTF-8 codepoints at the destination boundary. */
        for (size_t i = 0; i < n && p[i];) {
            unsigned len = p[i] < 128 ? 1 : (p[i]&0xe0)==0xc0 ? 2 : (p[i]&0xf0)==0xe0 ? 3 : (p[i]&0xf8)==0xf0 ? 4 : 0;
            if (!len || i + len > n || j + len >= cap) break;
            bool valid = true;
            for (unsigned k=1;k<len;k++) if ((p[i+k]&0xc0)!=0x80) valid=false;
            if (!valid) break;
            memcpy(dst+j,p+i,len); j+=len; i+=len;
        }
    } else if (enc == 1 || enc == 2) {
        bool big = enc == 2;
        if (n >= 2 && p[0] == 0xff && p[1] == 0xfe) { big=false; p+=2; n-=2; }
        else if (n >= 2 && p[0] == 0xfe && p[1] == 0xff) { big=true; p+=2; n-=2; }
        for (size_t i=0;i+1<n;i+=2) {
            uint32_t c = big ? (uint32_t)p[i]<<8 | p[i+1] : le16(p+i);
            if (!c) break;
            if (c>=0xd800 && c<=0xdbff && i+3<n) {
                uint32_t low=big ? (uint32_t)p[i+2]<<8 | p[i+3] : le16(p+i+2);
                if (low>=0xdc00 && low<=0xdfff) { c=0x10000+((c-0xd800)<<10)+(low-0xdc00); i+=2; }
                else c=0xfffd;
            } else if (c>=0xd800 && c<=0xdfff) c=0xfffd;
            j=utf8_put(dst,j,cap,c);
        }
    }
    dst[j]=0;
}
static uint64_t read_id3(probe_t *probe, audio_file_info_t *o)
{
    uint8_t h[10];
    if (at(probe,0,h,10) || memcmp(h,"ID3",3)) return 0;
    uint64_t end=10u+syncsafe(h+6);
    if (end>PROBE_MAX_HEADER_BYTES) return o->file_size;
    if (end>o->file_size || h[3]<3 || h[3]>4) return end<=o->file_size?end:0;
    unsigned version=h[3];
    uint64_t audio_start=end;
    if (version==4 && (h[5]&0x10) && end+10<=o->file_size) audio_start+=10;
    uint64_t offset=10;
    if (h[5]&0x80) return audio_start; /* Entire-tag unsynchronization: safely skip. */
    if (h[5]&0x40) {
        uint8_t ex[4];
        if (at(probe,offset,ex,4)) return audio_start;
        uint64_t ext=version==4?syncsafe(ex):4u+(uint64_t)be32(ex);
        if (ext<4 || offset+ext>end) return audio_start;
        offset+=ext;
    }
    for (unsigned frames=0; frames<PROBE_MAX_TAG_FRAMES && offset+10<=end; ++frames) {
        uint8_t fh[10];
        if (at(probe,offset,fh,10) || !fh[0]) break;
        uint32_t size=version==4?syncsafe(fh+4):be32(fh+4);
        if (!size || (uint64_t)size>end-offset-10) break;
        char *dst=NULL;
        if (!memcmp(fh,"TIT2",4)) dst=o->title;
        else if (!memcmp(fh,"TPE1",4)) dst=o->artist;
        else if (!memcmp(fh,"TALB",4)) dst=o->album;
        /* Ignore compressed/encrypted/grouped/unsynchronized text frames. */
        if (dst && !dst[0] && fh[9]==0) {
            uint8_t text[512]; size_t take=size<sizeof(text)?size:sizeof(text);
            if (!at(probe,offset+10,text,take)) id3_text(text,take,dst,128);
        }
        offset+=10u+size;
    }
    return audio_start;
}
static void read_riff_info(probe_t *probe, uint64_t pos, uint32_t size, audio_file_info_t *o)
{
    uint8_t kind[4];
    if (size<4 || at(probe,pos,kind,4) || memcmp(kind,"INFO",4)) return;
    uint64_t end=pos+size; pos+=4;
    for (unsigned chunks=0; chunks<PROBE_MAX_TAG_FRAMES && pos+8<=end; ++chunks) {
        uint8_t h[8];
        if (at(probe,pos,h,8)) break;
        uint32_t len=le32(h+4);
        if ((uint64_t)len>end-pos-8) break;
        char *dst=!memcmp(h,"INAM",4)?o->title:!memcmp(h,"IART",4)?o->artist:!memcmp(h,"IPRD",4)?o->album:NULL;
        if (dst && !dst[0]) {
            uint8_t text[129]={0}; size_t n=len<127?len:127;
            if (!at(probe,pos+8,text+1,n)) id3_text(text,n+1,dst,128);
        }
        pos+=8u+(uint64_t)len+(len&1);
    }
}
static int probe_wav(probe_t *probe, audio_file_info_t *o)
{
    uint8_t riff[12];
    if (at(probe,0,riff,12) || memcmp(riff,"RIFF",4) || memcmp(riff+8,"WAVE",4)) return -2;
    uint64_t end=8u+(uint64_t)le32(riff+4);
    if (end>o->file_size || end<12) return -2;
    bool fmt=false, data=false;
    for (uint64_t pos=12;pos+8<=end;) {
        if (!probe->reads_left || pos>PROBE_MAX_HEADER_BYTES) return -2;
        uint8_t h[8]; if (at(probe,pos,h,8)) return -1;
        uint32_t n=le32(h+4);
        uint64_t next=pos+8u+(uint64_t)n+(n&1);
        if ((uint64_t)n>end-pos-8 || next>end) return -2;
        if (!memcmp(h,"fmt ",4)) {
            uint8_t b[40]={0}; size_t count=n<sizeof(b)?n:sizeof(b);
            if (n<16 || at(probe,pos+8,b,count)) return -2;
            uint16_t tag=le16(b), channels=le16(b+2), bits=le16(b+14);
            uint32_t rate=le32(b+4);
            uint16_t align=le16(b+12);
            if (tag==0xfffe) {
                const uint8_t guid[16]={1,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
                if (n<40 || le16(b+16)<22 || memcmp(b+24,guid,16) || !le16(b+18) || le16(b+18)>bits) return -2;
            } else if (tag!=1) return -2;
            if ((channels!=1 && channels!=2) || (bits!=16 && bits!=24 && bits!=32) || rate<8000 || rate>96000 || align!=channels*(bits/8) || le32(b+8)!=rate*align) return -2;
            o->sample_rate=rate; o->channels=channels; o->bits_per_sample=bits; o->block_align=align; fmt=true;
        } else if (!memcmp(h,"data",4) && !data) {
            o->data_offset=pos+8; o->data_size=n; data=true;
        } else if (!memcmp(h,"LIST",4)) read_riff_info(probe,pos+8,n,o);
        /* Audio payload often spans hundreds of MiB. Seeking past it merely
         * for optional trailing INFO tags walks the FAT chain on some cards. */
        if (fmt && data) break;
        pos=next;
    }
    if (!fmt || !data || !o->data_size || o->data_size%o->block_align) return -2;
    o->format=AUDIO_FORMAT_WAV;
    o->total_frames=o->data_size/o->block_align;
    o->duration_ms=milliseconds(o->total_frames,o->sample_rate);
    o->bitrate_kbps=o->sample_rate*o->block_align*8/1000;
    o->duration_exact=true;
    return 0;
}
int audio_mp3_header(const uint8_t h[4], uint32_t *rate, uint16_t *channels,
                     uint16_t *samples, uint16_t *bitrate)
{
    if (h[0]!=0xff || (h[1]&0xe0)!=0xe0 || (h[1]&6)!=2) return 0;
    unsigned version=(h[1]>>3)&3, br=h[2]>>4, sr=(h[2]>>2)&3;
    if (version==1 || !br || br==15 || sr==3 || (h[3]&3)==2) return 0;
    static const uint16_t rates[3]={44100,48000,32000};
    static const uint16_t br1[15]={0,32,40,48,56,64,80,96,112,128,160,192,224,256,320};
    static const uint16_t br2[15]={0,8,16,24,32,40,48,56,64,80,96,112,128,144,160};
    uint32_t hz=rates[sr]>>(version==3?0:version==2?1:2);
    uint16_t kbps=version==3?br1[br]:br2[br];
    if (rate) *rate=hz;
    if (channels) *channels=(h[3]&0xc0)==0xc0?1:2;
    if (samples) *samples=version==3?1152:576;
    if (bitrate) *bitrate=kbps;
    return (version==3?144000:72000)*kbps/hz+((h[2]>>1)&1);
}
static int probe_mp3(probe_t *probe, audio_file_info_t *o,uint64_t start)
{
    uint64_t end=o->file_size;
    if (start>=end) return -2;
    /* Never seek to an MP3's tail during discovery: even a three-byte ID3v1
     * read can walk an entire fragmented FAT cluster chain. Trailing tags are
     * ignored by the streaming decoder. Xing/VBRI durations remain exact;
     * bitrate-derived duration remains explicitly an estimate. */
    const size_t cap=16384;
    uint8_t *buf=malloc(cap+PROBE_MP3_LOOKAHEAD); if (!buf) return -1;
    uint64_t limit=end-start>PROBE_MP3_SCAN_BYTES?start+PROBE_MP3_SCAN_BYTES:end;
    int result=-2;
    for (uint64_t base=start;base<limit;) {
        size_t n=(size_t)(end-base); if(n>cap+PROBE_MP3_LOOKAHEAD)n=cap+PROBE_MP3_LOOKAHEAD;
        size_t scan=(size_t)(limit-base); if(scan>cap)scan=cap;
        if (at(probe,base,buf,n)) { result=-1; break; }
        for (size_t i=0;i<scan && i+4<=n;i++) {
            uint32_t rate; uint16_t channels,samples,bitrate;
            int bytes=audio_mp3_header(buf+i,&rate,&channels,&samples,&bitrate);
            if (!bytes || base+i+(unsigned)bytes>end) continue;
            /* All standard Layer III frames fit in the lookahead. Candidate
             * validation uses bytes already read, never one SD seek per sync
             * pattern in a damaged file. */
            if (i+(size_t)bytes>n) continue;
            if (base+i+bytes+4<=end) {
                uint32_t next_rate; uint16_t next_channels;
                if (i+(size_t)bytes+4>n || !audio_mp3_header(buf+i+bytes,&next_rate,&next_channels,NULL,NULL) ||
                    next_rate!=rate || next_channels!=channels) continue;
            }
            o->format=AUDIO_FORMAT_MP3; o->sample_rate=rate; o->channels=channels;
            o->bits_per_sample=16; o->bitrate_kbps=bitrate;
            o->data_offset=base+i; o->data_size=end-o->data_offset;
            const uint8_t *first=buf+i;
            size_t take=(size_t)bytes;
            unsigned ver=(first[1]>>3)&3;
            unsigned side=ver==3?(channels==1?17:32):(channels==1?9:17);
            unsigned x=4+side; /* Xing/Info offset is relative to MPEG header. */
            uint32_t frame_count=0;
            if (x+12<=take && (!memcmp(first+x,"Xing",4)||!memcmp(first+x,"Info",4)) && (be32(first+x+4)&1)) frame_count=be32(first+x+8);
            if (!frame_count && 54<=take && !memcmp(first+36,"VBRI",4)) frame_count=be32(first+50);
            if (frame_count) {
                o->total_frames=(uint64_t)frame_count*samples;
                o->duration_ms=milliseconds(o->total_frames,rate); o->duration_exact=true;
                if (o->duration_ms) o->bitrate_kbps=(uint32_t)(o->data_size*8/o->duration_ms);
            } else {
                o->duration_ms=(uint32_t)(o->data_size*8/bitrate);
                o->total_frames=(uint64_t)o->duration_ms*rate/1000; o->duration_exact=false;
            }
            result=0; goto done;
        }
        base+=scan;
    }
done:
    free(buf); return result;
}
int audio_aac_header(const uint8_t h[7],uint32_t *rate,uint16_t *channels)
{
    static const uint32_t rates[13]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
    const unsigned index=(h[2]>>2)&15,channel=((h[2]&1)<<2)|(h[3]>>6);
    const unsigned length=((h[3]&3)<<11)|(h[4]<<3)|(h[5]>>5);
    if(h[0]!=255 || (h[1]&0xf6)!=0xf0 || (h[2]>>6)!=1 || index>=12 ||
       (channel!=1 && channel!=2) || (h[6]&3) || length<((h[1]&1)?7u:9u))return 0;
    if(rate)*rate=rates[index];
    if(channels)*channels=(uint16_t)channel;
    return (int)length;
}
static int probe_aac(probe_t *probe,audio_file_info_t *o,uint64_t start)
{
    if(start>=o->file_size)return -2;
    const size_t cap=16384,look=8192;
    uint8_t *buffer=malloc(cap+look);if(!buffer)return -1;
    const uint64_t end=o->file_size,limit=end-start>PROBE_MP3_SCAN_BYTES?start+PROBE_MP3_SCAN_BYTES:end;
    int result=-2;
    for(uint64_t base=start;base<limit;) {
        size_t n=(size_t)(end-base);if(n>cap+look)n=cap+look;
        size_t scan=(size_t)(limit-base);if(scan>cap)scan=cap;
        if(at(probe,base,buffer,n)){result=-1;break;}
        for(size_t i=0;i<scan && i+7<=n;i++) {
            uint32_t rate=0;uint16_t channels=0;
            int bytes=audio_aac_header(buffer+i,&rate,&channels);
            if(!bytes || base+i+(unsigned)bytes>end || i+(unsigned)bytes>n)continue;
            if(base+i+bytes+7<=end) {
                uint32_t next_rate;uint16_t next_channels;
                if(i+(unsigned)bytes+7>n || !audio_aac_header(buffer+i+bytes,&next_rate,&next_channels) ||
                   next_rate!=rate || next_channels!=channels)continue;
            }
            uint32_t sum=0,count=0;size_t pos=i;
            while(pos+7<=n && count<16) {
                uint32_t next_rate;uint16_t next_channels;
                bytes=audio_aac_header(buffer+pos,&next_rate,&next_channels);
                if(!bytes || pos+(unsigned)bytes>n || next_rate!=rate || next_channels!=channels)break;
                sum+=(unsigned)bytes;++count;pos+=(unsigned)bytes;
            }
            if(!count)continue;
            o->format=AUDIO_FORMAT_AAC;o->sample_rate=rate;o->channels=channels;o->bits_per_sample=16;
            o->data_offset=base+i;o->data_size=end-o->data_offset;
            o->bitrate_kbps=(uint32_t)((uint64_t)sum*8*rate/(count*1024u*1000u));
            o->total_frames=o->data_size*count*1024/sum;o->duration_ms=milliseconds(o->total_frames,rate);
            o->duration_exact=false;result=0;goto done;
        }
        base+=scan;
    }
done:
    free(buffer);return result;
}
static void flac_comments(probe_t *probe,uint64_t pos,uint32_t length,audio_file_info_t *o)
{
    uint8_t word[4];const uint64_t end=pos+length;
    if(length<8 || at(probe,pos,word,4))return;
    uint32_t vendor=le32(word);pos+=4;
    if(vendor>end-pos || end-pos-vendor<4)return;
    pos+=vendor;if(at(probe,pos,word,4))return;
    uint32_t count=le32(word);pos+=4;if(count>64)count=64;
    for(uint32_t i=0;i<count && pos+4<=end;i++) {
        if(at(probe,pos,word,4))break;
        uint32_t len=le32(word);pos+=4;if(len>end-pos)break;
        uint8_t text[257];size_t take=len<256?len:256;
        if(at(probe,pos,text,take))break;
        text[take]=0;
        char *equals=strchr((char*)text,'=');
        if(equals) {
            for(char *p=(char*)text;p<equals;p++)if(*p>='a' && *p<='z')*p-=32;
            *equals=0;char *dst=!strcmp((char*)text,"TITLE")?o->title:!strcmp((char*)text,"ARTIST")?o->artist:!strcmp((char*)text,"ALBUM")?o->album:NULL;
            if(dst) { *equals=3;id3_text((uint8_t*)equals,take-(size_t)(equals-(char*)text),dst,128); }
        }
        pos+=len;
    }
}
static int probe_flac(probe_t *probe,audio_file_info_t *o)
{
    uint64_t pos=4;bool info=false,last=false;
    for(unsigned blocks=0;blocks<64 && !last;blocks++) {
        uint8_t h[4];if(pos+4>o->file_size || pos>PROBE_MAX_HEADER_BYTES || at(probe,pos,h,4))return -2;
        const unsigned type=h[0]&127;const uint32_t length=(uint32_t)h[1]<<16|(uint32_t)h[2]<<8|h[3];
        last=(h[0]&128)!=0;pos+=4;
        if(length>o->file_size-pos || length>PROBE_MAX_HEADER_BYTES || pos+length>PROBE_MAX_HEADER_BYTES)return -2;
        if(!blocks && type!=0)return -2;
        if(type==0) {
            uint8_t b[34];if(info || length!=34 || at(probe,pos,b,34))return -2;
            const unsigned min_block=(unsigned)b[0]<<8|b[1],max_block=(unsigned)b[2]<<8|b[3];
            o->sample_rate=be32(b+10)>>12;o->channels=((b[12]>>1)&7)+1;
            o->bits_per_sample=(((b[12]&1)<<4)|(b[13]>>4))+1;
            o->total_frames=(uint64_t)(b[13]&15)<<32|be32(b+14);
            if(min_block<16 || max_block<min_block || o->sample_rate<8000 || o->sample_rate>96000 ||
               (o->channels!=1 && o->channels!=2) || (o->bits_per_sample!=16 && o->bits_per_sample!=24 && o->bits_per_sample!=32))return -2;
            info=true;
        } else if(type==4)flac_comments(probe,pos,length,o);
        pos+=length;
    }
    if(!info || !last || pos>=o->file_size)return -2;
    o->format=AUDIO_FORMAT_FLAC;o->data_offset=pos;o->data_size=o->file_size-pos;
    o->duration_exact=o->total_frames!=0;o->duration_ms=milliseconds(o->total_frames,o->sample_rate);
    if(o->duration_ms)o->bitrate_kbps=(uint32_t)(o->data_size*8/o->duration_ms);
    return 0;
}
int audio_probe_stream(FILE *file, audio_file_info_t *out)
{
    if (!file || !out) return -1;
    memset(out,0,sizeof(*out));
    /* FatFs f_stat/fstat obtains the directory-entry size without f_lseek's
     * cluster-chain traversal. fseek(SEEK_END) is intentionally absent. */
#if defined(_WIN32)
    struct _stat64 status;
    if (_fstat64(_fileno(file),&status)) return -1;
#else
    struct stat status;
    if (fstat(fileno(file),&status)) return -1;
#endif
    if (status.st_size<0) return -3;
    uint64_t size=(uint64_t)status.st_size;
    /* IDF's newlib/FAT VFS uses signed 32-bit off_t in standard builds. */
    if ((uint64_t)size>INT32_MAX) return -3;
    out->file_size=size;
    probe_t probe={file,PROBE_MAX_READS};
    uint8_t sig[12];
    if (size<12 || at(&probe,0,sig,12)) return -2;
    if(!memcmp(sig,"RIFF",4))return probe_wav(&probe,out);
    if(!memcmp(sig,"fLaC",4))return probe_flac(&probe,out);
    const uint64_t start=!memcmp(sig,"ID3",3)?read_id3(&probe,out):0;
    if(start>=size || size-start<7 || at(&probe,start,sig,7))return -2;
    return audio_aac_header(sig,NULL,NULL)?probe_aac(&probe,out,start):probe_mp3(&probe,out,start);
}
int audio_probe_file(const char *path, audio_file_info_t *out)
{
    if (!path || !out) return -1;
    FILE *f=fopen(path,"rb"); if (!f) return -1;
    int result=audio_probe_stream(f,out); fclose(f); return result;
}
static uint32_t random32(uint32_t *state)
{
    uint32_t v=*state?*state:0x91e10da5u;
    v^=v<<13;v^=v>>17;v^=v<<5;*state=v;return v;
}
void audio_pcm_to_stereo16(const uint8_t *src,size_t frames,uint16_t bits,
                           uint16_t channels,uint32_t *rng,int16_t *dst)
{
    unsigned bytes=bits/8;
    for(size_t i=0;i<frames;i++) {
        for(unsigned ch=0;ch<channels;ch++) {
            int64_t v;
            if(bits==16)v=(int16_t)le16(src);
            else {
                int32_t raw;
                if(bits==24) { uint32_t t=le16(src)|(uint32_t)src[2]<<16;raw=(int32_t)(t&0x800000?t|0xff000000u:t); }
                else raw=(int32_t)le32(src);
                unsigned shift=bits-16; uint32_t mask=(1u<<shift)-1;
                int32_t noise=(int32_t)(random32(rng)&mask)-(int32_t)(random32(rng)&mask);
                v=((int64_t)raw+noise+(1u<<(shift-1)))>>shift;
            }
            if(v>32767)v=32767;
            if(v<-32768)v=-32768;
            dst[i*2+ch]=(int16_t)v;src+=bytes;
        }
        if(channels==1)dst[i*2+1]=dst[i*2];
    }
}
