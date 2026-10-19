#include "player_artwork_decode.h"
#define LODEPNG_NO_COMPILE_CPP
#include "src/libs/lodepng/lodepng.h"
#include "vendor/tjpgd/tjpgd.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#ifndef O_BINARY
#define O_BINARY 0
#endif
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

// JPEG-only compatibility fallback for legal sampling layouts and progressive
// scans unsupported by TJpgDec. All scratch allocations live in PSRAM on Tab5.
struct JpegBudget {
    player_artwork_continue_cb proceed=nullptr;void *user=nullptr;
    size_t used=0,peak=0;bool aborted=false;
};
static thread_local JpegBudget jpeg_budget;
static constexpr size_t JPEG_MEMORY_LIMIT=13U*1024U*1024U;
struct alignas(16) JpegAllocation { size_t bytes; };
static bool artwork_jpeg_continue() {
    if(jpeg_budget.aborted)return false;
    if(jpeg_budget.proceed&&!jpeg_budget.proceed(jpeg_budget.user,false))jpeg_budget.aborted=true;
    return !jpeg_budget.aborted;
}
static void *artwork_jpeg_malloc(size_t bytes) {
    if(bytes>JPEG_MEMORY_LIMIT-sizeof(JpegAllocation)||jpeg_budget.used>JPEG_MEMORY_LIMIT-bytes-sizeof(JpegAllocation)||!artwork_jpeg_continue())return nullptr;
#ifdef ESP_PLATFORM
    auto *p=static_cast<JpegAllocation *>(heap_caps_malloc(bytes+sizeof(JpegAllocation),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
#else
    auto *p=static_cast<JpegAllocation *>(malloc(bytes+sizeof(JpegAllocation)));
#endif
    if(!p) { return nullptr; }
    p->bytes=bytes+sizeof(JpegAllocation);jpeg_budget.used+=p->bytes;
    jpeg_budget.peak=std::max(jpeg_budget.peak,jpeg_budget.used);return p+1;
}
static void artwork_jpeg_free(void *memory) {
    if(!memory) { return; }
    auto *p=static_cast<JpegAllocation *>(memory)-1;
    jpeg_budget.used-=p->bytes;free(p);
}
[[maybe_unused]] static void *artwork_jpeg_realloc(void *old,size_t size) {
    if(!old)return artwork_jpeg_malloc(size);
    auto *p=static_cast<JpegAllocation *>(old)-1;void *replacement=artwork_jpeg_malloc(size);
    if(replacement) { memcpy(replacement,old,std::min(size,p->bytes-sizeof(JpegAllocation)));artwork_jpeg_free(old); }
    return replacement;
}
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_MAX_DIMENSIONS 4096
#define STBI_MALLOC artwork_jpeg_malloc
#define STBI_REALLOC artwork_jpeg_realloc
#define STBI_FREE artwork_jpeg_free
#define STBI_AURORA_CONTINUE artwork_jpeg_continue
#if defined(__GNUC__)
#pragma GCC diagnostic push
// JPEG-only static stb declarations include optional APIs not built here.
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "vendor/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace {
constexpr size_t MAX_ENCODED = 2U * 1024U * 1024U;
constexpr size_t MAX_PNG_PIXELS = 1024U * 1024U;
constexpr unsigned EDGE = PLAYER_ARTWORK_EDGE;
struct Buffer {
    unsigned char *data{};size_t size{};
    ~Buffer() { free(data); }
    bool allocate(size_t bytes) {
        if(!bytes || bytes > MAX_ENCODED) { return false; }
        free(data);data=static_cast<unsigned char *>(malloc(bytes));size=data?bytes:0;
        return data!=nullptr;
    }
};
struct Context {
    player_artwork_continue_cb proceed;void *user;
    mutable bool cancelled=false;
    bool ready(bool io=true) const {
        if(cancelled) { return false; }
        if(proceed&&!proceed(user,io)) { cancelled=true;return false; }
        return true;
    }
};
uint32_t be32(const unsigned char *p) { return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3]; }
uint32_t le32(const unsigned char *p) { return uint32_t(p[3])<<24|uint32_t(p[2])<<16|uint32_t(p[1])<<8|p[0]; }
uint32_t syncsafe(const unsigned char *p) {
    if((p[0]|p[1]|p[2]|p[3])&0x80) { return UINT32_MAX; }
    return uint32_t(p[0])<<21|uint32_t(p[1])<<14|uint32_t(p[2])<<7|p[3];
}
bool read_exact(int file,unsigned char *out,size_t bytes,const Context &context) {
    // Keep every bulk SD destination in aligned internal task-stack memory.
    // Encoded image storage itself can live in PSRAM.
    alignas(64) unsigned char staging[4096];
    while(bytes) {
        if(!context.ready()) { return false; }
        const size_t count=std::min(bytes,sizeof(staging));
        // Use one VFS read per chunk. P4 newlib's unbuffered fread was observed
        // to turn a4096B request into4096 one-byte VFS calls.
        const auto got=read(file,staging,count);
        if(got<0&&errno==EINTR) { continue; }
        if(got<=0) { return false; }
        memcpy(out,staging,static_cast<size_t>(got));out+=got;bytes-=static_cast<size_t>(got);
    }
    return true;
}
size_t deunsync(unsigned char *p,size_t bytes) {
    size_t out=0;
    for(size_t n=0;n<bytes;++n) {
        p[out++]=p[n];
        if(p[n]==0xff&&n+1<bytes&&p[n+1]==0) { ++n; }
    }
    return out;
}
bool signature(const unsigned char *p,size_t size) {
    return size>=8&&((p[0]==0xff&&p[1]==0xd8)||memcmp(p,"\x89PNG\r\n\x1a\n",8)==0);
}
// Returns the JPEG/PNG payload in an APIC/PIC frame, skipping its text fields.
unsigned char *picture_payload(unsigned char *p,size_t &size,bool v22,int &kind) {
    if(size<6) { return nullptr; }
    const unsigned encoding=p[0];size_t pos=1;
    if(encoding>3) { return nullptr; }
    if(v22) { if(memcmp(p+pos,"-->",3)==0)return nullptr;pos+=3; }
    else {
        const size_t start=pos;
        while(pos<size&&p[pos]) { ++pos; }
        if(pos-start==3&&memcmp(p+start,"-->",3)==0)return nullptr;
        ++pos;
    }
    if(pos>=size) { return nullptr; }
    kind=p[pos++];
    if(encoding==0||encoding==3) {
        while(pos<size&&p[pos]) { ++pos; }++pos;
    } else {
        while(pos+1<size&&(p[pos]||p[pos+1])) { pos+=2; }pos+=2;
    }
    if(pos>=size||!signature(p+pos,size-pos)) { return nullptr; }
    size-=pos;return p+pos;
}
// All offsets are checked against the physical file and the enclosing chunk.
// Seeking over audio, padding and unrelated metadata does not read those bytes.
struct File {
    int fd=-1;uint64_t bytes=0;const Context &context;
    File(const char *path,const Context &c):context(c) {
        if(!context.ready()) { return; }
        fd=open(path,O_RDONLY|O_BINARY);if(fd<0) { return; }
        const off_t end=lseek(fd,0,SEEK_END);
        if(end<0) { close(fd);fd=-1;return; }bytes=uint64_t(end);
    }
    ~File() { if(fd>=0) { close(fd); } }
    bool at(uint64_t offset,unsigned char *out,size_t count) const {
        return fd>=0&&offset<=bytes&&count<=bytes-offset&&context.ready()
            &&lseek(fd,static_cast<off_t>(offset),SEEK_SET)>=0
            &&read_exact(fd,out,count,context);
    }
};
struct Source {
    const File &file;uint64_t base;size_t size;const unsigned char *memory=nullptr;
    bool at(size_t offset,unsigned char *out,size_t count) const {
        if(offset>size||count>size-offset) { return false; }
        if(memory) { if(!file.context.ready(false))return false;memcpy(out,memory+offset,count);return true; }
        return file.at(base+offset,out,count);
    }
};
struct Tag { uint64_t start=0;size_t bytes=0;unsigned version=0,flags=0; };
bool locate_id3(const File &file,Tag &tag) {
    unsigned char h[12];uint64_t start=0,limit=file.bytes;
    if(!file.at(0,h,10)) { return false; }
    if(memcmp(h,"RIFF",4)==0) {
        if(!file.at(0,h,12)||memcmp(h+8,"WAVE",4)) { return false; }
        const uint64_t end=uint64_t(le32(h+4))+8;
        if(end<12||end>file.bytes) { return false; }
        uint64_t pos=12;bool found=false;
        for(unsigned n=0;n<256&&pos+8<=end;++n) {
            if(!file.at(pos,h,8)) { return false; }
            const uint32_t count=le32(h+4);pos+=8;
            if(count>end-pos) { return false; }
            if(memcmp(h,"id3 ",4)==0||memcmp(h,"ID3 ",4)==0) {
                start=pos;limit=pos+count;found=true;break;
            }
            pos+=uint64_t(count)+(count&1U);
        }
        if(!found||limit-start<10||!file.at(start,h,10)) { return false; }
    }
    if(memcmp(h,"ID3",3)||h[3]<2||h[3]>4||h[4]==0xff) { return false; }
    const uint32_t bytes=syncsafe(h+6);
    if(bytes==UINT32_MAX||start+10>limit||bytes>limit-start-10) { return false; }
    tag={start+10,bytes,h[3],h[5]};return true;
}
player_artwork_image_t *new_image() {
    auto *result=static_cast<player_artwork_image_t *>(calloc(1,sizeof(player_artwork_image_t)));
    if(!result) { return nullptr; }
    constexpr size_t bytes=EDGE*EDGE*2;
#ifdef ESP_PLATFORM
    result->allocation=heap_caps_malloc(bytes+63,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
    result->allocation=malloc(bytes+63);
#endif
    if(!result->allocation) { free(result);return nullptr; }
    auto *data=reinterpret_cast<unsigned char *>((reinterpret_cast<uintptr_t>(result->allocation)+63)&~uintptr_t(63));
    memset(data,0,bytes);
    auto &d=result->image;d.header.magic=LV_IMAGE_HEADER_MAGIC;d.header.cf=LV_COLOR_FORMAT_RGB565;
    d.header.w=EDGE;d.header.h=EDGE;d.header.stride=EDGE*2;d.data=data;d.data_size=bytes;
    return result;
}
struct Geometry {
    unsigned side,left,top;
    Geometry(unsigned w,unsigned h) {
        side=std::min(w,h);left=(w-side)/2;top=(h-side)/2;
    }
};
uint16_t rgb565(unsigned r,unsigned g,unsigned b) { return uint16_t((r>>3)<<11|(g>>2)<<5|(b>>3)); }
struct Jpeg {
    const unsigned char *data;size_t size,pos;const Context *context;
    player_artwork_image_t *image;Geometry geometry;unsigned width,height;
};
size_t jpeg_input(JDEC *decoder,uint8_t *buffer,size_t count) {
    auto &j=*static_cast<Jpeg *>(decoder->device);
    if(!j.context->ready(false))return 0;
    count=std::min(count,j.size-j.pos);
    if(buffer) { memcpy(buffer,j.data+j.pos,count); }j.pos+=count;return count;
}
int jpeg_output(JDEC *decoder,void *bitmap,JRECT *area) {
    auto &j=*static_cast<Jpeg *>(decoder->device);
    if(!j.context->ready(false)) { return 0; }
    const auto &g=j.geometry;const unsigned block_width=area->right-area->left+1;
    auto *source=static_cast<const unsigned char *>(bitmap);
    auto *target=const_cast<uint16_t *>(reinterpret_cast<const uint16_t *>(j.image->image.data));
    // Center crop rather than baking letterbox bars into the album raster.
    // Map only samples owned by this MCU; this is bounded by EDGE*EDGE even
    // for extremely narrow source images.
    auto destination=[&](unsigned source,unsigned origin) {
        return source<=origin?0U:std::min(EDGE,((source-origin)*EDGE+g.side-1)/g.side);
    };
    const unsigned x0=destination(area->left,g.left),x1=destination(area->right+1,g.left);
    const unsigned y0=destination(area->top,g.top),y1=destination(area->bottom+1,g.top);
    for(unsigned y=y0;y<y1;++y) {
        const unsigned sy=g.top+y*g.side/EDGE-area->top;
        for(unsigned x=x0;x<x1;++x) {
            const unsigned sx=g.left+x*g.side/EDGE-area->left;
            const unsigned char *p=source+(sy*block_width+sx)*3;
            // The private upstream TJpgDec emits RGB888 (LVGL's fork is BGR).
            target[y*EDGE+x]=rgb565(p[0],p[1],p[2]);
        }
    }
    return 1;
}
player_artwork_image_t *decode_compatible_jpeg(const unsigned char *data,size_t bytes,const Context &context) {
    jpeg_budget={[](void *p,bool)->bool { return static_cast<const Context *>(p)->ready(false); },const_cast<Context *>(&context)};
    int width=0,height=0,channels=0;
    if(!stbi_info_from_memory(data,static_cast<int>(bytes),&width,&height,&channels)||width<=0||height<=0||uint64_t(width)*height>4U*1024U*1024U)return nullptr;
    unsigned char *rgb=stbi_load_from_memory(data,static_cast<int>(bytes),&width,&height,&channels,3);
    if(!rgb)return nullptr;
    auto *image=new_image();
    if(image) {
        const Geometry g(width,height);auto *target=const_cast<uint16_t *>(reinterpret_cast<const uint16_t *>(image->image.data));
        for(unsigned y=0;y<EDGE;++y) {
            if(!context.ready(false)) { player_artwork_image_free(image);image=nullptr;break; }
            for(unsigned x=0;x<EDGE;++x) {
                const auto *p=rgb+(size_t(g.top+y*g.side/EDGE)*width+g.left+x*g.side/EDGE)*3;
                target[y*EDGE+x]=rgb565(p[0],p[1],p[2]);
            }
        }
    }
    stbi_image_free(rgb);return image;
}
player_artwork_image_t *decode(const unsigned char *data,size_t bytes,const Context &context) {
    if(!signature(data,bytes)||!context.ready(false)) { return nullptr; }
    if(data[0]==0xff) {
        JDEC decoder{};alignas(4) unsigned char work[4096];
        Jpeg jpeg{data,bytes,0,&context,nullptr,Geometry(1,1),0,0};
        const JRESULT prepared=jd_prepare(&decoder,jpeg_input,work,sizeof work,&jpeg);
        if(prepared!=JDR_OK) { return context.ready(false)?decode_compatible_jpeg(data,bytes,context):nullptr; }
        if(!decoder.width||!decoder.height||decoder.width>4096||decoder.height>4096)return nullptr;
        unsigned scale=0;
        // Never undersample the final crop: decode at the largest supported
        // power-of-two reduction whose short edge still covers all 312 pixels.
        while(scale<3&&(std::min(decoder.width,decoder.height)>>(scale+1))>=EDGE) { ++scale; }
        jpeg.width=decoder.width>>scale;jpeg.height=decoder.height>>scale;jpeg.geometry=Geometry(jpeg.width,jpeg.height);
        jpeg.image=new_image();if(!jpeg.image) { return nullptr; }
        if(jd_decomp(&decoder,jpeg_output,scale)!=JDR_OK) { player_artwork_image_free(jpeg.image);return nullptr; }
        return jpeg.image;
    }
    // Reject decompression bombs before allocating the full PNG raster.
    if(bytes<33||memcmp(data+12,"IHDR",4)!=0) { return nullptr; }
    const unsigned width=be32(data+16),height=be32(data+20);
    if(!width||!height||width>2048||height>2048||uint64_t(width)*height>MAX_PNG_PIXELS||data[24]>8) { return nullptr; }
    unsigned char *png_result=nullptr;unsigned decoded_w=0,decoded_h=0;
    LodePNGState state;lodepng_state_init(&state);
    state.decoder.read_text_chunks=0;state.decoder.remember_unknown_chunks=0;
    state.decoder.max_icc_size=65536;
    state.decoder.zlibsettings.max_output_size=size_t(width)*height*6+height*8+1024;
    const unsigned error=lodepng_decode(&png_result,&decoded_w,&decoded_h,&state,data,bytes);
    lodepng_state_cleanup(&state);
    // LVGL9.3's LodePNG fork returns an lv_draw_buf_t, not the upstream raw
    // allocation. Its pixels are RGBA until lv_lodepng's separate swizzle.
    auto *decoded=reinterpret_cast<lv_draw_buf_t *>(png_result);
    if(error||!decoded||!context.ready(false)||width!=decoded_w||height!=decoded_h) {
        if(decoded) { lv_draw_buf_destroy(decoded); }return nullptr;
    }
    const unsigned char *rgba=decoded->data;
    auto *image=new_image();if(!image) { lv_draw_buf_destroy(decoded);return nullptr; }
    const Geometry g(width,height);auto *target=const_cast<uint16_t *>(reinterpret_cast<const uint16_t *>(image->image.data));
    for(unsigned y=0;y<EDGE;++y) {
        if(!context.ready(false)) { lv_draw_buf_destroy(decoded);player_artwork_image_free(image);return nullptr; }
        for(unsigned x=0;x<EDGE;++x) {
            const auto *p=rgba+(size_t(g.top+y*g.side/EDGE)*width+g.left+x*g.side/EDGE)*4;
            target[y*EDGE+x]=rgb565(p[0]*p[3]/255,p[1]*p[3]/255,p[2]*p[3]/255);
        }
    }
    lv_draw_buf_destroy(decoded);return image;
}
// Deterministic type preference shared by ID3 PIC/APIC and FLAC PICTURE.
// Icons are usable only when no album/other photograph can be decoded.
unsigned picture_rank(unsigned kind) {
    if(kind==3)return 0;
    if(kind==0)return 1;
    if(kind==4)return 2;
    if(kind==5)return 3;
    if(kind==6)return 4;
    if(kind==1||kind==2)return 100;
    return kind<=20?5:50;
}
struct Candidate { size_t offset,bytes;unsigned rank,flags; };
struct Candidates {
    Candidate items[32]{};unsigned count=0;
    void add(Candidate item) {
        unsigned at=0;while(at<count&&items[at].rank<=item.rank) { ++at; }
        if(at>=32)return;
        const unsigned end=std::min(count,31U);
        for(unsigned i=end;i>at;--i)items[i]=items[i-1];
        items[at]=item;if(count<32)++count;
    }
};
unsigned char *apic_payload(unsigned char *p,size_t &bytes,unsigned version,unsigned tag_flags,unsigned frame_flags,int &kind) {
    if(version==3&&(frame_flags&0xc0))return nullptr;
    if(version==4&&(frame_flags&0x0c))return nullptr; // compression/encryption unsupported
    if(version==4&&((tag_flags&0x80)||(frame_flags&2)))bytes=deunsync(p,bytes);
    const size_t prefix=version==3?((frame_flags&0x20)?1U:0U):version==4?((frame_flags&0x40)?1U:0U)+((frame_flags&1)?4U:0U):0;
    if(prefix>=bytes)return nullptr;
    p+=prefix;bytes-=prefix;return picture_payload(p,bytes,version==2,kind);
}
player_artwork_image_t *from_tag(const File &file,const Tag &tag,const Context &context) {
    if(tag.version==2&&(tag.flags&0x40))return nullptr;
    Buffer unsynced;
    Source body{file,tag.start,tag.bytes};
    if(tag.version<4&&(tag.flags&0x80)) {
        // Global v2.2/v2.3 unsynchronisation changes even frame-header offsets.
        // Preserve a bounded compatibility path; ordinary tags never read it all.
        if(!unsynced.allocate(tag.bytes)||!body.at(0,unsynced.data,tag.bytes))return nullptr;
        unsynced.size=deunsync(unsynced.data,tag.bytes);
        body.size=unsynced.size;body.memory=unsynced.data;
    }
    size_t pos=0;unsigned char header[10];
    if(tag.version>=3&&(tag.flags&0x40)) {
        if(!body.at(0,header,4))return nullptr;
        const uint32_t n=tag.version==4?syncsafe(header):be32(header);
        if(n==UINT32_MAX||n<6)return nullptr;
        const uint64_t skip=uint64_t(n)+(tag.version==3?4:0);
        if(skip>body.size)return nullptr;
        pos=static_cast<size_t>(skip);
    }
    Candidates candidates;Buffer probe;
    if(!probe.allocate(4096))return nullptr;
    const size_t hsize=tag.version==2?6:10;
    for(unsigned n=0;n<1024&&pos<=body.size&&hsize<=body.size-pos;++n) {
        if(!body.at(pos,header,hsize))return nullptr;
        if(!header[0])break;
        bool valid=true;
        for(unsigned k=0;k<(tag.version==2?3U:4U);++k)valid&=(header[k]>='A'&&header[k]<='Z')||(header[k]>='0'&&header[k]<='9');
        if(!valid)break;
        const uint32_t bytes=tag.version==2?(uint32_t(header[3])<<16|uint32_t(header[4])<<8|header[5]):tag.version==4?syncsafe(header+4):be32(header+4);
        pos+=hsize;
        if(bytes==UINT32_MAX||bytes>body.size-pos)break;
        const bool picture=memcmp(header,tag.version==2?"PIC":"APIC",tag.version==2?3:4)==0;
        const unsigned flags=tag.version==2?0:header[9];
        if(picture&&bytes&&bytes<=MAX_ENCODED) {
            size_t prefix=std::min(size_t(bytes),probe.size);
            if(!body.at(pos,probe.data,prefix))return nullptr;
            int kind=0;
            if(apic_payload(probe.data,prefix,tag.version,tag.flags,flags,kind))
                candidates.add({pos,bytes,picture_rank(kind),flags});
        }
        pos+=bytes;
    }
    for(unsigned n=0;n<candidates.count&&context.ready(false);++n) {
        const auto &c=candidates.items[n];Buffer encoded;
        if(!encoded.allocate(c.bytes)||!body.at(c.offset,encoded.data,c.bytes))continue;
        size_t bytes=c.bytes;int kind=0;
        if(auto *p=apic_payload(encoded.data,bytes,tag.version,tag.flags,c.flags,kind))
            if(auto *image=decode(p,bytes,context))return image;
    }
    return nullptr;
}
player_artwork_image_t *flac_cover(const File &file,const Context &context) {
    unsigned char h[20];
    if(!file.at(0,h,4)||memcmp(h,"fLaC",4))return nullptr;
    uint64_t pos=4;Candidates candidates;
    for(unsigned n=0;n<256&&context.ready();++n) {
        if(!file.at(pos,h,4))break;
        const bool last=(h[0]&0x80)!=0;const unsigned type=h[0]&0x7f;
        const uint32_t bytes=uint32_t(h[1])<<16|uint32_t(h[2])<<8|h[3];pos+=4;
        if(pos>file.bytes||bytes>file.bytes-pos)break;
        if(type==6&&bytes>=32) {
            Source block{file,pos,bytes};size_t at=8;
            if(!block.at(0,h,8))return nullptr;
            const unsigned kind=be32(h);const uint32_t mime=be32(h+4);
            bool valid=mime<=bytes-at;
            if(valid&&mime==3) { if(!block.at(at,h,3))return nullptr;valid=memcmp(h,"-->",3)!=0; }
            if(valid) {
                at+=mime;
                if(block.at(at,h,4)) {
                    const uint32_t description=be32(h);at+=4;
                    if(description<=bytes-at) {
                        at+=description;
                        if(block.at(at,h,20)) {
                            const uint32_t length=be32(h+16);at+=20;
                            if(length>=8&&length<=MAX_ENCODED&&length<=bytes-at&&block.at(at,h,8)&&signature(h,8))
                                candidates.add({static_cast<size_t>(pos+at),length,picture_rank(kind),0});
                        }
                    }
                }
            }
        }
        pos+=bytes;if(last)break;
    }
    for(unsigned n=0;n<candidates.count&&context.ready(false);++n) {
        const auto &c=candidates.items[n];Buffer encoded;
        if(encoded.allocate(c.bytes)&&file.at(c.offset,encoded.data,c.bytes))
            if(auto *image=decode(encoded.data,c.bytes,context))return image;
    }
    return nullptr;
}
int cover_rank(const char *name) {
    static const char *const names[]={"cover.jpg","cover.jpeg","cover.png","folder.jpg","folder.jpeg","folder.png","front.jpg","front.jpeg","front.png"};
    char lower[24];const size_t length=strlen(name);if(length>=sizeof lower) { return -1; }
    for(size_t n=0;n<=length;++n) { lower[n]=static_cast<char>(tolower(static_cast<unsigned char>(name[n]))); }
    for(int n=0;n<9;++n) { if(strcmp(lower,names[n])==0) { return n; } }return -1;
}
player_artwork_image_t *folder_cover(const char *track,const Context &context) {
    const char *slash=strrchr(track,'/');if(!slash) { return nullptr; }
    const std::string directory(track,static_cast<size_t>(slash-track));
    if(!context.ready()) { return nullptr; }
    DIR *dir=opendir(directory.c_str());if(!dir) { return nullptr; }
    int rank=99;std::string chosen;unsigned visited=0;
    while(auto *entry=readdir(dir)) {
        if((visited++%16)==0&&!context.ready()) { closedir(dir);return nullptr; }
        const int candidate=cover_rank(entry->d_name);
        if(candidate>=0&&candidate<rank) { chosen=directory+"/"+entry->d_name;rank=candidate;if(!rank) { break; } }
        if(visited>20000) { break; }
    }
    closedir(dir);if(chosen.empty()||!context.ready()) { return nullptr; }
    const int file=open(chosen.c_str(),O_RDONLY|O_BINARY);if(file<0) { return nullptr; }
    Buffer encoded;const off_t length=lseek(file,0,SEEK_END);
    const bool ok=length>0&&uint64_t(length)<=MAX_ENCODED&&lseek(file,0,SEEK_SET)==0&&encoded.allocate(static_cast<size_t>(length))&&read_exact(file,encoded.data,encoded.size,context);
    close(file);return ok?decode(encoded.data,encoded.size,context):nullptr;
}
}

player_artwork_image_t *player_artwork_load(const char *track_path,player_artwork_continue_cb proceed,void *user) {
    if(!track_path||!*track_path||strlen(track_path)>=512) { return nullptr; }
    const Context context{proceed,user};
    {
        File file(track_path,context);Tag tag;
        if(locate_id3(file,tag)) {
            if(auto *image=from_tag(file,tag,context))return image;
        }
        if(auto *image=flac_cover(file,context))return image;
    }
    return context.ready()?folder_cover(track_path,context):nullptr;
}
void player_artwork_image_free(player_artwork_image_t *image) {
    if(image) { free(image->allocation);free(image); }
}
player_artwork_image_t *player_artwork_image_clone(const player_artwork_image_t *image) {
    if(!image)return nullptr;
    auto *copy=new_image();
    if(copy)memcpy(const_cast<uint8_t*>(copy->image.data),image->image.data,EDGE*EDGE*2);
    return copy;
}
