/* Host visual / touch regression harness. Build with LVGL 9.3.0 and the same
 * player_ui.cpp used by the firmware. Native RGB565 assets must be linked.
 * Writes a real LVGL render, not a separate mock-up. */
#include "player_ui.h"
#include "player_artwork_decode.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" const uint8_t skin_pixels[] asm("_binary_ui_shell_rgb565_start");
static uint32_t pixels[1280*720];
static uint32_t draw_buffer[1280*80];
static lv_point_t pointer_point;
static bool pointer_pressed;
static std::vector<player_ui_action_t> actions;
static std::vector<int32_t> values;
static player_ui_state_t state;
static uint64_t flushed_pixels;
static uint32_t flush_calls;
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p) {
    flushed_pixels+=(uint64_t)lv_area_get_width(a)*lv_area_get_height(a);
    ++flush_calls;
    auto *src=(uint32_t*)p;
    for(int y=a->y1;y<=a->y2;++y)for(int x=a->x1;x<=a->x2;++x) pixels[y*1280+x]=*src++;
    lv_display_flush_ready(d);
}
static void read_pointer(lv_indev_t*,lv_indev_data_t *data) {data->point=pointer_point;data->state=pointer_pressed?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;}
static void action(player_ui_action_t a,int32_t v,void*) {
    actions.push_back(a);values.push_back(v);
    switch(a) {
        case PLAYER_UI_PLAY_PAUSE:state.playing=!state.playing;break;
        case PLAYER_UI_SHUFFLE:state.shuffle=v;break;
        case PLAYER_UI_REPEAT:state.repeat=v;break;
        case PLAYER_UI_MUTE:state.muted=v;break;
        case PLAYER_UI_VOLUME:state.volume=v;break;
        case PLAYER_UI_VISUAL_MODE:state.visual_mode=v;break;
        case PLAYER_UI_EFFECT:state.effect=v;break;
        case PLAYER_UI_LIBRARY_TAB:state.library_tab=v;break;
        case PLAYER_UI_FORMAT_FILTER:state.format_filter=v;break;
        case PLAYER_UI_FAVORITE:state.favorite=!state.favorite;break;
        case PLAYER_UI_SELECT_TRACK:state.track_index=v;break;
        default:break;
    }
    player_ui_set_state(&state);
}
static void step() {lv_tick_inc(25);lv_timer_handler();}
static void click(int x,int y,player_ui_action_t expected) {
    size_t count=actions.size();pointer_point={x,y};pointer_pressed=true;
    // Allow multiple indev samples: LVGL arc updates use PRESSING elapsed time.
    for(int n=0;n<4;++n) { step(); }
    pointer_pressed=false;step();step();
    printf("Touch %d at%d,%d: count%u->%u last%d\n",int(expected),x,y,unsigned(count),unsigned(actions.size()),actions.empty()?-1:int(actions.back()));
    assert(actions.size()>count&&actions.back()==expected);
}
static void save(const char *path) {
    FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n1280 720\n255\n");
    for(uint32_t p:pixels) {unsigned char rgb[3]={(unsigned char)(p>>16),(unsigned char)(p>>8),(unsigned char)p};fwrite(rgb,1,3,f);}fclose(f);
}
static lv_obj_t *find_label(lv_obj_t *parent,const char *text) {
    if(lv_obj_check_type(parent,&lv_label_class)&&strcmp(lv_label_get_text(parent),text)==0) { return parent; }
    for(uint32_t i=0;i<lv_obj_get_child_count(parent);++i) {
        if(auto *found=find_label(lv_obj_get_child(parent,i),text)) { return found; }
    }
    return nullptr;
}
static void assert_single_line_fits(lv_obj_t *obj,const char *text) {
    assert(obj&&strcmp(lv_label_get_text(obj),text)==0);
    lv_point_t size;
    lv_text_get_size(&size,text,lv_obj_get_style_text_font(obj,0),0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
    assert(size.x<=lv_obj_get_content_width(obj));
    assert(lv_obj_get_height(obj)<=lv_font_get_line_height(lv_obj_get_style_text_font(obj,0)));
}
static void template_pixels() {
    struct Region { const char *name; int x,y,w,h; };
    const Region regions[]={{"play",594,400,122,128},{"previous",509,425,76,79},{"next",722,425,78,79},
        {"shuffle",446,431,49,64},{"repeat",815,431,49,64},{"normal",466,583,67,70},
        {"bass",536,583,69,70},{"space",605,583,69,70},{"retro",675,583,70,70},
        {"bars",1063,590,57,61},{"wave",1120,590,62,61},{"dots",1182,590,63,61},
        {"playlist",902,90,105,34},{"local",1008,90,82,34},{"favorites",1091,90,112,34}};
    size_t checked=0;
    for(const auto &r:regions) { for(int y=r.y;y<r.y+r.h;++y) { for(int x=r.x;x<r.x+r.w;++x) {
        uint16_t p;memcpy(&p,skin_pixels+(y*1280+x)*2,2);
        uint32_t expected=(((((p>>11)&31)*2106)>>8)<<16)|(((((p>>5)&63)*1037)>>8)<<8)|(((p&31)*2106)>>8);
        if((pixels[y*1280+x]&0xffffff)!=expected) { printf("Template mismatch %s at%d,%d actual%06x expected%06x\n",r.name,x,y,unsigned(pixels[y*1280+x]&0xffffff),unsigned(expected));assert(false); }
        ++checked;
    } } }
    printf("PASS: %u original template pixels are exact across transport, FX, tabs and visualizer controls.\n",unsigned(checked));
}
static void check_label_heights(lv_obj_t *parent) {
    if(lv_obj_check_type(parent,&lv_label_class)) { assert(lv_obj_get_height(parent)==lv_font_get_line_height(lv_obj_get_style_text_font(parent,0))); }
    for(unsigned i=0;i<lv_obj_get_child_count(parent);++i) { check_label_heights(lv_obj_get_child(parent,i)); }
}
static void volume_regression(const char *preview) {
    const uint8_t original=state.volume;
    unsigned compared=0;
    auto compare_full=[&](const char *mode) {
        uint32_t partial[124*124];for(int y=0;y<124;++y) { for(int x=0;x<124;++x) { partial[y*124+x]=pixels[(559+y)*1280+60+x]; } }
        lv_obj_invalidate(player_ui_root());lv_refr_now(lv_obj_get_display(player_ui_root()));
        unsigned mismatch=0;for(int y=0;y<124;++y) { for(int x=0;x<124;++x) { mismatch+=partial[y*124+x]!=pixels[(559+y)*1280+60+x]; } }
        if(mismatch) { printf("Volume %s value%u: %u stale pixels versus full redraw.\n",mode,unsigned(state.volume),mismatch);if(preview) { save(preview); } }
        assert(mismatch==0);++compared;
    };
    for(int value:{0,1,2,3,10,25,50,68,69,70,99,100,99,98,75,50,25,0}) { state.volume=value;player_ui_set_state(&state);step();step();compare_full("state"); }
    pointer_pressed=true;
    for(int value:{0,5,10,20,30,40,50,60,70,80,90,95,100,90,70,50,30,10,0}) {
        float angle=(135+value*2.7f)*.0174532925f;pointer_point={122+int(cosf(angle)*51),621+int(sinf(angle)*51)};step();step();compare_full("drag");
    }
    pointer_pressed=false;step();step();
    state.volume=0;player_ui_set_state(&state);step();step();
    if(preview) { save(preview); }
    assert((pixels[612*1280+300]&0xffffff)==0x262b25); // No baked green fill remains at volume zero.
    const uint64_t unchanged=flushed_pixels;for(int i=0;i<8;++i) { player_ui_set_state(&state);step();step(); }assert(flushed_pixels==unchanged);
    state.volume=original;player_ui_set_state(&state);step();step();
    printf("PASS: %u knob frames during state updates and touch drag equal forced full redraw; unchanged volume draws 0pixels.\n",compared);
}
static lv_obj_t *find_widget(lv_obj_t *parent,const lv_obj_class_t *type) {
    if(lv_obj_check_type(parent,type)) { return parent; }
    for(unsigned i=0;i<lv_obj_get_child_count(parent);++i) { if(auto *found=find_widget(lv_obj_get_child(parent,i),type)) { return found; } }
    return nullptr;
}
static void seek_regression() {
    const uint32_t original=state.position_ms;unsigned compared=0;
    auto *seek=find_widget(player_ui_root(),&lv_slider_class);assert(seek);
    auto compare_full=[&]() {
        uint32_t partial[330*34];for(int y=0;y<34;++y) { for(int x=0;x<330;++x) { partial[y*330+x]=pixels[(358+y)*1280+493+x]; } }
        lv_obj_invalidate(player_ui_root());lv_refr_now(lv_obj_get_display(player_ui_root()));
        unsigned mismatch=0;for(int y=0;y<34;++y) { for(int x=0;x<330;++x) { mismatch+=partial[y*330+x]!=pixels[(358+y)*1280+493+x]; } }
        if(mismatch) { printf("Seek value%ld: %u stale pixels versus full redraw.\n",long(lv_slider_get_value(seek)),mismatch); }
        assert(mismatch==0);++compared;
    };
    for(int value:{0,1,2,10,99,100,250,500,501,900,999,1000,999,500,0,1000}) {
        state.position_ms=uint64_t(state.duration_ms)*value/1000;player_ui_set_state(&state);step();step();compare_full();
    }
    // The custom 19px circle extends farther than LVGL's native 12px knob.
    printf("Seek endpoint outer pixel: %06x.\n",unsigned(pixels[373*1280+819]&0xffffff));
    assert((pixels[373*1280+819]&0xffffff)==0xf0f1ef);
    pointer_pressed=true;
    for(int value:{0,1,10,30,50,70,90,99,100,90,50,10,0}) {
        pointer_point={504+307*value/100,374};step();step();
        const int drag_value=lv_slider_get_value(seek);state.position_ms=original;player_ui_set_state(&state);step();
        assert(lv_slider_get_value(seek)==drag_value);compare_full();
    }
    pointer_pressed=false;step();step();state.position_ms=original;player_ui_set_state(&state);step();step();
    const auto unchanged=flushed_pixels;for(int i=0;i<8;++i) { player_ui_set_state(&state);step();step(); }assert(flushed_pixels==unchanged);
    printf("PASS: %u seek frames equal forced full redraw, complete endpoint knob, playback updates preserve dragging, unchanged state 0pixels.\n",compared);
}
static bool cancel_art(void *p,bool) { return ++*static_cast<unsigned *>(p)<3; }
static void artwork_tests(const char *directory,const char *preview) {
    struct Case {const char *folder,*file;int expected;};
    const Case cases[]={{"apic-jpeg","track.mp3",1},{"apic-png","track.mp3",2},
        {"v22-pic","track.mp3",2},{"v24-unsync","track.mp3",1},{"folder","track.mp3",2},
        {"progressive-fallback","track.mp3",2},{"wav-id3","track.wav",1},
        {"thin-jpeg","track.mp3",1},{"thin-png","track.mp3",2},{"flac-front","track.flac",1},{"flac-invalid","track.flac",0},{"flac-folder","track.flac",2},{"missing","track.mp3",0},{"corrupt","track.mp3",0},{"png-bomb","track.mp3",0},{"too-large","track.mp3",0}};
    for(const auto &c:cases) {
        const std::string path=std::string(directory)+"/"+c.folder+"/"+c.file;
        auto *image=player_artwork_load(path.c_str(),nullptr,nullptr);
        printf("Artwork fixture %s: %s\n",c.folder,image?"decoded":"no cover");
        if(!c.expected) { assert(!image);continue; }
        assert(image&&image->image.header.w==312&&image->image.header.h==312&&image->image.data_size==194688);
        const auto *data=reinterpret_cast<const uint16_t *>(image->image.data);
        const uint16_t center=data[156*312+156];
        printf("  center RGB565: %04x\n",center);
        for(size_t sample:{size_t(0),size_t(311),size_t(311*312),size_t(312*312-1),size_t(156*312+156)}) {
            uint16_t pixel=data[sample];
            if(c.expected==1) { assert((pixel>>11)>=27&&((pixel>>5)&63)<=11); }
            else { assert(((pixel>>5)&63)>=57&&(pixel>>11)<=4); }
        }
        if(strcmp(c.folder,"apic-jpeg")==0) {
            std::vector<uint32_t> before(pixels,pixels+1280*720);
            player_ui_set_artwork(&image->image);step();step();
            for(int y=75;y<489;++y)for(int x=28;x<422;++x) {
                if(x<52||x>=401||y<93||y>=476) { assert(pixels[y*1280+x]==before[y*1280+x]); }
            }
            const uint64_t no_op=flushed_pixels;
            player_ui_set_artwork(&image->image);step();step();assert(flushed_pixels==no_op);
            if(preview) { save(preview); }
            player_ui_set_artwork(nullptr);step();step();
            for(int y=75;y<489;++y)for(int x=28;x<422;++x) { assert(pixels[y*1280+x]==before[y*1280+x]); }
        }
        player_artwork_image_free(image);
    }
    unsigned calls=0;const std::string path=std::string(directory)+"/apic-jpeg/track.mp3";
    auto *cancelled=player_artwork_load(path.c_str(),cancel_art,&calls);assert(!cancelled);
    puts("Artwork bounds, ID3v2.2/2.3/2.4, JPEG/PNG, WAV ID3, folder fallback, cancellation and preserved frame passed.");
}


static void thumbnail_tests(const player_ui_track_t *original,size_t count) {
    std::string keys[9];for(int i=0;i<9;++i) { keys[i]="/Music/cached"+std::to_string(i)+".flac"; }
    uint16_t colors[]={0xf800,0x07e0,0x001f,0xffe0,0xf81f,0x07ff,0xffff,0x8410,0x4208};
    uint16_t source[56*56];lv_image_dsc_t image{};image.header.magic=LV_IMAGE_HEADER_MAGIC;image.header.cf=LV_COLOR_FORMAT_RGB565;
    image.header.w=image.header.h=56;image.header.stride=112;image.data=reinterpret_cast<uint8_t *>(source);image.data_size=sizeof source;
    const char *old_key=state.artwork_key;const auto old_count=state.track_count,old_index=state.track_index;
    auto expected=[](uint16_t v) { unsigned r=(v>>11)&31,g=(v>>5)&63,b=v&31;return ((r*255/31)<<16)|((g*255/63)<<8)|(b*255/31); };
    for(int i=0;i<8;++i) {
        for(auto &pixel:source) { pixel=colors[i]; }state.artwork_key=keys[i].c_str();player_ui_set_state(&state);player_ui_set_artwork(&image);player_ui_set_artwork(nullptr);
    }
    for(auto &pixel:source) { pixel=0; } // Cached pixels must be independently owned.
    state.track_count=1;state.track_index=0;
    for(int i=0;i<8;++i) {
        player_ui_track_t row={"Cached cover","Artist",60000,false,keys[i].c_str()};player_ui_set_playlist(&row,1,0);player_ui_set_state(&state);step();step();
        printf("Thumbnail %d got%06x expected%06x\n",i,unsigned(pixels[168*1280+942]&0xffffff),expected(colors[i]));assert((pixels[168*1280+942]&0xffffff)==expected(colors[i]));
    }
    for(auto &pixel:source) { pixel=colors[8]; }state.artwork_key=keys[8].c_str();player_ui_set_state(&state);player_ui_set_artwork(&image);player_ui_set_artwork(nullptr);
    player_ui_track_t evicted={"Evicted cover","Artist",60000,false,keys[0].c_str()};player_ui_set_playlist(&evicted,1,0);step();step();
    assert((pixels[168*1280+942]&0xffffff)!=expected(colors[0]));
    state.artwork_key=old_key;state.track_count=old_count;state.track_index=old_index;player_ui_set_playlist(original,count,old_index);player_ui_set_state(&state);step();step();
    puts("PASS: eight copied thumbnails survive filter/reorder; bounded eviction restores fallback.");
}
static unsigned objects(lv_obj_t *o) {
    unsigned count=1;for(unsigned i=0;i<lv_obj_get_child_count(o);++i) { count+=objects(lv_obj_get_child(o,i)); }return count;
}
int main(int argc,char **argv) {
    setvbuf(stdout,nullptr,_IONBF,0);lv_init();auto *d=lv_display_create(1280,720);lv_display_set_color_format(d,LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(d,draw_buffer,nullptr,sizeof(draw_buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(d,flush);
    auto *indev=lv_indev_create();lv_indev_set_type(indev,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(indev,read_pointer);
    player_ui_create(lv_screen_active(),action,nullptr);
    const char *names[]={"Aurora Protocol","Midnight Circuit","Glass Ocean","Binary Bloom","Solar Drift","Echo Chamber","Neon Shore","Terminal Dreams","Slow Orbit","Night Transit","Signal Echo","Dawn Circuit"};
    const char *artists[]={"Hikari Collective","Lo-Fi Assembly","Nexis","Studio Kumo","Yugen","Vector Fields"};
    const uint32_t durations[]={276000,201000,314000,232000,258000,187000};
    std::string paths[12];player_ui_track_t tracks[12]{};
    for(int i=0;i<12;++i) { paths[i]="/Music/track"+std::to_string(i)+".mp3";tracks[i]={names[i],artists[i%6],durations[i%6],false,paths[i].c_str()}; }
    player_ui_set_playlist(tracks,12,0);
    state.title=names[0];state.artist=artists[0];state.album="Neon Tides";state.file_name="01_Aurora_Protocol.mp3";state.format="MP3";state.modified="2026.10.09 18:36";
    state.clock_text="14:28";state.date_text="2026.10.09";state.firmware="v1.2.2";state.position_ms=137000;state.duration_ms=276000;state.sample_rate=44100;state.bitrate_kbps=320;
    state.track_count=12;state.volume=68;state.battery_percent=78;state.playing=true;state.battery_estimated=true;state.bluetooth_available=true;state.bluetooth_enabled=true;
    state.artwork_key=paths[0].c_str();player_ui_set_state(&state);
    float bands[40];int16_t wave[128];for(int i=0;i<40;++i) { bands[i]=.16f+.77f*fabsf(sinf(i*.37f))*fabsf(cosf(i*.13f)); }for(int i=0;i<128;++i) { wave[i]=int16_t(20000*sinf(i*.2f)*cosf(i*.11f)); }
    player_ui_set_spectrum(bands,40,wave,128);for(int i=0;i<5;++i) { step(); }
    save(argc>1?argv[1]:"ui-preview.ppm");
    template_pixels();
    const std::string base_preview=argc>1?argv[1]:"ui-preview.ppm";
    const auto preview_separator=base_preview.find_last_of("/\\");
    const auto volume_preview=(preview_separator==std::string::npos?"":base_preview.substr(0,preview_separator+1))+"preview-volume-zero.ppm";
    volume_regression(volume_preview.c_str());
    seek_regression();
    if(argc>3) { artwork_tests(argv[3],argc>4?argv[4]:nullptr); }
    thumbnail_tests(tracks,12);
    const uint64_t start=flushed_pixels;for(int i=0;i<8;++i) { player_ui_set_state(&state);step();step(); }assert(flushed_pixels==start);
    printf("Repeated state: %llu pixels.\n",static_cast<unsigned long long>(flushed_pixels-start));
    state.battery_percent=-1;player_ui_set_state(&state);step();step();assert_single_line_fits(find_label(player_ui_root(),LV_SYMBOL_BATTERY_EMPTY " --%"),LV_SYMBOL_BATTERY_EMPTY " --%");
    state.battery_percent=100;state.battery_charging=true;player_ui_set_state(&state);step();step();assert_single_line_fits(find_label(player_ui_root(),LV_SYMBOL_CHARGE " ~100%"),LV_SYMBOL_CHARGE " ~100%");
    state.battery_percent=78;state.battery_charging=false;player_ui_set_state(&state);step();step();

    // Use only the production geometry. The former test set title height itself
    // and concealed wrapping caused by the real label helper's CONTENT height.
    auto *title=find_label(player_ui_root(),names[0]);auto *artist=find_label(player_ui_root(),artists[0]);auto *album=find_label(player_ui_root(),"Neon Tides");
    assert(title&&artist&&album);
    const char *long_title="01 - Zażółć gęślą jaźń - A very long real filename and live concert recording remastered deluxe edition 2026.flac";
    const char *long_artist="Hikari Collective featuring a very long list of contributing artists and orchestral performers";
    const char *long_album="First album line with many words that cannot fit the compact panel\nSecond album line must not overwrite the visualizer";
    player_ui_track_t long_tracks[12];for(int i=0;i<12;++i) { long_tracks[i]={long_title,long_artist,tracks[i].duration_ms,i==1,paths[i].c_str()}; }
    state.title=long_title;state.artist=long_artist;state.album=long_album;state.format="WAV";state.sample_rate=96000;state.bit_depth=32;state.bitrate_kbps=6144;
    player_ui_set_playlist(long_tracks,12,0);player_ui_set_state(&state);
    player_ui_set_notice("A long status message should remain inside its own bar even when a file or folder has a very long name and contains extra details.");step();step();
    const std::string initial_preview=argc>1?argv[1]:"ui-preview.ppm";
    const size_t separator=initial_preview.find_last_of("/\\");
    const std::string long_preview=(separator==std::string::npos?"":initial_preview.substr(0,separator+1))+"preview-long.ppm";
    save(long_preview.c_str());check_label_heights(player_ui_root());
    printf("Production long metadata heights: title=%ld/%u artist=%ld/%u album=%ld/%u.\n",long(lv_obj_get_height(title)),lv_font_get_line_height(lv_obj_get_style_text_font(title,0)),long(lv_obj_get_height(artist)),lv_font_get_line_height(lv_obj_get_style_text_font(artist,0)),long(lv_obj_get_height(album)),lv_font_get_line_height(lv_obj_get_style_text_font(album,0)));
    assert(lv_obj_get_height(title)==lv_font_get_line_height(lv_obj_get_style_text_font(title,0)));
    assert(lv_obj_get_height(artist)==lv_font_get_line_height(lv_obj_get_style_text_font(artist,0)));
    assert(lv_obj_get_height(album)==lv_font_get_line_height(lv_obj_get_style_text_font(album,0)));
    assert(lv_obj_get_y(title)+lv_obj_get_height(title)<=lv_obj_get_y(artist));
    assert(lv_obj_get_y(artist)+lv_obj_get_height(artist)<=lv_obj_get_y(album));
    assert(strcmp(lv_label_get_text(title),long_title)!=0);
    assert(strcmp(lv_label_get_text(artist),long_artist)!=0);
    const uint64_t long_start=flushed_pixels;for(int i=0;i<8;++i) { player_ui_set_state(&state);step();step(); }assert(flushed_pixels==long_start);
    state.title="A filename with an explicit\nline break and more long text for the same one-line display";player_ui_set_state(&state);step();step();
    assert(lv_obj_get_height(title)==lv_font_get_line_height(lv_obj_get_style_text_font(title,0)));
    state.title=names[0];state.artist=artists[0];state.album="Neon Tides";state.format="MP3";state.sample_rate=44100;state.bit_depth=0;state.bitrate_kbps=320;
    player_ui_set_playlist(tracks,12,0);player_ui_set_state(&state);player_ui_set_notice("");step();step();
    puts("PASS: long UTF-8 filenames, artists, multiline tags and notices use production single-line bounds without overlap; repeated updates draw 0pixels.");
    click(653,462,PLAYER_UI_PLAY_PAUSE);
    const std::string paused_preview=(separator==std::string::npos?"":initial_preview.substr(0,separator+1))+"preview-paused.ppm";
    save(paused_preview.c_str());
    for(int y=400;y<528;++y) { for(int x=594;x<716;++x) { if(x>=634&&x<=681&&y>=433&&y<=494) { continue; }uint16_t p;memcpy(&p,skin_pixels+(y*1280+x)*2,2);uint32_t expected=(((((p>>11)&31)*2106)>>8)<<16)|(((((p>>5)&63)*1037)>>8)<<8)|(((p&31)*2106)>>8);assert((pixels[y*1280+x]&0xffffff)==expected); } }
    puts("PASS: paused state changes only the center icon; original circular texture and rim remain exact.");
    click(546,463,PLAYER_UI_PREVIOUS);click(764,463,PLAYER_UI_NEXT);click(470,462,PLAYER_UI_SHUFFLE);click(843,462,PLAYER_UI_REPEAT);
    click(788,182,PLAYER_UI_FAVORITE);assert(values.back()==-1);click(841,182,PLAYER_UI_FILE_INFO);assert(values.back()==-1);
    click(992,38,PLAYER_UI_SETTINGS);click(1126,38,PLAYER_UI_BLUETOOTH);
    for(int x:{952,1050,1147}) { click(x,107,PLAYER_UI_LIBRARY_TAB); }assert(values.back()==2);
    for(int i=0;i<4;++i) { click(502+i*70,616,PLAYER_UI_EFFECT);assert(values.back()==i); }
    for(int i=0;i<4;++i) { click(821+i*55,618,PLAYER_UI_FORMAT_FILTER);assert(values.back()==i+1); }
    click(986,618,PLAYER_UI_FORMAT_FILTER);assert(values.back()==0);
    for(int i=0;i<3;++i) { click(1097+i*62,620,PLAYER_UI_VISUAL_MODE);assert(values.back()==i); }
    click(227,620,PLAYER_UI_MUTE);click(350,619,PLAYER_UI_VOLUME);click(161,645,PLAYER_UI_VOLUME);click(656,374,PLAYER_UI_SEEK_MS);
    click(1080,154,PLAYER_UI_SELECT_TRACK);assert(values.back()==0);
    // Row overflow menu opens without selecting a track; favorite refers to its row.
    size_t count=actions.size();pointer_point={1236,178};pointer_pressed=true;step();step();pointer_pressed=false;step();step();assert(actions.size()==count);
    click(1090,163,PLAYER_UI_FAVORITE);assert(values.back()==0);
    // The playlist overflow keeps folder/search/sort accessible without old widgets.
    for(int item=0;item<3;++item) {
        pointer_point={1231,107};pointer_pressed=true;step();step();pointer_pressed=false;step();step();
        click(1090,212+item*49,item==0?PLAYER_UI_FOLDER:item==1?PLAYER_UI_SEARCH:PLAYER_UI_SORT);
    }
    state.visual_mode=1;state.effect=0;state.library_tab=0;state.format_filter=0;state.playing=true;state.muted=false;player_ui_set_state(&state);player_ui_set_spectrum(bands,40,wave,128);step();step();
    if(argc>2) { save(argv[2]); }
    const uint64_t wave_start=flushed_pixels;for(int i=0;i<8;++i) { player_ui_set_spectrum(bands,40,wave,128);step();step(); }assert(flushed_pixels==wave_start);
    state.visual_mode=0;player_ui_set_state(&state);for(int i=0;i<160;++i) { player_ui_set_spectrum(nullptr,0,nullptr,0);step();step(); }
    const uint64_t silent_start=flushed_pixels;for(int i=0;i<8;++i) { player_ui_set_spectrum(nullptr,0,nullptr,0);step();step(); }assert(flushed_pixels==silent_start);
    const unsigned small_objects=objects(player_ui_root());
    std::vector<std::string> titles;titles.reserve(10000);std::vector<player_ui_track_t> large;large.reserve(10000);
    for(int i=0;i<10000;++i) { titles.push_back("Track "+std::to_string(i));large.push_back({titles.back().c_str(),"Artist",180000,false,nullptr}); }
    player_ui_set_playlist(large.data(),5000,4999);state.track_index=4999;state.track_count=5000;player_ui_set_state(&state);step();step();
    assert_single_line_fits(find_label(player_ui_root(),"5000 / 5000"),"5000 / 5000");assert_single_line_fits(find_label(player_ui_root(),"5000"),"5000");
    const uint64_t playlist_start=flushed_pixels;player_ui_set_playlist(large.data(),5000,4999);step();step();assert(flushed_pixels==playlist_start);
    player_ui_set_playlist(large.data(),10000,9999);state.track_index=9999;state.track_count=10000;player_ui_set_state(&state);step();step();
    assert_single_line_fits(find_label(player_ui_root(),"10000 / 10000"),"10000 / 10000");assert_single_line_fits(find_label(player_ui_root(),"10000"),"10000");assert(objects(player_ui_root())==small_objects);
    const uint64_t row_start=flushed_pixels;player_ui_track_t changed={"Updated title","New artist",222000,true,nullptr};player_ui_update_track(9999,&changed);step();step();
    assert(flushed_pixels>row_start&&flushed_pixels-row_start<=347U*68U);assert(find_label(player_ui_root(),"Updated title"));assert(find_label(player_ui_root(),"New artist"));assert(find_label(player_ui_root(),"03:42"));
    const uint64_t row_noop=flushed_pixels;player_ui_update_track(9999,&changed);player_ui_update_track(0,&changed);step();step();assert(flushed_pixels==row_noop);
    changed.duration_ms=0;player_ui_update_track(9999,&changed);step();step();assert(find_label(player_ui_root(),"--:--"));
    state.sample_rate=0;state.duration_ms=0;state.bitrate_kbps=0;player_ui_set_state(&state);step();step();assert(find_label(player_ui_root(),"LOAD ON PLAY"));
    const uint64_t last=flushed_pixels;player_ui_set_state(&state);step();step();assert(flushed_pixels==last);
    player_ui_render_stats_t stats{};player_ui_get_render_stats(&stats);assert(stats.frames>0&&stats.skipped_spectrum_updates>=16);
    printf("PASS: silver native UI; %u touch actions; equal object count %u at12/10000tracks; single-row updates; repeated state/silence/waveform 0pixels.\n",unsigned(actions.size()),small_objects);
    return 0;
}
