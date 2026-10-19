// Real production LVGL UI: render and dispatch touch events through its widgets.
#include "player_ui.h"
#include "search_keyboard.h"
#include "src/widgets/buttonmatrix/lv_buttonmatrix_private.h"
#include "aurora_geometry.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
static uint32_t pixels[1280*720],buffer[1280*80];
static unsigned visual_source_calls;
static bool target_rgb565;
static bool visual_source(float *bands,int16_t *wave,void*){++visual_source_calls;for(int i=0;i<32;i++)bands[i]=((i+visual_source_calls)%10)/10.f;for(int i=0;i<128;i++)wave[i]=int16_t(15000*sinf(i*.18f+visual_source_calls*.2f));return true;}
static lv_point_t pointer;static bool pressed;static player_ui_state_t state{};
static std::vector<int> actions,values;static uint64_t flushed;
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p){
 auto *rgb=(uint32_t*)p;auto *native=(uint16_t*)p;
 for(int y=a->y1;y<=a->y2;y++)for(int x=a->x1;x<=a->x2;x++){
  uint32_t value;if(target_rgb565){uint16_t v=*native++;value=0xff000000|((uint32_t(v>>11)*255/31)<<16)|((uint32_t((v>>5)&63)*255/63)<<8)|(uint32_t(v&31)*255/31);}else value=*rgb++;
  pixels[y*1280+x]=value;flushed++;
 }lv_display_flush_ready(d);
}
static void read_pointer(lv_indev_t*,lv_indev_data_t *d){d->point=pointer;d->state=pressed?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;}
static void apply(player_ui_action_t a,int32_t v,void*){
 actions.push_back(a);values.push_back(v);
 switch(a){case PLAYER_UI_PLAY_PAUSE:state.playing=!state.playing;break;case PLAYER_UI_VOLUME:state.volume=v;break;case PLAYER_UI_MUTE:state.muted=!state.muted;break;case PLAYER_UI_SEEK_MS:state.position_ms=v;break;
 case PLAYER_UI_SHUFFLE:state.shuffle=v;break;case PLAYER_UI_REPEAT:state.repeat=v;break;case PLAYER_UI_FAVORITE:state.favorite=!state.favorite;break;case PLAYER_UI_LIBRARY_TAB:state.library_tab=v;break;case PLAYER_UI_SELECT_TRACK:state.track_index=v;break;
 case PLAYER_UI_TRACK_SPEED:state.speed_percent=v;break;case PLAYER_UI_EFFECT:state.effect=v;break;case PLAYER_UI_FORMAT_FILTER:state.format_filter=v;break;case PLAYER_UI_VISUAL_MODE:state.visual_mode=v;break;
 case PLAYER_UI_DRAGON_TYPE:state.dragon_type=v;break;case PLAYER_UI_DRAGON_SPEED:state.dragon_speed=v;break;case PLAYER_UI_DRAGON_COLORS:state.dragon_colors=!state.dragon_colors;break;case PLAYER_UI_BRIGHTNESS:state.brightness=v;break;case PLAYER_UI_ANIMATIONS:state.animations=!state.animations;break;case PLAYER_UI_CLOCK_FORMAT:state.clock24=!state.clock24;break;default:break;}
 player_ui_set_state(&state);
}
static void step(int n=3){for(int i=0;i<n;i++){lv_tick_inc(25);lv_timer_handler();}}
static void tap(int x,int y,int expected=-1){size_t before=actions.size();pointer={x,y};pressed=true;step(4);pressed=false;step(3);if(expected>=0){printf("Touch %d at %d,%d: %u -> %u, last %d\n",expected,x,y,unsigned(before),unsigned(actions.size()),actions.empty()?-1:actions.back());assert(actions.size()>before);assert(actions.back()==expected);}}
static void control(int id,int expected){const auto&r=aurora_controls[id];tap(r.x+r.w/2,r.y+r.h/2,expected);}
static void save(const std::string&path){FILE*f=fopen(path.c_str(),"wb");assert(f);fprintf(f,"P6\n1280 720\n255\n");for(auto p:pixels){unsigned char c[]={uint8_t(p>>16),uint8_t(p>>8),uint8_t(p)};fwrite(c,1,3,f);}fclose(f);}
// Finish pending partial rendering at the current tick before comparing it to
// a full redraw. Scroll inertia can update layout after the 30ms refresh timer
// while the host test advances in 25ms steps; both images must use one state.
static void no_trails(){lv_refr_now(lv_display_get_default());std::vector<uint32_t> old(pixels,pixels+1280*720);lv_obj_invalidate(player_ui_root());lv_refr_now(lv_display_get_default());for(size_t i=0;i<old.size();i++)if(old[i]!=pixels[i]){printf("Stale pixel %u,%u: %08x -> %08x\n",unsigned(i%1280),unsigned(i/1280),old[i],pixels[i]);assert(false);}}
static unsigned objects(lv_obj_t*o){unsigned n=1;for(unsigned i=0;i<lv_obj_get_child_count(o);i++)n+=objects(lv_obj_get_child(o,i));return n;}
static lv_obj_t *find_text(lv_obj_t*o,const char*t,bool visible=true){if(lv_obj_check_type(o,&lv_label_class)&&(!visible||lv_obj_is_visible(o))&&!strcmp(lv_label_get_text(o),t))return o;for(unsigned i=0;i<lv_obj_get_child_count(o);i++)if(auto *found=find_text(lv_obj_get_child(o,i),t,visible))return found;return nullptr;}
static void tap_text(const char*t,int expected=-1){auto *o=find_text(player_ui_root(),t);assert(o);lv_area_t a;lv_obj_get_coords(o,&a);tap((a.x1+a.x2)/2,(a.y1+a.y2)/2,expected);}
static void clock_bounds(lv_obj_t*o){if(lv_obj_check_type(o,&lv_label_class)){const char*t=lv_label_get_text(o);if(lv_obj_is_visible(o)&&(!strcmp(t,"14:28")||!strcmp(t,"OFF"))){lv_area_t a;lv_obj_get_coords(o,&a);printf("Clock field %s %d,%d to %d,%d\n",t,a.x1,a.y1,a.x2,a.y2);assert(a.x1>=900&&a.x2<1135&&a.y1>=25&&a.y2<=70);}}for(unsigned i=0;i<lv_obj_get_child_count(o);i++)clock_bounds(lv_obj_get_child(o,i));}
static void cover_bounds(){
 std::vector<uint32_t> before(pixels,pixels+1280*720);std::vector<uint16_t> green(312*312,0x07e0);
 lv_image_dsc_t image{};image.header.magic=LV_IMAGE_HEADER_MAGIC;image.header.cf=LV_COLOR_FORMAT_RGB565;image.header.w=image.header.h=312;image.header.stride=624;image.data=(const uint8_t*)green.data();image.data_size=green.size()*2;
 player_ui_set_artwork(&image);step();for(int y=0;y<720;y++)for(int x=0;x<1280;x++)if(x<36||x>=343||y<101||y>=408)assert(pixels[y*1280+x]==before[y*1280+x]);
 player_ui_set_artwork(nullptr);step();assert(std::equal(before.begin(),before.end(),pixels));puts("PASS: decoded cover remains in the square; fallback restores every pixel.");
}
static void filter_checks(const std::string &dir){
 const auto &all=aurora_controls[16],&mp3=aurora_controls[17],&wav=aurora_controls[18],&flac=aurora_controls[22],&aac=aurora_controls[23];
 assert(all.w==89&&mp3.w==90&&wav.w==89&&all.h==58&&mp3.h==58&&wav.h==58);
 assert(flac.w==all.w&&aac.w==all.w&&flac.h==all.h&&aac.h==all.h);
 assert(all.y==mp3.y&&mp3.y==wav.y&&flac.y==aac.y&&all.y+all.h<flac.y);
 assert(flac.y+flac.h<=685&&aac.y+aac.h<=685);
 assert(all.x+(wav.x+wav.w)==flac.x+(aac.x+aac.w));
 for(int filter=0;filter<5;filter++){
  int id=filter<3?16+filter:19+filter;control(id,PLAYER_UI_FORMAT_FILTER);assert(state.format_filter==filter);step();no_trails();
  const auto &r=aurora_controls[id];assert(((pixels[(r.y+13)*1280+r.x+13]>>16)&255)>120);
  if(filter>=3)save(dir+(filter==3?"/aurora-filter-flac.ppm":"/aurora-filter-aac.ppm"));
 }
 control(23,PLAYER_UI_FORMAT_FILTER);assert(state.format_filter==0);step();
 puts("PASS: five physical filter targets, active states, toggle-to-ALL, original sizes, symmetric rows and footer clearance.");
}
static void thumbnail_checks(const char *const *paths,const std::string &dir){
 size_t missing[8]{};assert(player_ui_get_missing_thumbnails(missing,8)==5);for(size_t i=0;i<5;i++)assert(missing[i]==i);
 std::vector<uint32_t> before(pixels,pixels+1280*720);std::vector<uint16_t> red(56*56,0xf800);
 lv_image_dsc_t thumb{};thumb.header.magic=LV_IMAGE_HEADER_MAGIC;thumb.header.cf=LV_COLOR_FORMAT_RGB565;thumb.header.w=thumb.header.h=56;thumb.header.stride=112;thumb.data=(const uint8_t*)red.data();thumb.data_size=red.size()*2;
 player_ui_set_thumbnail(paths[1],&thumb);std::fill(red.begin(),red.end(),0x001f);step();
 assert((pixels[276*1280+900]&0xffffff)==0xff0000);
 for(int y=0;y<720;y++)for(int x=0;x<1280;x++)if(x<890||x>=946||y<266||y>=322)assert(pixels[y*1280+x]==before[y*1280+x]);
 assert(player_ui_get_missing_thumbnails(missing,8)==4);for(size_t i=0;i<4;i++)assert(missing[i]!=1);
 player_ui_set_thumbnail(paths[0],nullptr);assert(player_ui_get_missing_thumbnails(missing,8)==3);
 lv_image_dsc_t invalid=thumb;invalid.data_size=1;player_ui_set_thumbnail(paths[2],&invalid);assert(player_ui_get_missing_thumbnails(missing,8)==3);
 assert(player_ui_get_missing_thumbnails(nullptr,8)==0);assert(player_ui_get_missing_thumbnails(missing,0)==0);assert(player_ui_get_missing_thumbnails(missing,1)==1);
 player_ui_set_overlay(true);assert(player_ui_get_missing_thumbnails(missing,8)==0);player_ui_set_overlay(false);
 state.covers=false;player_ui_set_state(&state);step();assert(player_ui_get_missing_thumbnails(missing,8)==0);assert((pixels[276*1280+900]&0xffffff)!=0xff0000);
 state.covers=true;player_ui_set_state(&state);step();assert((pixels[276*1280+900]&0xffffff)==0xff0000);
 player_ui_open_settings();assert(player_ui_get_missing_thumbnails(missing,8)==0);control(8,PLAYER_UI_LIBRARY_TAB);
 player_ui_set_sleeping(true);assert(player_ui_get_missing_thumbnails(missing,8)==0);player_ui_set_sleeping(false);step();
 lv_tick_inc(60001);assert(player_ui_get_missing_thumbnails(missing,8)==4);
 // Late results outside this viewport cannot evict an on-screen cover.
 for(int i=0;i<5;i++)player_ui_set_thumbnail(paths[i],&thumb);
 for(int i=0;i<20;i++){std::string late="/sdcard/Elsewhere/"+std::to_string(i)+".mp3";player_ui_set_thumbnail(late.c_str(),&thumb);}
 assert(player_ui_get_missing_thumbnails(missing,8)==0);step();no_trails();save(dir+"/aurora-background-thumbnails.ppm");
 player_ui_clear_thumbnails();step();assert(player_ui_get_missing_thumbnails(missing,8)==5);no_trails();
 puts("PASS: non-playing thumbnails copy pixels; repaint only that image; covers toggle, hidden views, bounds, negative TTL, late-result protection and clear.");
}
static void artwork_ownership_checks(const char *const *paths){
 std::vector<uint16_t> red(56*56,0xf800),blue(56*56,0x001f);
 lv_image_dsc_t a{},b{};a.header.magic=LV_IMAGE_HEADER_MAGIC;a.header.cf=LV_COLOR_FORMAT_RGB565;a.header.w=a.header.h=56;a.header.stride=112;a.data=(const uint8_t*)red.data();a.data_size=red.size()*2;b=a;b.data=(const uint8_t*)blue.data();
 char borrowed_path[128];snprintf(borrowed_path,sizeof borrowed_path,"%s",paths[0]);
 state.artwork_key=borrowed_path;state.covers=true;player_ui_set_state(&state);
 // UI owns the path key after set_state; caller may reuse/free its text buffer.
 snprintf(borrowed_path,sizeof borrowed_path,"%s",paths[1]);state.artwork_key=paths[0];
 assert(player_ui_accepts_artwork(paths[0])&&!player_ui_accepts_artwork(paths[1]));
 assert(player_ui_accepts_artwork(nullptr)&&player_ui_accepts_artwork(""));
 player_ui_set_artwork(&a);step();assert((pixels[200*1280+150]&0xffffff)==0xff0000);
 // Cached B completing before its UI state cannot change the A cover/key.
 if(player_ui_accepts_artwork(paths[1]))player_ui_set_artwork(&b);
 step();assert((pixels[200*1280+150]&0xffffff)==0xff0000);
 state.artwork_key=paths[1];player_ui_set_state(&state);
 // Conversely, late A must not be attributed to B after a filter/selection.
 assert(!player_ui_accepts_artwork(paths[0])&&player_ui_accepts_artwork(paths[1]));
 if(player_ui_accepts_artwork(paths[0]))player_ui_set_artwork(&a);
 if(player_ui_accepts_artwork(paths[1]))player_ui_set_artwork(&b);
 step();assert((pixels[200*1280+150]&0xffffff)==0x0000ff);
 assert((pixels[204*1280+900]&0xffffff)==0xff0000);
 assert((pixels[276*1280+900]&0xffffff)==0x0000ff);
 state.covers=false;player_ui_set_state(&state);assert(!player_ui_accepts_artwork(paths[1])&&player_ui_accepts_artwork(nullptr));
 player_ui_set_artwork(nullptr);player_ui_clear_thumbnails();state.covers=true;state.artwork_key=nullptr;player_ui_set_state(&state);
 assert(!player_ui_accepts_artwork(paths[0])&&!player_ui_accepts_artwork(paths[1]));step();no_trails();
 puts("PASS: main-cover ownership in both result/UI orderings; A/B thumbnails keep their own pixels; covers=OFF rejects images but permits clear.");
}
static lv_obj_t *visible_library_list(){
 for(unsigned i=0;i<lv_obj_get_child_count(player_ui_root());i++){
  auto *o=lv_obj_get_child(player_ui_root(),i);
  if(lv_obj_is_visible(o)&&lv_obj_has_flag(o,LV_OBJ_FLAG_SCROLLABLE)&&lv_obj_get_width(o)==367&&lv_obj_get_height(o)==328)return o;
 }
 return nullptr;
}
static void embedded_folders_checks(const std::string &dir){
 state.animations=false;state.playing=false;state.library_tab=0;player_ui_set_state(&state);step();
 std::vector<uint32_t> shell_before(pixels,pixels+1280*720);
 const unsigned before_objects=objects(player_ui_root());
 std::vector<std::string> storage;storage.reserve(10000);
 for(int i=0;i<10000;i++)storage.emplace_back("Collection / Artist "+std::to_string(i));
 std::vector<const char*> names;for(auto &name:storage)names.push_back(name.c_str());
 auto *prepared=player_ui_prepare_folders(names.data(),names.size());assert(prepared);
 storage[0]="Caller reused its strings"; // Prepared owns its strings already.
 player_ui_apply_folders(prepared,0);player_ui_free_folders(prepared);
 control(9,PLAYER_UI_LIBRARY_TAB);step();auto *list=visible_library_list();assert(list);
 lv_area_t area;lv_obj_get_coords(list,&area);assert(area.x1==880&&area.y1==186&&area.x2==1246&&area.y2==513);
 assert(objects(player_ui_root())==before_objects&&objects(list)<40);
 assert(find_text(player_ui_root(),"10000 FOLDERS"));
 assert(find_text(list,"Collection / Artist 0",false));
 assert(!find_text(player_ui_root(),"SELECT FOLDER"));size_t missing[8];assert(!player_ui_get_missing_thumbnails(missing,8));
 for(int y=0;y<720;y++)for(int x=0;x<1280;x++)if(x<870||y<96||y>540)assert(pixels[y*1280+x]==shell_before[y*1280+x]);
 save(dir+"/aurora-files-before.ppm");no_trails();save(dir+"/aurora-files.ppm");
 // Press feedback overlays the immutable row snapshot and restores it on release.
 std::vector<uint32_t> unpressed(pixels,pixels+1280*720);pointer={1040,212};pressed=true;step(4);lv_refr_now(lv_display_get_default());
 assert(!std::equal(unpressed.begin(),unpressed.end(),pixels));pressed=false;step();lv_refr_now(lv_display_get_default());
 assert(actions.back()==PLAYER_UI_FOLDER_SELECT&&values.back()==0);assert(std::equal(unpressed.begin(),unpressed.end(),pixels));
 // Actual pointer drag must scroll the embedded list without choosing a folder.
 size_t before_actions=actions.size();pointer={1020,470};pressed=true;step();
 for(int n=0;n<12;n++){pointer.y-=15;step(1);}pressed=false;step(15);
 assert(lv_obj_get_scroll_y(list)>100&&actions.size()==before_actions);no_trails();
 // Direct scrollbar drag reaches distant entries, still without opening them.
 pointer={1242,300};pressed=true;step();pointer.y=480;step();pressed=false;step();
 assert(lv_obj_get_scroll_y(list)>300000&&actions.size()==before_actions);
 for(int position:{0,1234,54321,200000,539000}){
  lv_obj_scroll_to_y(list,position,LV_ANIM_OFF);step();int sy=lv_obj_get_scroll_y(list);
  tap(1040,area.y1+100,PLAYER_UI_FOLDER_SELECT);assert(values.back()==(sy+100)/54);
  assert(visible_library_list()==list);no_trails();
 }
 // Settings, Queue and Favorites never stack folder/track lists or lose scroll.
 const int remembered=lv_obj_get_scroll_y(list);
 player_ui_open_settings();step();assert(!visible_library_list());assert(!player_ui_get_missing_thumbnails(missing,8));
 player_ui_open_files();step();assert(visible_library_list()==list&&lv_obj_get_scroll_y(list)==remembered);
 control(8,PLAYER_UI_LIBRARY_TAB);assert(visible_library_list()!=list);
 control(10,PLAYER_UI_LIBRARY_TAB);assert(visible_library_list()!=list);
 control(9,PLAYER_UI_LIBRARY_TAB);assert(visible_library_list()==list&&lv_obj_get_scroll_y(list)==remembered);
 // Same selected folder preserves scroll even across a refreshed model.
 prepared=player_ui_prepare_folders(names.data(),names.size());player_ui_apply_folders(prepared,0);player_ui_free_folders(prepared);step();
 assert(lv_obj_get_scroll_y(list)==remembered);no_trails();
 // Shorter and empty models clamp old geometry, expose no old rows/track text.
 const char *short_names[]={"All music","Jazz"};
 prepared=player_ui_prepare_folders(short_names,2);player_ui_apply_folders(prepared,1);player_ui_free_folders(prepared);step();
 assert(lv_obj_get_scroll_y(list)==0);tap(1010,266,PLAYER_UI_FOLDER_SELECT);assert(values.back()==1);
 assert(find_text(player_ui_root(),"2 FOLDERS"));no_trails();
 prepared=player_ui_prepare_folders(nullptr,10);player_ui_apply_folders(prepared,SIZE_MAX);player_ui_free_folders(prepared);step();
 assert(find_text(player_ui_root(),"No folders on SD card")&&find_text(player_ui_root(),"0 FOLDERS"));
 before_actions=actions.size();tap(1010,266);assert(actions.size()==before_actions);no_trails();
 // The controller owns folder filtering/navigation; publish its resulting Queue
 // state and a temporary label, then exercise settings after that label expires.
 char folder_label[]="Selected folder";state.folder_label=folder_label;state.library_tab=0;player_ui_set_state(&state);step();
 strcpy(folder_label,"overwritten");player_ui_open_settings();step();control(11,-1);
 assert(find_text(player_ui_root(),"Selected folder"));state.folder_label=nullptr;state.library_tab=0;player_ui_set_state(&state);
 prepared=player_ui_prepare_folders(short_names,2);player_ui_apply_folders(prepared,1);player_ui_free_folders(prepared);step();
 assert(visible_library_list()!=list);no_trails();
 puts("PASS: embedded Files is bounded to the original panel; 10000 folders, touch/scrollbar, indices, tab/settings transitions, shrink/empty models, string ownership and thumbnail suppression.");
}

