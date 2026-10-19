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
static std::set<int> opened;
static size_t read_bytes=0,short_limit=0;
static unsigned interrupts=0;
static int tracked_open(const char *name,int flags){int fd=open(name,flags);if(fd>=0)opened.insert(fd);return fd;}
static int tracked_close(int fd){opened.erase(fd);return close(fd);}
static auto tracked_read(int fd,void *out,size_t bytes)->decltype(read(fd,out,bytes)) {
    if(interrupts){--interrupts;errno=EINTR;return -1;}
    if(short_limit)bytes=std::min(bytes,short_limit);
    auto n=read(fd,out,bytes);if(n>0)read_bytes+=static_cast<size_t>(n);return n;
}
#define open tracked_open
#define close tracked_close
#define read tracked_read
#include "lock_wallpaper.cpp"
#undef open
#undef close
#undef read
#include "aurora_startup.hpp"
struct Gate { unsigned calls=0,limit=~0U; };
static bool proceed(void *p,bool){auto &g=*static_cast<Gate *>(p);return ++g.calls<=g.limit;}
static lock_wallpaper_image_t load(const std::string &root,const char *name,Gate &gate){
    Context ctx{proceed,&gate};auto image=load_directory(root.c_str(),name,ctx);assert(opened.empty());return image;
}
static void solid(const lock_wallpaper_image_t &image,unsigned r,unsigned g,unsigned b,unsigned tolerance=10){
    assert(image.error==LOCK_WALLPAPER_OK&&image.pixels&&image.width==1280&&image.height==720);
    const auto expected=lv_color_16_16_mix(rgb565(5,9,9),rgb565(r,g,b),90);
    rgb888(expected,r,g,b);
    for(unsigned i=0;i<1280*720;++i){unsigned rr,gg,bb;rgb888(image.pixels[i],rr,gg,bb);
        assert(abs(int(rr)-int(r))<=int(tolerance));assert(abs(int(gg)-int(g))<=int(tolerance));assert(abs(int(bb)-int(b))<=int(tolerance));}
}
int main(int argc,char **argv){
    lv_init();
    if(argc==4&&!strcmp(argv[1],"--frost")){
        std::vector<uint16_t> input(1280*720);FILE *source=fopen(argv[2],"rb");assert(source);
        assert(fread(input.data(),2,input.size(),source)==input.size());assert(fgetc(source)==EOF);fclose(source);
        auto result=lock_wallpaper_frosted(input.data());assert(result.pixels&&result.error==LOCK_WALLPAPER_OK);
        FILE *destination=fopen(argv[3],"wb");assert(destination);
        assert(fwrite(result.pixels,1,OUTPUT_BYTES,destination)==OUTPUT_BYTES);assert(fclose(destination)==0);
        lock_wallpaper_image_free(&result);puts("PASS generated production frosted backdrop (1280x720 RGB565 with precomposited veil)");return 0;
    }
    assert(argc==2);const std::string root=argv[1];
    // Compare all possible source RGB565 values against the actual LVGL
    // compositor: the optimized backdrop must retain the old veil appearance.
    for(unsigned pixel=0;pixel<=65535;++pixel){unsigned r,g,b;rgb888(uint16_t(pixel),r,g,b);
        assert(backdrop565(r,g,b)==lv_color_16_16_mix(rgb565(5,9,9),uint16_t(pixel),90));}
    puts("PASS native 1280x720 backdrop and exact LVGL veil blend for all 65536 RGB565 colors");
    for(const char *good:{"Photo.JPG","cover.jpeg","a.png","a b.png","żółć.png"})assert(lock_wallpaper_valid_name(good));
    for(const char *bad:{"",".jpg","..","../cover.jpg","sub/cover.jpg","sub\\cover.jpg","C:cover.jpg","a..b.png","a.png ","a.png.","a\n.png","cover.gif"})assert(!lock_wallpaper_valid_name(bad));
    const std::string long_name=std::string(236,'a')+".jpg";
    assert(lock_wallpaper_valid_name(long_name.c_str()));assert(!lock_wallpaper_valid_name(("x"+long_name).c_str()));
    for(const auto &c:std::vector<std::pair<const char *,std::vector<unsigned>>>{
        {"red.jpg",{220,32,16}},{"green.png",{20,220,32}},{"alpha.png",{250,121,119}},{"full-alpha.png",{250,121,119}},
        {"one-bit.png",{255,255,255}},{"palette.png",{0,0,255}},{"gray.png",{64,64,64}},{"icc-limit.png",{20,220,32}},
        {"gray-alpha.png",{154,153,151}},{"narrow.jpg",{220,32,16}},{"sixteen-mp.jpg",{220,32,16}},
        {"wide.jpg",{0,255,0}},{"portrait.png",{0,0,255}},{"portrait.jpg",{0,0,255}}}){
        Gate gate;read_bytes=0;auto image=load(root,c.first,gate);solid(image,c.second[0],c.second[1],c.second[2]);
        printf("PASS %s callbacks=%u read_bytes=%zu\n",c.first,gate.calls,read_bytes);lock_wallpaper_image_free(&image);assert(!image.pixels);
    }
    for(const auto &c:std::vector<std::pair<const char *,lock_wallpaper_error_t>>{
        {"progressive.jpg",LOCK_WALLPAPER_UNSUPPORTED},{"sixteen.png",LOCK_WALLPAPER_UNSUPPORTED},
        {"wide.png",LOCK_WALLPAPER_TOO_LARGE},{"too-wide.jpg",LOCK_WALLPAPER_TOO_LARGE},
        {"bomb.png",LOCK_WALLPAPER_TOO_LARGE},{"large.jpg",LOCK_WALLPAPER_TOO_LARGE},
        {"memory.png",LOCK_WALLPAPER_NO_MEMORY},{"corrupt.png",LOCK_WALLPAPER_CORRUPT},{"inflate-bomb.png",LOCK_WALLPAPER_CORRUPT},
        {"truncated.jpg",LOCK_WALLPAPER_CORRUPT},{"corrupt.jpg",LOCK_WALLPAPER_UNSUPPORTED},
        {"absent.jpg",LOCK_WALLPAPER_UNAVAILABLE},{"../red.jpg",LOCK_WALLPAPER_INVALID_NAME}}){
        Gate gate;read_bytes=0;auto image=load(root,c.first,gate);
        if(image.error!=c.second){fprintf(stderr,"FAIL %s error=%d expected=%d\n",c.first,image.error,c.second);return 2;}
        assert(!image.pixels);if(c.second==LOCK_WALLPAPER_TOO_LARGE||c.second==LOCK_WALLPAPER_NO_MEMORY)assert(read_bytes<=8192);
        printf("PASS rejection %s (%s)\n",c.first,lock_wallpaper_error_text(image.error));
    }
    for(const char *name:{"red.jpg","green.png","palette.png"}){
        Gate all;auto image=load(root,name,all);assert(image.pixels);lock_wallpaper_image_free(&image);
        for(unsigned limit:{0U,1U,2U,10U,all.calls/2,all.calls-1}){
            Gate stop{0,limit};image=load(root,name,stop);assert(!image.pixels&&image.error==LOCK_WALLPAPER_CANCELLED);
        }
    }
    short_limit=7;interrupts=2;Gate gate;auto image=load(root,"red.jpg",gate);solid(image,220,32,16);lock_wallpaper_image_free(&image);short_limit=0;
    auto *list=new lock_wallpaper_list_t;Context ctx{nullptr,nullptr};
    list_directory((root+"/listing").c_str(),list,ctx);assert(list->count==128&&list->truncated&&list->error==LOCK_WALLPAPER_OK);
    for(size_t i=1;i<list->count;++i)assert(strcmp(list->names[i-1],list->names[i])<0);
    list_directory((root+"/scan-limit").c_str(),list,ctx);assert(list->count==0&&list->truncated);
    Gate cancel{0,3};Context stop{proceed,&cancel};list_directory((root+"/listing").c_str(),list,stop);assert(!list->count&&list->error==LOCK_WALLPAPER_CANCELLED);delete list;
    std::vector<uint16_t> frame(1280*720,rgb565(80,120,160));
    auto frost=lock_wallpaper_frosted(frame.data());unsigned r,g,b;rgb888(frame[0],r,g,b);unsigned rgb[]={r,g,b};
    const unsigned luma=(r*77+g*150+b*29+128)/256,milk[]={244,242,238};
    for(unsigned c=0;c<3;++c)rgb[c]=(((rgb[c]*70+luma*30+50)/100)*38+milk[c]*62+50)/100;
    solid(frost,rgb[0],rgb[1],rgb[2],5);lock_wallpaper_image_free(&frost);
    // A sharp split must become a smooth transition, while distant flat areas
    // retain their expected tone. Uniform input and repeat runs are deterministic.
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x)frame[y*1280+x]=x<640?0:65535;
    frost=lock_wallpaper_frosted(frame.data());assert(frost.pixels);
    for(unsigned x=560;x<718;++x)assert(frost.pixels[360*1280+x]<=frost.pixels[360*1280+x+1]);
    assert(frost.pixels[360*1280+620]>frost.pixels[360*1280+400]);
    assert(frost.pixels[360*1280+660]<frost.pixels[360*1280+880]);
    auto again=lock_wallpaper_frosted(frame.data());assert(!memcmp(frost.pixels,again.pixels,OUTPUT_BYTES));
    lock_wallpaper_image_free(&frost);lock_wallpaper_image_free(&again);
    for(unsigned limit:{0U,50U,200U,1000U,2000U}){Gate no{0,limit};frost=lock_wallpaper_frosted(frame.data(),1280,proceed,&no);assert(!frost.pixels&&frost.error==LOCK_WALLPAPER_CANCELLED);}
    assert(lock_wallpaper_frosted(nullptr).error==LOCK_WALLPAPER_INVALID_FRAME);
    assert(lock_wallpaper_frosted(frame.data(),1279).error==LOCK_WALLPAPER_INVALID_FRAME);
    assert(!enough_memory(MAX_DECODE_BYTES+1));
    // The optional boot logo shares admission/error handling, but must not
    // inherit the PIN wallpaper's dark veil or flatten transparency to white.
    for(const auto &c:std::vector<std::pair<const char *,uint16_t>>{
        {"startup-valid",rgb565(220,32,16)},{"startup-png",rgb565(20,220,32)},
        {"startup-fallback",rgb565(20,220,32)}}){
        Context boot_ctx{nullptr,nullptr};auto art=load_startup_directory((root+"/"+c.first).c_str(),boot_ctx);
        assert(art.pixels&&art.error==LOCK_WALLPAPER_OK&&!boot_ctx.wallpaper&&opened.empty());
        unsigned r,g,b;rgb888(c.second,r,g,b);
        for(unsigned i=0;i<1280*720;++i){unsigned rr,gg,bb;rgb888(art.pixels[i],rr,gg,bb);
            assert(abs(int(rr)-int(r))<=10&&abs(int(gg)-int(g))<=10&&abs(int(bb)-int(b))<=10);}
        lock_wallpaper_image_free(&art);
    }
    for(const char *scenario:{"startup-missing","startup-corrupt"}){
        Context boot_ctx{nullptr,nullptr};auto art=load_startup_directory((root+"/"+scenario).c_str(),boot_ctx);
        assert(!art.pixels&&art.error!=LOCK_WALLPAPER_OK&&opened.empty());
    }
    auto *display=lv_display_create(1280,720);assert(display);
    auto *initial=lv_screen_active();player::StartupView startup;startup.create("1.3.16");
    lv_obj_delete(initial);auto *boot_screen=startup.screen();
    assert(lv_screen_active()==boot_screen&&!startup.has_artwork());
    assert(!strcmp(lv_label_get_text(lv_obj_get_child(boot_screen,0)),"AURORA PLAYER 1.3.16\nStarting interface..."));
    auto *player_screen=lv_obj_create(nullptr);auto *controls=lv_button_create(player_screen);
    assert(lv_screen_active()==boot_screen&&!lv_obj_is_visible(controls));
    startup.progress("Loading saved music library...");
    assert(lv_screen_active()==boot_screen);
    startup.stalled();auto *restart=lv_obj_get_child(boot_screen,-1);
    assert(!startup.restart_requested());lv_obj_send_event(restart,LV_EVENT_CLICKED,nullptr);assert(startup.restart_requested());
    startup.resumed();
    Context boot_ctx{nullptr,nullptr};auto art=load_startup_directory((root+"/startup-valid").c_str(),boot_ctx);
    startup.artwork(art,LV_FONT_DEFAULT);assert(startup.has_artwork());
    // The PIN panel can be prepared on the inactive destination first; there
    // is no intermediate publicly visible player screen during catalog load.
    auto *pin=lv_obj_create(player_screen);assert(!lv_obj_is_visible(pin));
    lv_screen_load(player_screen);lv_obj_update_layout(player_screen);startup.destroy();lock_wallpaper_image_free(&art);
    assert(lv_screen_active()==player_screen&&lv_obj_is_visible(pin));
    lv_display_delete(display);
    puts("PASS startup JPG/PNG colours, priority, missing/corrupt fallback, inactive player, progress, stall recovery and PIN-before-screen-swap");
    printf("PASS path/list bounds, crop/color/alpha, corrupt/oversized inputs, short reads/EINTR, cancellation and deterministic frosted blur.\n");
}
