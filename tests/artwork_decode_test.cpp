#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <set>
#include <vector>
static size_t read_bytes=0,read_calls=0,short_limit=0;
static int eintr_left=0;
static std::set<int> opened;
static int artwork_open(const char *p,int f) { int fd=open(p,f);if(fd>=0)opened.insert(fd);return fd; }
static int artwork_close(int fd) { opened.erase(fd);return close(fd); }
static auto artwork_read(int fd,void *p,size_t n)->decltype(read(fd,p,n)) {
    ++read_calls;if(eintr_left>0) { --eintr_left;errno=EINTR;return -1; }
    if(short_limit&&n>short_limit)n=short_limit;
    auto got=read(fd,p,n);if(got>0)read_bytes+=static_cast<size_t>(got);return got;
}
#define read artwork_read
#define open artwork_open
#define close artwork_close
#include "player_artwork_decode.cpp"
#undef read
#undef open
#undef close
struct Gate { unsigned calls=0,limit=~0U; };
static bool proceed(void *v,bool) { auto &g=*static_cast<Gate *>(v);return ++g.calls<=g.limit; }
static void reset() { assert(opened.empty());read_bytes=read_calls=short_limit=0;eintr_left=0; }
static void output(player_artwork_image_t *im,const char *path) {
    FILE*f=fopen(path,"wb");assert(f);assert(fwrite(im->image.data,1,im->image.data_size,f)==im->image.data_size);fclose(f);
}
int main(int argc,char **argv) {
    assert(argc>=2);lv_init();
    const std::string base=argv[1];
    FILE *manifest=fopen((base+"/manifest.tsv").c_str(),"r");assert(manifest);
    char name[100],ext[12],color[16];unsigned bound;unsigned cases=0;
    while(fscanf(manifest,"%99s %11s %15s %u",name,ext,color,&bound)==4) {
        reset();Gate gate;const std::string path=base+"/"+name+"/track."+ext;
        auto *im=player_artwork_load(path.c_str(),proceed,&gate);
        assert(opened.empty());assert(jpeg_budget.used==0);
        if(!strcmp(color,"none"))assert(!im);
        else {
            if(!im) { fprintf(stderr,"FAIL no cover: %s\n",name);return 2; }
            assert(im->image.header.w==312&&im->image.header.h==312);
            if(!strcmp(color,"red")||!strcmp(color,"green")) {
                const uint16_t *pixels=reinterpret_cast<const uint16_t *>(im->image.data);
                for(unsigned i=0;i<312*312;++i) {
                    unsigned r=(pixels[i]>>11)*255/31,g=((pixels[i]>>5)&63)*255/63,b=(pixels[i]&31)*255/31;
                    assert(b<80);
                    if(!strcmp(color,"red"))assert(r>190&&g<80);else assert(g>190&&r<80);
                }
            }
            output(im,(base+"/"+name+".rgb565").c_str());player_artwork_image_free(im);
        }
        if(read_bytes>bound) { fprintf(stderr,"FAIL read bound %s: %zu > %u\n",name,read_bytes,bound);return 3; }
        printf("PASS %s bytes=%zu reads=%zu callbacks=%u jpeg_peak=%zu\n",name,read_bytes,read_calls,gate.calls,jpeg_budget.peak);++cases;
    }
    fclose(manifest);
    for(const char *name:{"large-tag","progressive","flac-front","wav-trailing","v23-global-unsync"}) {
        const char *ext=!strcmp(name,"flac-front")?"flac":!strcmp(name,"wav-trailing")?"wav":"mp3";
        const std::string path=base+"/"+name+"/track."+ext;
        reset();Gate all;auto *im=player_artwork_load(path.c_str(),proceed,&all);assert(im);player_artwork_image_free(im);
        const unsigned limits[]={0,1,2,5,10,20,all.calls/2,all.calls-1};
        for(unsigned limit:limits) {
            reset();Gate stop{0,limit};im=player_artwork_load(path.c_str(),proceed,&stop);
            assert(!im&&opened.empty()&&jpeg_budget.used==0);
        }
    }
    reset();short_limit=7;eintr_left=2;Gate gate;
    auto *im=player_artwork_load((base+"/apic-other/track.mp3").c_str(),proceed,&gate);
    assert(im&&opened.empty());player_artwork_image_free(im);
    jpeg_budget={};
    assert(!artwork_jpeg_malloc(JPEG_MEMORY_LIMIT)&&jpeg_budget.used==0);
    void *memory=artwork_jpeg_malloc(JPEG_MEMORY_LIMIT-sizeof(JpegAllocation));
    assert(memory&&jpeg_budget.used==JPEG_MEMORY_LIMIT&&!artwork_jpeg_malloc(1));
    artwork_jpeg_free(memory);assert(jpeg_budget.used==0);
    // Corrupt/truncate real production decoder input, not a parser model.
    const std::string fuzz_path=base+"/progressive/track.mp3";
    FILE *seed=fopen(fuzz_path.c_str(),"rb");assert(seed);
    std::vector<unsigned char> bytes;int ch;
    while((ch=fgetc(seed))!=EOF)bytes.push_back(static_cast<unsigned char>(ch));fclose(seed);
    // JPEG starts after the 10-byte ID3 header, 10-byte APIC header and 14-byte prefix.
    const size_t start=34;assert(bytes.size()>start+40);
    unsigned fuzz_cases=0;
    for(size_t cut=8;cut<bytes.size()-start;cut+=23) {
        Gate fg;Context fc{proceed,&fg};
        auto *damaged=decode(bytes.data()+start,cut,fc);player_artwork_image_free(damaged);
        assert(jpeg_budget.used==0&&jpeg_budget.peak<=JPEG_MEMORY_LIMIT);++fuzz_cases;
    }
    printf("PASS %u progressive truncation boundaries and exact JPEG memory-budget exhaustion.\n",fuzz_cases);
    if(argc>=3) {
        reset();Gate real;im=player_artwork_load(argv[2],proceed,&real);assert(im&&opened.empty());
        output(im,(base+"/real.rgb565").c_str());player_artwork_image_free(im);
        printf("PASS real user MP3 bytes=%zu callbacks=%u JPEG peak=%zu\n",read_bytes,real.calls,jpeg_budget.peak);
    }
    printf("PASS %u parser/decode fixtures, cancellation at 40 boundaries, short reads/EINTR, no open descriptors or JPEG scratch allocations.\n",cases);
}
