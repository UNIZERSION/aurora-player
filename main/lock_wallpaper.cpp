#include "lock_wallpaper.h"
#include "lvgl.h"
#define LODEPNG_NO_COMPILE_CPP
#include "src/libs/lodepng/lodepng.h"
#include "vendor/tjpgd/tjpgd.h"
#include "asset_pack.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#ifndef O_BINARY
#define O_BINARY 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

namespace {
constexpr char DIRECTORY[]="/sdcard/Aurora/Lockscreen";
constexpr size_t MAX_FILE_BYTES=8U*1024U*1024U;
constexpr size_t MAX_PNG_PIXELS=1024U*1024U;
constexpr size_t MAX_DECODE_BYTES=10U*1024U*1024U;
[[maybe_unused]] constexpr size_t AUDIO_RESERVE=2U*1024U*1024U;
constexpr size_t OUTPUT_BYTES=LOCK_WALLPAPER_WIDTH*LOCK_WALLPAPER_HEIGHT*2U;
constexpr unsigned MAX_VISITED=2048;
struct Context {
    lock_wallpaper_continue_cb proceed; void *user; bool cancelled=false, memory_failed=false;
    bool wallpaper=true;
#ifdef ESP_PLATFORM
    int64_t last_yield=0;
#endif
    bool ready(bool io=false) {
        if(cancelled)return false;
        if(proceed&&!proceed(user,io)){cancelled=true;return false;}
#ifdef ESP_PLATFORM
        const int64_t now=esp_timer_get_time();
        if(now-last_yield>=4000){vTaskDelay(1);last_yield=esp_timer_get_time();}
#endif
        return true;
    }
};
bool enough_memory(size_t bytes) {
    if(bytes>MAX_DECODE_BYTES)return false;
#ifdef ESP_PLATFORM
    const size_t free_bytes=heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    return free_bytes>AUDIO_RESERVE&&bytes<=free_bytes-AUDIO_RESERVE;
#else
    return true;
#endif
}
void *allocate(size_t bytes) {
    if(!enough_memory(bytes))return nullptr;
#ifdef ESP_PLATFORM
    return heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
    return malloc(bytes);
#endif
}
struct Allocation {
    void *data=nullptr;
    explicit Allocation(size_t bytes):data(allocate(bytes)){}
    ~Allocation(){free(data);}
    Allocation(const Allocation &)=delete;
    Allocation &operator=(const Allocation &)=delete;
};
struct File {
    int fd=-1; size_t size=0;
    explicit File(const char *path) {
        fd=open(path,O_RDONLY|O_BINARY|O_NOFOLLOW);
        struct stat info{};
        if(fd>=0&&(fstat(fd,&info)!=0||!S_ISREG(info.st_mode)||info.st_size<=0)){
            close(fd);fd=-1;
        }
        if(fd>=0)size=uint64_t(info.st_size)>MAX_FILE_BYTES?MAX_FILE_BYTES+1:static_cast<size_t>(info.st_size);
    }
    ~File(){if(fd>=0)close(fd);}
};
uint32_t be32(const unsigned char *p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
uint16_t rgb565(unsigned r,unsigned g,unsigned b){return uint16_t((r>>3)<<11|(g>>2)<<5|(b>>3));}
uint16_t backdrop565(unsigned r,unsigned g,unsigned b) {
    // Exact LVGL RGB565 opacity-90 mix, baked once instead of blending almost
    // one million PSRAM pixels on every lock-screen refresh.
    const uint32_t pixel=rgb565(r,g,b), veil=rgb565(5,9,9), mask=0x07e0f81f;
    const uint32_t background=(pixel|(pixel<<16))&mask;
    const uint32_t foreground=(veil|(veil<<16))&mask;
    const uint32_t mixed=((((foreground-background)*((90+4)>>3))>>5)+background)&mask;
    return uint16_t(mixed>>16)|uint16_t(mixed);
}
void rgb888(uint16_t p,unsigned &r,unsigned &g,unsigned &b){r=((p>>11)*255+15)/31;g=(((p>>5)&63)*255+31)/63;b=((p&31)*255+15)/31;}
lock_wallpaper_image_t failure(lock_wallpaper_error_t error){lock_wallpaper_image_t r;r.error=error;return r;}
lock_wallpaper_image_t new_image() {
    lock_wallpaper_image_t result;
    result.pixels=static_cast<uint16_t *>(allocate(OUTPUT_BYTES));
    if(!result.pixels)return failure(LOCK_WALLPAPER_NO_MEMORY);
    result.width=LOCK_WALLPAPER_WIDTH;result.height=LOCK_WALLPAPER_HEIGHT;
    return result;
}
bool read_exact(int fd,unsigned char *out,size_t bytes,Context &ctx) {
    // SD DMA destinations stay in aligned internal stack memory.
    alignas(64) unsigned char staging[1024];
    while(bytes){
        if(!ctx.ready(true))return false;
        const auto got=read(fd,staging,std::min(bytes,sizeof staging));
        if(got<0&&errno==EINTR)continue;
        if(got<=0)return false;
        memcpy(out,staging,static_cast<size_t>(got));out+=got;bytes-=static_cast<size_t>(got);
    }
    return true;
}
bool make_path(char *path,size_t size,const char *directory,const char *name) {
    if(!lock_wallpaper_valid_name(name))return false;
    const int n=snprintf(path,size,"%s/%s",directory,name);
    return n>=0&&static_cast<size_t>(n)<size;
}
struct Crop {
    unsigned left,top,width,height;
    Crop(unsigned w,unsigned h):left(0),top(0),width(w),height(h) {
        if(uint64_t(w)*9>uint64_t(h)*16)width=std::max(1U,h*16/9);
        else height=std::max(1U,w*9/16);
        left=(w-width)/2;top=(h-height)/2;
    }
};
struct Jpeg {
    File &file;Context &ctx;size_t position=0;Crop crop{1,1};uint16_t *pixels=nullptr;
};
size_t jpeg_read(JDEC *decoder,uint8_t *buffer,size_t count) {
    auto &j=*static_cast<Jpeg *>(decoder->device);
    if(!j.ctx.ready(true)||j.position>j.file.size)return 0;
    count=std::min(count,j.file.size-j.position);
    if(buffer){if(!read_exact(j.file.fd,buffer,count,j.ctx))return 0;}
    else if(lseek(j.file.fd,static_cast<off_t>(count),SEEK_CUR)<0)return 0;
    j.position+=count;return count;
}
int jpeg_write(JDEC *decoder,void *bitmap,JRECT *area) {
    auto &j=*static_cast<Jpeg *>(decoder->device);
    if(!j.ctx.ready())return 0;
    const auto &g=j.crop;const unsigned block_width=area->right-area->left+1;
    auto destination=[](unsigned source,unsigned origin,unsigned extent,unsigned output){
        return source<=origin?0U:std::min(output,((source-origin)*output+extent-1)/extent);
    };
    const unsigned x0=destination(area->left,g.left,g.width,LOCK_WALLPAPER_WIDTH);
    const unsigned x1=destination(area->right+1,g.left,g.width,LOCK_WALLPAPER_WIDTH);
    const unsigned y0=destination(area->top,g.top,g.height,LOCK_WALLPAPER_HEIGHT);
    const unsigned y1=destination(area->bottom+1,g.top,g.height,LOCK_WALLPAPER_HEIGHT);
    const auto *source=static_cast<const unsigned char *>(bitmap);
    for(unsigned y=y0;y<y1;++y){
        if(!j.ctx.ready())return 0;
        const unsigned sy=g.top+y*g.height/LOCK_WALLPAPER_HEIGHT-area->top;
        for(unsigned x=x0;x<x1;++x){
            const unsigned sx=g.left+x*g.width/LOCK_WALLPAPER_WIDTH-area->left;
            const auto *p=source+(sy*block_width+sx)*3;
            j.pixels[y*LOCK_WALLPAPER_WIDTH+x]=j.ctx.wallpaper?backdrop565(p[0],p[1],p[2]):rgb565(p[0],p[1],p[2]);
        }
    }
    return 1;
}
lock_wallpaper_image_t decode_jpeg(File &file,Context &ctx) {
    JDEC decoder{};Allocation work(4096);if(!work.data)return failure(LOCK_WALLPAPER_NO_MEMORY);
    Jpeg jpeg{file,ctx};
    const JRESULT prepared=jd_prepare(&decoder,jpeg_read,work.data,4096,&jpeg);
    if(ctx.cancelled)return failure(LOCK_WALLPAPER_CANCELLED);
    if(prepared!=JDR_OK)return failure(prepared==JDR_FMT2||prepared==JDR_FMT3?LOCK_WALLPAPER_UNSUPPORTED:LOCK_WALLPAPER_CORRUPT);
    if(!decoder.width||!decoder.height||decoder.width>4096||decoder.height>4096||
       uint64_t(decoder.width)*decoder.height>16U*1024U*1024U)return failure(LOCK_WALLPAPER_TOO_LARGE);
    unsigned scale=0;
    while(scale<3&&(decoder.width>>(scale+1))>=LOCK_WALLPAPER_WIDTH&&
          (decoder.height>>(scale+1))>=LOCK_WALLPAPER_HEIGHT)++scale;
    jpeg.crop=Crop(decoder.width>>scale,decoder.height>>scale);
    auto result=new_image();if(!result.pixels)return result;
    jpeg.pixels=result.pixels;
    const JRESULT decoded=jd_decomp(&decoder,jpeg_write,scale);
    if(decoded!=JDR_OK||!ctx.ready()){
        lock_wallpaper_image_free(&result);
        return failure(ctx.cancelled?LOCK_WALLPAPER_CANCELLED:LOCK_WALLPAPER_CORRUPT);
    }
    return result;
}
// Decode raw PNG colour ourselves after LodePNG's bounded unfiltering, avoiding
// its second 4-byte-per-pixel conversion buffer. Only 1/2/4/8-bit PNG is allowed.
void png_pixel(const unsigned char *data,size_t index,const LodePNGColorMode &mode,unsigned out[3],bool wallpaper=true) {
    unsigned r=0,g=0,b=0,a=255;
    if(mode.colortype==LCT_RGB||mode.colortype==LCT_RGBA){
        const auto *p=data+index*(mode.colortype==LCT_RGB?3:4);r=p[0];g=p[1];b=p[2];
        if(mode.colortype==LCT_RGBA)a=p[3];
        else if(mode.key_defined&&r==mode.key_r&&g==mode.key_g&&b==mode.key_b)a=0;
    }else if(mode.colortype==LCT_GREY_ALPHA){r=g=b=data[index*2];a=data[index*2+1];}
    else {
        const unsigned bits=mode.bitdepth;
        const unsigned value=bits==8?data[index]:(data[index*bits/8]>>(8-bits-(index*bits%8)))&((1U<<bits)-1);
        if(mode.colortype==LCT_PALETTE){
            if(value<mode.palettesize){const auto *p=mode.palette+value*4;r=p[0];g=p[1];b=p[2];a=p[3];}
        }else{r=g=b=value*255/((1U<<bits)-1);if(mode.key_defined&&value==mode.key_r)a=0;}
    }
    // Transparent wallpapers flatten against the same neutral milk tint.
    out[0]=(r*a+(wallpaper?244U:0U)*(255-a)+127)/255;
    out[1]=(g*a+(wallpaper?242U:0U)*(255-a)+127)/255;
    out[2]=(b*a+(wallpaper?238U:0U)*(255-a)+127)/255;
}
// LodePNG's default inflater grows its allocation near the end of a stream.
// Reuse Aurora's ROM tinfl adapter with a fixed upper bound instead, so realloc
// cannot temporarily consume a second large buffer. The source and output are
// sliced only to create scheduler/cancellation checkpoints; history is retained
// in the complete non-wrapping output allocation.
unsigned png_inflate(unsigned char **out,size_t *out_size,const unsigned char *input,
                     size_t input_size,const LodePNGDecompressSettings *settings) {
    auto &ctx=*static_cast<Context *>(const_cast<void *>(settings->custom_context));
    *out=nullptr;*out_size=0;
    const size_t capacity=settings->max_output_size;
    if(!capacity||capacity>MAX_PNG_PIXELS*4+4096*8+4096)return 109;
    if(!ctx.ready())return 110;
    if(!enough_memory(capacity)){ctx.memory_failed=true;return 83;}
    *out=static_cast<unsigned char *>(lv_malloc(capacity));
    if(!*out){ctx.memory_failed=true;return 83;}
    using namespace aurora_asset_zlib;
    Allocation state_memory(sizeof(tinfl_decompressor));
    if(!state_memory.data){ctx.memory_failed=true;return 83;}
    auto *state=static_cast<tinfl_decompressor *>(state_memory.data);memset(state,0,sizeof *state);
    size_t consumed=0;
    for(;;){
        if(!ctx.ready())return 110;
        size_t in_count=std::min(size_t(4096),input_size-consumed);
        size_t out_count=std::min(size_t(16384),capacity-*out_size);
        const auto status=tinfl_decompress(state,input+consumed,&in_count,*out,*out+*out_size,&out_count,
            TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF|TINFL_FLAG_HAS_MORE_INPUT);
        consumed+=in_count;*out_size+=out_count;
        if(status==TINFL_STATUS_DONE)return consumed==input_size?0:110;
        // With an exact-capacity ICC profile, allow a final zero-output step
        // to consume its zlib checksum. A stream needing more pixels stalls
        // without writing past capacity and is rejected by the progress check.
        if(status<0||(!in_count&&!out_count))return 110;
    }
}
lock_wallpaper_image_t decode_png(File &file,const unsigned char header[33],Context &ctx) {
    const unsigned width=be32(header+16),height=be32(header+20);
    if(memcmp(header+12,"IHDR",4)||be32(header+8)!=13)return failure(LOCK_WALLPAPER_CORRUPT);
    if(!width||!height||width>4096||height>4096||uint64_t(width)*height>MAX_PNG_PIXELS)return failure(LOCK_WALLPAPER_TOO_LARGE);
    if(header[24]>8)return failure(LOCK_WALLPAPER_UNSUPPORTED);
    const size_t pixels=size_t(width)*height;
    // IDAT copy + inflated scanlines, or scanlines + raw 4-byte draw buffer.
    // Include output, palettes/Huffman tables, interlace row overhead and ICC.
    const size_t peak=std::max(file.size*2+pixels*4,file.size+pixels*8)+OUTPUT_BYTES+256U*1024U;
    if(!enough_memory(peak))return failure(LOCK_WALLPAPER_NO_MEMORY);
    Allocation encoded(file.size);if(!encoded.data)return failure(LOCK_WALLPAPER_NO_MEMORY);
    if(lseek(file.fd,0,SEEK_SET)<0||!read_exact(file.fd,static_cast<unsigned char *>(encoded.data),file.size,ctx))
        return failure(ctx.cancelled?LOCK_WALLPAPER_CANCELLED:LOCK_WALLPAPER_CORRUPT);
    LodePNGState state;lodepng_state_init(&state);
    state.decoder.color_convert=0;state.decoder.read_text_chunks=0;state.decoder.remember_unknown_chunks=0;
    state.decoder.max_icc_size=65536;
    state.decoder.zlibsettings.max_output_size=pixels*4+height*8+4096;
    state.decoder.zlibsettings.custom_zlib=png_inflate;
    state.decoder.zlibsettings.custom_context=&ctx;
    unsigned char *raw=nullptr;unsigned decoded_w=0,decoded_h=0;
    const unsigned error=lodepng_decode(&raw,&decoded_w,&decoded_h,&state,static_cast<unsigned char *>(encoded.data),file.size);
    auto *decoded=reinterpret_cast<lv_draw_buf_t *>(raw);
    lock_wallpaper_image_t result;
    if(ctx.cancelled)result=failure(LOCK_WALLPAPER_CANCELLED);
    else if(error||!decoded||decoded_w!=width||decoded_h!=height)result=failure(error==83||ctx.memory_failed?LOCK_WALLPAPER_NO_MEMORY:LOCK_WALLPAPER_CORRUPT);
    else if(!ctx.ready())result=failure(LOCK_WALLPAPER_CANCELLED);
    else{
        result=new_image();const Crop crop(width,height);
        if(result.pixels)for(unsigned y=0;y<LOCK_WALLPAPER_HEIGHT;++y){
            if(!ctx.ready()){lock_wallpaper_image_free(&result);result=failure(LOCK_WALLPAPER_CANCELLED);break;}
            // Bilinear sampling keeps low-resolution custom images smooth.
            const unsigned fy=LOCK_WALLPAPER_HEIGHT==1?0:y*(crop.height-1)*256/(LOCK_WALLPAPER_HEIGHT-1);
            const unsigned sy=crop.top+fy/256,sy1=std::min(sy+1,crop.top+crop.height-1),wy=fy%256;
            for(unsigned x=0;x<LOCK_WALLPAPER_WIDTH;++x){
                const unsigned fx=x*(crop.width-1)*256/(LOCK_WALLPAPER_WIDTH-1);
                const unsigned sx=crop.left+fx/256,sx1=std::min(sx+1,crop.left+crop.width-1),wx=fx%256;
                unsigned p[4][3],rgb[3];png_pixel(decoded->data,size_t(sy)*width+sx,state.info_png.color,p[0],ctx.wallpaper);
                png_pixel(decoded->data,size_t(sy)*width+sx1,state.info_png.color,p[1],ctx.wallpaper);
                png_pixel(decoded->data,size_t(sy1)*width+sx,state.info_png.color,p[2],ctx.wallpaper);
                png_pixel(decoded->data,size_t(sy1)*width+sx1,state.info_png.color,p[3],ctx.wallpaper);
                for(unsigned c=0;c<3;++c)rgb[c]=((p[0][c]*(256-wx)+p[1][c]*wx)*(256-wy)+(p[2][c]*(256-wx)+p[3][c]*wx)*wy+32768)/65536;
                result.pixels[y*LOCK_WALLPAPER_WIDTH+x]=ctx.wallpaper?backdrop565(rgb[0],rgb[1],rgb[2]):rgb565(rgb[0],rgb[1],rgb[2]);
            }
        }
    }
    if(decoded)lv_draw_buf_destroy(decoded);
    lodepng_state_cleanup(&state);return result;
}
void list_directory(const char *directory,lock_wallpaper_list_t *out,Context &ctx) {
    if(!out)return;
    *out={};
    if(!ctx.ready(true)){out->error=LOCK_WALLPAPER_CANCELLED;return;}
    DIR *dir=opendir(directory);if(!dir){out->error=LOCK_WALLPAPER_UNAVAILABLE;return;}
    unsigned visited=0;char path[512];
    while(auto *entry=readdir(dir)){
        if(!ctx.ready(true)){out->error=LOCK_WALLPAPER_CANCELLED;out->count=0;break;}
        if(++visited>MAX_VISITED){out->truncated=true;break;}
        if(!make_path(path,sizeof path,directory,entry->d_name))continue;
#ifdef DT_LNK
        if(entry->d_type==DT_LNK)continue;
#endif
        struct stat info{};
        if(stat(path,&info)!=0||!S_ISREG(info.st_mode)||info.st_size<=0||uint64_t(info.st_size)>MAX_FILE_BYTES)continue;
        if(out->count==LOCK_WALLPAPER_MAX_FILES){out->truncated=true;break;}
        strcpy(out->names[out->count++],entry->d_name);
    }
    closedir(dir);
    qsort(out->names,out->count,sizeof out->names[0],[](const void *a,const void *b){return strcmp(static_cast<const char *>(a),static_cast<const char *>(b));});
}
lock_wallpaper_image_t load_directory(const char *directory,const char *name,Context &ctx) {
    char path[512];if(!make_path(path,sizeof path,directory,name))return failure(LOCK_WALLPAPER_INVALID_NAME);
    if(!ctx.ready(true))return failure(LOCK_WALLPAPER_CANCELLED);
    File file(path);if(file.fd<0)return failure(LOCK_WALLPAPER_UNAVAILABLE);
    if(file.size>MAX_FILE_BYTES)return failure(LOCK_WALLPAPER_TOO_LARGE);
    unsigned char header[33]{};
    if(file.size<sizeof header||!read_exact(file.fd,header,sizeof header,ctx))return failure(ctx.cancelled?LOCK_WALLPAPER_CANCELLED:LOCK_WALLPAPER_CORRUPT);
    if(lseek(file.fd,0,SEEK_SET)<0)return failure(LOCK_WALLPAPER_CORRUPT);
    if(header[0]==0xff&&header[1]==0xd8)return decode_jpeg(file,ctx);
    if(!memcmp(header,"\x89PNG\r\n\x1a\n",8))return decode_png(file,header,ctx);
    return failure(LOCK_WALLPAPER_UNSUPPORTED);
}
lock_wallpaper_image_t load_startup_directory(const char *directory,Context &ctx) {
    ctx.wallpaper=false;
    auto image=load_directory(directory,"startup.jpg",ctx);
    if(image.pixels||ctx.cancelled||image.error==LOCK_WALLPAPER_NO_MEMORY)return image;
    auto png=load_directory(directory,"startup.png",ctx);
    if(png.pixels||png.error!=LOCK_WALLPAPER_UNAVAILABLE)return png;
    return image;
}
bool blur_pass(const unsigned char *source,unsigned char *target,bool horizontal,Context &ctx) {
    constexpr unsigned W=320,H=180,R=6,N=R*2+1;
    const unsigned lines=horizontal?H:W,extent=horizontal?W:H;
    for(unsigned line=0;line<lines;++line){
        if(!ctx.ready())return false;
        auto index=[&](unsigned position){return (horizontal?line*W+position:position*W+line)*3;};
        unsigned sum[3]{};
        for(int k=-int(R);k<=int(R);++k){const size_t at=index(static_cast<unsigned>(std::max(0,k)));for(unsigned c=0;c<3;++c)sum[c]+=source[at+c];}
        for(unsigned p=0;p<extent;++p){
            const size_t at=index(p);for(unsigned c=0;c<3;++c)target[at+c]=static_cast<unsigned char>((sum[c]+N/2)/N);
            const size_t leave=index(p<R?0:p-R),enter=index(std::min(extent-1,p+R+1));
            for(unsigned c=0;c<3;++c)sum[c]=sum[c]-source[leave+c]+source[enter+c];
        }
    }
    return true;
}
}

bool lock_wallpaper_valid_name(const char *name) {
    if(!name||!*name||name[0]=='.')return false;
    const size_t length=strnlen(name,LOCK_WALLPAPER_NAME_BYTES);
    if(length>=LOCK_WALLPAPER_NAME_BYTES||name[length-1]==' '||name[length-1]=='.'||strstr(name,".."))return false;
    for(size_t i=0;i<length;++i){const unsigned char c=static_cast<unsigned char>(name[i]);if(c<32||c==127||c=='/'||c=='\\'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|')return false;}
    const char *extension=strrchr(name,'.');if(!extension)return false;
    char lower[6]{};const size_t n=strlen(extension);if(n>=sizeof lower)return false;
    for(size_t i=0;i<n;++i)lower[i]=static_cast<char>(tolower(static_cast<unsigned char>(extension[i])));
    return !strcmp(lower,".jpg")||!strcmp(lower,".jpeg")||!strcmp(lower,".png");
}
void lock_wallpaper_list(lock_wallpaper_list_t *out,lock_wallpaper_continue_cb proceed,void *user){Context ctx{proceed,user};list_directory(DIRECTORY,out,ctx);}
lock_wallpaper_image_t lock_wallpaper_load(const char *name,lock_wallpaper_continue_cb proceed,void *user){Context ctx{proceed,user};return load_directory(DIRECTORY,name,ctx);}
lock_wallpaper_image_t startup_artwork_load(lock_wallpaper_continue_cb proceed,void *user){Context ctx{proceed,user};return load_startup_directory("/sdcard/Aurora",ctx);}
lock_wallpaper_image_t lock_wallpaper_frosted(const uint16_t *snapshot,size_t stride,lock_wallpaper_continue_cb proceed,void *user) {
    if(!snapshot||stride<1280||stride>4096)return failure(LOCK_WALLPAPER_INVALID_FRAME);
    Context ctx{proceed,user};if(!ctx.ready())return failure(LOCK_WALLPAPER_CANCELLED);
    constexpr unsigned W=320,H=180;constexpr size_t bytes=W*H*3;
    if(!enough_memory(OUTPUT_BYTES+bytes*2))return failure(LOCK_WALLPAPER_NO_MEMORY);
    Allocation first(bytes),second(bytes);if(!first.data||!second.data)return failure(LOCK_WALLPAPER_NO_MEMORY);
    auto *a=static_cast<unsigned char *>(first.data),*b=static_cast<unsigned char *>(second.data);
    for(unsigned y=0;y<H;++y){
        if(!ctx.ready())return failure(LOCK_WALLPAPER_CANCELLED);
        for(unsigned x=0;x<W;++x){
            unsigned sum[3]{};
            for(unsigned yy=0;yy<4;++yy)for(unsigned xx=0;xx<4;++xx){unsigned r,g,blue;rgb888(snapshot[(y*4+yy)*stride+x*4+xx],r,g,blue);sum[0]+=r;sum[1]+=g;sum[2]+=blue;}
            for(unsigned c=0;c<3;++c)a[(y*W+x)*3+c]=static_cast<unsigned char>((sum[c]+8)/16);
        }
    }
    // Three separable box passes approximate a soft blur, with edge extension.
    for(unsigned pass=0;pass<3;++pass)if(!blur_pass(a,b,true,ctx)||!blur_pass(b,a,false,ctx))return failure(LOCK_WALLPAPER_CANCELLED);
    auto result=new_image();if(!result.pixels)return result;
    for(unsigned y=0;y<LOCK_WALLPAPER_HEIGHT;++y){
        if(!ctx.ready()){lock_wallpaper_image_free(&result);return failure(LOCK_WALLPAPER_CANCELLED);}
        const unsigned sy=y/4,sy1=std::min(H-1,sy+1),wy=y%4;
        for(unsigned x=0;x<LOCK_WALLPAPER_WIDTH;++x){
            const unsigned sx=x/4,sx1=std::min(W-1,sx+1),wx=x%4;unsigned rgb[3];
            for(unsigned c=0;c<3;++c)rgb[c]=(a[(sy*W+sx)*3+c]*(4-wx)*(4-wy)+a[(sy*W+sx1)*3+c]*wx*(4-wy)+a[(sy1*W+sx)*3+c]*(4-wx)*wy+a[(sy1*W+sx1)*3+c]*wx*wy+8)/16;
            const unsigned luma=(rgb[0]*77+rgb[1]*150+rgb[2]*29+128)/256;
            constexpr unsigned milk[3]={244,242,238};
            for(unsigned c=0;c<3;++c)rgb[c]=(((rgb[c]*70+luma*30+50)/100)*38+milk[c]*62+50)/100;
            result.pixels[y*LOCK_WALLPAPER_WIDTH+x]=backdrop565(rgb[0],rgb[1],rgb[2]);
        }
    }
    return result;
}
void lock_wallpaper_image_free(lock_wallpaper_image_t *image){if(image){free(image->pixels);*image={};}}
const char *lock_wallpaper_error_text(lock_wallpaper_error_t error) {
    switch(error){
    case LOCK_WALLPAPER_OK:return "";
    case LOCK_WALLPAPER_INVALID_NAME:return "Invalid wallpaper name";
    case LOCK_WALLPAPER_UNAVAILABLE:return "Wallpaper folder or file unavailable";
    case LOCK_WALLPAPER_TOO_LARGE:return "Image too large (JPEG: 16 MP; PNG: 1 MP; file: 8 MB)";
    case LOCK_WALLPAPER_UNSUPPORTED:return "Unsupported image; use baseline JPEG or 8-bit PNG";
    case LOCK_WALLPAPER_CORRUPT:return "Image could not be decoded";
    case LOCK_WALLPAPER_NO_MEMORY:return "Not enough free memory for this wallpaper";
    case LOCK_WALLPAPER_CANCELLED:return "Wallpaper load cancelled";
    case LOCK_WALLPAPER_INVALID_FRAME:return "Main-screen capture unavailable";
    }
    return "Wallpaper unavailable";
}