int main(int argc,char**argv){
 setvbuf(stdout,nullptr,_IONBF,0);target_rgb565=argc>2&&!strcmp(argv[2],"--rgb565");std::string dir=argc>1?argv[1]:".";lv_init();auto*d=lv_display_create(1280,720);lv_display_set_color_format(d,target_rgb565?LV_COLOR_FORMAT_RGB565:LV_COLOR_FORMAT_XRGB8888);lv_display_set_buffers(d,buffer,nullptr,target_rgb565?sizeof buffer/2:sizeof buffer,LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(d,flush);auto*input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(input,read_pointer);
 player_ui_create(lv_screen_active(),apply,nullptr);
 const char*names[]={"Aurora Protocol","Midnight Circuit","Glass Ocean","Binary Bloom","Solar Drift","Echo Chamber"};const char*artists[]={"Hikari Collective","Lo-Fi Assembly","Nexis","Studio Kumo","Yugen","Vector Fields"};const char *paths[]={"/sdcard/Music/aurora.mp3","/sdcard/Music/midnight.mp3","/sdcard/Music/glass.wav","/sdcard/Music/binary.flac","/sdcard/Music/solar.aac","/sdcard/Music/echo.mp3"};player_ui_track_t rows[6]{};for(int i=0;i<6;i++)rows[i]={names[i],artists[i],uint32_t(276000-i*17000),false,paths[i]};
 state.title=names[0];state.artist=artists[0];state.album="Neon Tides";state.clock_text="14:28";state.firmware="1.3.17";state.speed_percent=100;state.position_ms=137000;state.duration_ms=276000;state.sample_rate=44100;state.bitrate_kbps=320;state.format="MP3";state.track_count=6;state.volume=65;state.battery_percent=78;state.battery_charging=true;state.bluetooth_available=true;state.dragon_type=1;state.dragon_speed=10;state.dragon_colors=true;state.clock24=true;state.brightness=100;state.animations=true;state.covers=true;
 player_ui_set_playlist(rows,6,0);player_ui_set_state(&state);float bands[56];int16_t wave[128];for(int i=0;i<56;i++)bands[i]=.17f+.68f*fabsf(sinf(i*.32f))*fabsf(cosf(i*.095f));for(int i=0;i<128;i++)wave[i]=int16_t(28000*sinf(i*.13f));player_ui_set_spectrum(bands,56,wave,128);step(8);player_ui_set_spectrum(bands,56,wave,128);step();clock_bounds(player_ui_root());no_trails();save(dir+"/aurora-native.ppm");
 if(argc>2&&!target_rgb565){for(int mode=0;mode<2;mode++){state.dragon_type=mode;player_ui_set_state(&state);for(int i=0;i<40;i++){step(4);char name[64];snprintf(name,sizeof name,"/motion-%d-%02d.ppm",mode,i);save(dir+name);}}}
 state.animations=false;player_ui_set_state(&state);step();cover_bounds();filter_checks(dir);thumbnail_checks(paths,dir);artwork_ownership_checks(paths);
 // Charge/BT changes redraw immediately, including clearing every old bolt pixel.
 for(int i=0;i<6;i++){state.battery_charging=i%2;state.bluetooth_enabled=i%2;player_ui_set_state(&state);step();no_trails();}
 player_ui_open_settings();step();tap_text("Audio");step();auto *speed_label=find_text(player_ui_root(),"Turntable speed",false);assert(speed_label);auto *speed_page=lv_obj_get_parent(speed_label);lv_obj_t *speed_slider=nullptr;for(unsigned i=0;i<lv_obj_get_child_count(speed_page);i++){auto *o=lv_obj_get_child(speed_page,i);if(lv_obj_check_type(o,&lv_slider_class))speed_slider=o;}assert(speed_slider);step();
 lv_area_t sa;lv_obj_get_coords(speed_slider,&sa);tap(sa.x1+lv_area_get_width(&sa)*87/100,(sa.y1+sa.y2)/2,PLAYER_UI_TRACK_SPEED);assert(state.speed_percent>110&&state.speed_percent<=125);
 tap_text("Reset to 100%",PLAYER_UI_TRACK_SPEED);assert(state.speed_percent==100);save(dir+"/aurora-speed.ppm");control(8,PLAYER_UI_LIBRARY_TAB);
 player_ui_open_settings();step();tap_text("Library");step();save(dir+"/aurora-library-settings.ppm");control(8,PLAYER_UI_LIBRARY_TAB);
 const int ids[]={0,1,2,3,4,5,6,8,9,10,12,13,14,15,16,17,18,19,20,21};const int expected[]={PLAYER_UI_FAVORITE,PLAYER_UI_SHUFFLE,PLAYER_UI_PREVIOUS,PLAYER_UI_PLAY_PAUSE,PLAYER_UI_NEXT,PLAYER_UI_REPEAT,PLAYER_UI_MUTE,PLAYER_UI_LIBRARY_TAB,PLAYER_UI_LIBRARY_TAB,PLAYER_UI_LIBRARY_TAB,PLAYER_UI_EFFECT,PLAYER_UI_EFFECT,PLAYER_UI_EFFECT,PLAYER_UI_EFFECT,PLAYER_UI_FORMAT_FILTER,PLAYER_UI_FORMAT_FILTER,PLAYER_UI_FORMAT_FILTER,PLAYER_UI_VISUAL_MODE,PLAYER_UI_VISUAL_MODE,PLAYER_UI_VISUAL_MODE};
 for(int i=0;i<20;i++)control(ids[i],expected[i]);tap(1060,49,PLAYER_UI_BLUETOOTH);tap(980,224,PLAYER_UI_SELECT_TRACK);tap(632,405,PLAYER_UI_SEEK_MS);tap(256,634,PLAYER_UI_VOLUME);
 for(int v:{0,1,2,10,30,65,99,100,50,0}){state.volume=v;state.position_ms=state.duration_ms*v/100;player_ui_set_state(&state);step();no_trails();}
 state.volume=65;state.muted=false;state.playing=false;state.repeat=0;state.shuffle=false;state.favorite=false;state.library_tab=0;state.effect=0;state.format_filter=0;state.visual_mode=0;state.position_ms=137000;player_ui_set_state(&state);step();
 control(11,-1);save(dir+"/aurora-settings-home.ppm");tap(966,274);tap(966,274,PLAYER_UI_DRAGON_TYPE);assert(state.dragon_type==0);tap(1146,274,PLAYER_UI_DRAGON_TYPE);assert(state.dragon_type==1);tap(1156,370,PLAYER_UI_DRAGON_SPEED);assert(state.dragon_speed>=4&&state.dragon_speed<=16);tap(1180,438,PLAYER_UI_DRAGON_COLORS);save(dir+"/aurora-settings.ppm");control(8,PLAYER_UI_LIBRARY_TAB);
 // Exported CSS pressed frame differs while held and exactly restores on release.
 state.animations=false;player_ui_set_state(&state);step();std::vector<uint32_t> idle(pixels,pixels+1280*720);const auto &pr=aurora_controls[3];pointer={pr.x+pr.w/2,pr.y+pr.h/2};pressed=true;step(4);assert(!std::equal(idle.begin(),idle.end(),pixels));save(dir+"/aurora-pressed.ppm");pressed=false;step();state.playing=false;player_ui_set_state(&state);step();no_trails();
 // Original HTML gesture: dragging the knob upward raises volume immediately.
 const auto &kn=aurora_controls[7];state.volume=40;player_ui_set_state(&state);step();pointer={kn.x+kn.w/2,kn.y+kn.h/2};pressed=true;step();pointer.y-=24;step();assert(state.volume==60);pressed=false;step();no_trails();
 // Battery tap is inert; one long press triggers one hidden screenshot command.
 size_t ba=actions.size();tap(1170,45);assert(actions.size()==ba);pointer={1170,45};pressed=true;step(40);pressed=false;step();assert(actions.size()==ba+1&&actions.back()==PLAYER_UI_SCREENSHOT);
 state.animations=true;player_ui_set_state(&state);step();player_ui_set_sleeping(true);step();uint64_t sleep_pixels=flushed;size_t sleep_actions=actions.size();step(80);assert(flushed==sleep_pixels);for(auto pixel:pixels)assert((pixel&0xffffff)==0);save(dir+"/aurora-screensaver.ppm");tap(613,495);assert(actions.size()==sleep_actions);player_ui_set_sleeping(false);step();no_trails();
 // Keep audio phase fixed. Silence and a transient must produce different images in all three modes.
 state.playing=false;for(int mode=0;mode<3;mode++){
  state.visual_mode=mode;player_ui_set_state(&state);float zero[56]{};int16_t flat[128]{};
  for(int n=0;n<16;n++)player_ui_set_spectrum(zero,56,flat,128);step();
  std::vector<uint32_t> quiet;for(int y=251;y<361;y++)for(int x=386;x<839;x++)quiet.push_back(pixels[y*1280+x]);
  player_ui_set_spectrum(bands,56,wave,128);step();bool changed=false;size_t n=0;
  for(int y=251;y<361;y++)for(int x=386;x<839;x++)changed|=quiet[n++]!=pixels[y*1280+x];assert(changed);
  save(dir+"/aurora-visual-"+std::to_string(mode)+".ppm");printf("Visual redraw check mode %d\n",mode);no_trails();
 }
 state.visual_mode=0;state.animations=false;player_ui_set_state(&state);step();
 // Real-time visualization is sampled by the UI timer without controller calls.
 state.animations=true;state.playing=true;player_ui_set_state(&state);player_ui_set_visual_source(visual_source,nullptr);step(40);assert(visual_source_calls>15);
 player_ui_set_sleeping(true);unsigned sample_count=visual_source_calls;step(40);assert(visual_source_calls==sample_count);player_ui_set_sleeping(false);step();assert(visual_source_calls>sample_count);
 player_ui_set_overlay(true);sample_count=visual_source_calls;step(40);assert(visual_source_calls==sample_count);player_ui_set_overlay(false);player_ui_set_visual_source(nullptr,nullptr);
 state.playing=false;state.animations=false;player_ui_set_state(&state);step();
 // Virtual folders keep the same object count for ten thousand entries.
 auto *host=lv_obj_create(lv_screen_active());lv_obj_set_size(host,870,630);lv_obj_center(host);std::vector<std::string> folder_names;folder_names.reserve(10000);for(int i=0;i<10000;i++)folder_names.push_back("Collection / Artist "+std::to_string(i));std::vector<const char*> folders;for(auto &name:folder_names)folders.push_back(name.c_str());auto *fl=player_ui_create_folder_list(host,folders.data(),folders.size(),9999);step();assert(objects(fl)<50);assert(lv_obj_get_scroll_y(fl)>500000);lv_area_t fa;lv_obj_get_coords(fl,&fa);pointer={fa.x2-4,fa.y2-9};pressed=true;step();pointer.y=fa.y1+35;step();pressed=false;step();assert(lv_obj_get_scroll_y(fl)<100000);size_t no_pick=actions.size();int before_y=lv_obj_get_scroll_y(fl);pointer={fa.x1+300,fa.y1+350};pressed=true;step();for(int n=0;n<12;n++){pointer.y-=15;step(1);}pressed=false;step(12);assert(lv_obj_get_scroll_y(fl)>before_y+100);assert(actions.size()==no_pick);
 for(int position:{0,54321,200000,400000,539000,1234}){
  lv_obj_scroll_to_y(fl,position,LV_ANIM_OFF);step();int sy=lv_obj_get_scroll_y(fl);int expected_row=(sy+100)/54;
  size_t before_pick=actions.size();tap(fa.x1+300,fa.y1+100,PLAYER_UI_FOLDER_SELECT);
  assert(actions.size()==before_pick+1&&values.back()==expected_row);assert(objects(fl)<50);
 }
 save(dir+"/aurora-folders.ppm");lv_obj_delete(host);step();no_trails();
 embedded_folders_checks(dir);
 player_ui_open_settings();step();tap_text("Battery / Power");step();tap_text("Power off",PLAYER_UI_POWER_OFF);control(8,PLAYER_UI_LIBRARY_TAB);
 // Actual LVGL keyboard retains all rows inside its dialog; pointer types
 // first/last rows and space/backspace, rather than merely firing callbacks.
 auto *search=lv_obj_create(lv_layer_top());lv_obj_set_size(search,870,630);lv_obj_center(search);lv_obj_remove_flag(search,LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_set_style_bg_color(search,lv_color_hex(0x141615),0);lv_obj_set_style_bg_opa(search,255,0);
 auto *input_text=lv_textarea_create(search);lv_obj_set_pos(input_text,12,66);lv_obj_set_size(input_text,795,64);lv_textarea_set_one_line(input_text,true);
 auto *search_title=lv_label_create(search);lv_label_set_text(search_title,"SEARCH MUSIC / filename, title, artist, album");lv_obj_set_pos(search_title,12,6);lv_obj_set_style_text_color(search_title,lv_color_hex(0xEDEEE8),0);
 auto action_button=[&](const char *caption,int x,int y,int w){auto *b=lv_button_create(search);lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,46);lv_obj_set_style_bg_color(b,lv_color_hex(0xDFEB73),0);lv_obj_set_style_text_color(b,lv_color_hex(0x111312),0);auto *l=lv_label_create(b);lv_label_set_text(l,caption);lv_obj_center(l);};
 action_button("CLEAR FILTER",12,146,195);action_button("SEARCH",606,146,195);action_button("CLOSE",708,540,112);
 auto *keyboard=player_search_keyboard(search,input_text);step();lv_area_t ka,pa;lv_obj_get_coords(keyboard,&ka);lv_obj_get_coords(search,&pa);
 assert(ka.x1>=pa.x1&&ka.x2<=pa.x2&&ka.y1>pa.y1+190&&ka.y2<pa.y2-55&&ka.y2<720);
 auto keytap=[&](const char *caption){auto *matrix=(lv_buttonmatrix_t*)keyboard;for(unsigned i=0;i<matrix->btn_cnt;i++){const char *t=lv_buttonmatrix_get_button_text(keyboard,i);if(t&&!strcmp(t,caption)){auto a=matrix->button_areas[i];tap(ka.x1+(a.x1+a.x2)/2,ka.y1+(a.y1+a.y2)/2);return;}}assert(false);};
 keytap("a");keytap("z");keytap(" ");keytap("q");assert(!strcmp(lv_textarea_get_text(input_text),"az q"));keytap(LV_SYMBOL_BACKSPACE);assert(!strcmp(lv_textarea_get_text(input_text),"az "));
 save(dir+"/aurora-search.ppm");lv_obj_delete(search);step();no_trails();
 // Modal overlays suspend decorative repaints completely.
 state.animations=true;player_ui_set_state(&state);step();player_ui_set_overlay(true);std::vector<uint32_t> overlay(pixels,pixels+1280*720);step(40);assert(std::equal(overlay.begin(),overlay.end(),pixels));player_ui_set_overlay(false);

 state.dragon_type=0;state.animations=true;player_ui_set_state(&state);step(50);save(dir+"/aurora-living.ppm");state.dragon_type=1;player_ui_set_state(&state);step(30);save(dir+"/aurora-flight.ppm");state.animations=false;player_ui_set_state(&state);step();std::vector<uint32_t> frozen(pixels,pixels+1280*720);step(50);assert(std::equal(frozen.begin(),frozen.end(),pixels));
 const uint64_t unchanged=flushed;for(int i=0;i<8;i++){player_ui_set_state(&state);step();}assert(flushed==unchanged);puts("PASS: eight unchanged state updates paint zero pixels with animations off.");
 auto count=objects(player_ui_root());std::vector<player_ui_track_t> large(10000,rows[0]);auto *prepared=player_ui_prepare_playlist(large.data(),large.size());assert(prepared);player_ui_apply_playlist(prepared,9999);player_ui_free_playlist(prepared);state.track_index=9999;state.track_count=10000;player_ui_set_state(&state);step();assert(objects(player_ui_root())==count);tap(960,365,PLAYER_UI_SELECT_TRACK);assert(values.back()>9900);
 state.title="Żółć, długi tytuł — metadata must stay on one line and never overlap";state.artist="Very long artist with accented characters ĄĆĘŁŃÓŚŹŻ";state.album="Long album name";state.format="WAV";state.sample_rate=192000;state.bitrate_kbps=9216;state.bit_depth=24;player_ui_set_state(&state);step();save(dir+"/aurora-long-text.ppm");no_trails();
 state.title="Aurora Protocol";state.artist="Hikari Collective";state.album="Neon Tides";state.visual_mode=2;state.animations=true;state.playing=true;player_ui_set_state(&state);step();
 for(int n=0;n<90;n++){for(int j=0;j<56;j++)bands[j]=.10f+.75f*powf(std::max(0.f,sinf(n*.10f-j*.008f)),2);lv_tick_inc(33);player_ui_set_spectrum(bands,56,wave,128);lv_timer_handler();save(dir+"/particles-"+std::to_string(n)+".ppm");}
 printf("PASS: keyboard rows type via touch, black screen is fully dark with zero idle repaints and no click-through.\n");
 printf("PASS: %u touch actions; 10 volume/seek frames match full redraw; both dragon modes; settings; freeze; 10,000-track virtualization.\n",unsigned(actions.size()));
 lv_obj_delete(player_ui_root());return 0;
}
