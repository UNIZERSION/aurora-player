#include "player_ui.h"
#include "src/draw/lv_draw_private.h"
#include "src/misc/lv_area_private.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
static uint16_t pixels[1280*720], buffer[1280*480];
static uint64_t flushed, draws, draw_pixels, label_draws, label_pixels, dragon_draws, visual_draws, thumbs;
static unsigned visual_calls;
static lv_point_t pointer; static bool pressed;
static std::vector<std::string> names, artists, paths;
static player_ui_state_t state{};
static std::vector<int> actions;
static std::vector<double> render_us;
static std::chrono::steady_clock::time_point rendering;
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p){
  const auto *src=(uint16_t*)p;
  for(int y=a->y1;y<=a->y2;++y){memcpy(pixels+y*1280+a->x1,src,lv_area_get_width(a)*2);src+=lv_area_get_width(a);}
  flushed+=lv_area_get_size(a);lv_display_flush_ready(d);
}
static void display_event(lv_event_t *e){
  if(lv_event_get_code(e)==LV_EVENT_RENDER_START)rendering=std::chrono::steady_clock::now();
  else if(lv_event_get_code(e)==LV_EVENT_RENDER_READY)render_us.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-rendering).count());
}
static void observe_draw(lv_event_t *e){
  auto *o=lv_event_get_target_obj(e);lv_area_t a,clip;lv_obj_get_coords(o,&a);auto *l=lv_event_get_layer(e);
  if(!lv_area_intersect(&clip,&a,&l->_clip_area))return;
  ++draws;draw_pixels+=lv_area_get_size(&clip);
  if(lv_obj_check_type(o,&lv_label_class)){++label_draws;label_pixels+=lv_area_get_size(&clip);}
  if(a.x1==123&&a.y1==415&&a.x2==254)++dragon_draws;
  if(a.x1==386&&a.y1==251&&a.x2==838)++visual_draws;
}
static void observe(lv_obj_t *o){lv_obj_add_event_cb(o,observe_draw,LV_EVENT_DRAW_MAIN,nullptr);for(unsigned i=0;i<lv_obj_get_child_count(o);i++)observe(lv_obj_get_child(o,i));}
static unsigned object_count(lv_obj_t *o){unsigned n=1;for(unsigned i=0;i<lv_obj_get_child_count(o);i++)n+=object_count(lv_obj_get_child(o,i));return n;}
static void read_pointer(lv_indev_t*,lv_indev_data_t *d){d->point=pointer;d->state=pressed?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;}
static void apply(player_ui_action_t a,int32_t v,void*){actions.push_back(a);if(a==PLAYER_UI_SELECT_TRACK){state.track_index=v;player_ui_set_state(&state);}}
static bool visual(float *bands,int16_t *wave,void*){++visual_calls;for(int i=0;i<32;i++)bands[i]=.02f+.92f*fabsf(sinf(i*.31f+visual_calls*.067f));for(int i=0;i<128;i++)wave[i]=int16_t(25000*sinf(i*.18f+visual_calls*.11f));return true;}
static void tick(unsigned ms=17){lv_tick_inc(ms);lv_timer_handler();}
static void settle(){for(int i=0;i<8;i++)tick();lv_refr_now(lv_display_get_default());}
static uint64_t checksum(){uint64_t h=1469598103934665603ULL;for(auto p:pixels){h=(h^uint8_t(p))*1099511628211ULL;h=(h^uint8_t(p>>8))*1099511628211ULL;}return h;}
static void checkpoint(FILE *f,int frame){
  lv_refr_now(lv_display_get_default());std::vector<uint16_t> before(pixels,pixels+1280*720);
  lv_obj_invalidate(player_ui_root());lv_refr_now(lv_display_get_default());
  for(size_t i=0;i<before.size();i++)if(before[i]!=pixels[i]){fprintf(stderr,"Partial/full mismatch frame=%d x=%u y=%u %04x != %04x\n",frame,unsigned(i%1280),unsigned(i/1280),before[i],pixels[i]);abort();}
  fprintf(f,"%d %016llx\n",frame,(unsigned long long)checksum());
}
static void supply_thumbnails(){
  size_t missing[8];size_t n=player_ui_get_missing_thumbnails(missing,8);static uint16_t texture[56*56];
  lv_image_dsc_t im{};im.header.magic=LV_IMAGE_HEADER_MAGIC;im.header.cf=LV_COLOR_FORMAT_RGB565;im.header.w=im.header.h=56;im.header.stride=112;im.data_size=sizeof texture;im.data=(uint8_t*)texture;
  for(size_t j=0;j<n;j++){size_t index=missing[j];for(int y=0;y<56;y++)for(int x=0;x<56;x++)texture[y*56+x]=uint16_t((((x+index*7)%32)<<11)|(((y+index*11)%64)<<5)|((x+y+index*3)%32));player_ui_set_thumbnail(paths[index].c_str(),&im);++thumbs;}
}
static lv_obj_t *playlist(){for(unsigned i=0;i<lv_obj_get_child_count(player_ui_root());i++){auto *o=lv_obj_get_child(player_ui_root(),i);if(lv_obj_is_visible(o)&&lv_obj_has_flag(o,LV_OBJ_FLAG_SCROLLABLE)&&lv_obj_get_width(o)==367&&lv_obj_get_height(o)==328)return o;}return nullptr;}
static double percentile(std::vector<double> a,double p){if(a.empty())return 0;std::sort(a.begin(),a.end());return a[std::min(a.size()-1,size_t(p*(a.size()-1)))];}
static double total(const std::vector<double> &a){double n=0;for(double t:a)n+=t;return n;}
int main(int argc,char **argv){
  assert(argc==5);setvbuf(stdout,nullptr,_IONBF,0);const int mode=atoi(argv[2]),dragon=atoi(argv[3]);const bool animations=atoi(argv[4]);
  lv_init();auto *d=lv_display_create(1280,720);lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);lv_display_set_buffers(d,buffer,nullptr,sizeof buffer,LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(d,flush);lv_display_add_event_cb(d,display_event,LV_EVENT_ALL,nullptr);
  auto *input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(input,read_pointer);
  player_ui_create(lv_screen_active(),apply,nullptr);
  for(int i=0;i<1000;i++){names.push_back(i%3?"Track "+std::to_string(i)+" — A detailed song title":"A very long song title with accented letters Żółć "+std::to_string(i));artists.push_back("Artist / Collaboration "+std::to_string(i));paths.push_back("/Music/Album/"+std::to_string(i)+".mp3");}
  std::vector<player_ui_track_t> tracks;for(int i=0;i<1000;i++)tracks.push_back({names[i].c_str(),artists[i].c_str(),uint32_t(160000+i*13),bool(i%5==0),paths[i].c_str()});
  state.title=names[0].c_str();state.artist=artists[0].c_str();state.album="Benchmark";state.clock_text="14:28";state.firmware="benchmark";state.format="MP3";state.sample_rate=44100;state.bitrate_kbps=320;state.duration_ms=160000;state.animations=animations;state.playing=true;state.covers=true;state.volume=65;state.speed_percent=100;state.dragon_speed=10;state.dragon_colors=true;state.dragon_type=dragon;state.visual_mode=mode;state.track_count=1000;
  player_ui_set_playlist(tracks.data(),tracks.size(),0);player_ui_set_state(&state);player_ui_set_visual_source(visual,nullptr);settle();auto *list=playlist();assert(list);supply_thumbnails();settle();observe(player_ui_root());
  const auto objects=object_count(player_ui_root());FILE *hashes=fopen((std::string(argv[1])+".hashes").c_str(),"w");assert(hashes);
  std::vector<double> handler_us;uint64_t measured_flush=0,measured_draws=0,measured_draw_pixels=0,measured_label_draws=0,measured_label_pixels=0,measured_dragon=0,measured_visual=0;unsigned samples=0;int moved=0;
  render_us.clear();visual_calls=0;thumbs=0;actions.clear();
  for(int frame=0;frame<600;frame++){
    // One real touch drag per 60 frames, with release/inertial motion. No
    // synthetic scroll events bypass the production row recycling callbacks.
    const int phase=frame%60;pointer={1100,480-std::min(phase,27)*10};pressed=phase<28;
    const uint64_t bf=flushed,bd=draws,bdp=draw_pixels,bl=label_draws,blp=label_pixels,bdr=dragon_draws,bv=visual_draws;
    const auto start=std::chrono::steady_clock::now();tick();supply_thumbnails();
    handler_us.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count());
    measured_flush+=flushed-bf;measured_draws+=draws-bd;measured_draw_pixels+=draw_pixels-bdp;measured_label_draws+=label_draws-bl;measured_label_pixels+=label_pixels-blp;measured_dragon+=dragon_draws-bdr;measured_visual+=visual_draws-bv;++samples;
    if(phase==27&&lv_obj_get_scroll_y(list)>moved)moved=lv_obj_get_scroll_y(list);
    // Full redraw oracle/checksum outside measured interval. Keep timings for
    // scroll only: oracle render events are removed from the result sample.
    if(frame%60==59){const auto n=render_us.size();checkpoint(hashes,frame);render_us.resize(n);}
    assert(object_count(player_ui_root())==objects);
  }
  assert(moved>1000);assert(actions.empty());if(animations){assert(visual_calls>=250);assert(measured_dragon>100);assert(measured_visual>100);}else assert(!visual_calls);
  const auto measured_render=render_us;const unsigned measured_source=visual_calls;const uint64_t measured_thumbs=thumbs;
  // Cover replacement, title/artist/duration, selection, held row and keyboard
  // overlays must survive cache invalidation and remain pixel-identical.
  pressed=false;lv_obj_scroll_to_y(list,0,LV_ANIM_OFF);settle();supply_thumbnails();settle();checkpoint(hashes,600);
  tracks[2].title="Replacement long title — track 2";tracks[2].artist="Updated artist";tracks[2].duration_ms=245000;tracks[2].favorite=true;player_ui_update_track(2,&tracks[2]);state.track_index=2;player_ui_set_state(&state);settle();checkpoint(hashes,601);
  static uint16_t changed_thumb[56*56];std::fill(changed_thumb,changed_thumb+56*56,0xf81f);lv_image_dsc_t changed{};changed.header.magic=LV_IMAGE_HEADER_MAGIC;changed.header.cf=LV_COLOR_FORMAT_RGB565;changed.header.w=changed.header.h=56;changed.header.stride=112;changed.data_size=sizeof changed_thumb;changed.data=(uint8_t*)changed_thumb;player_ui_set_thumbnail(paths[2].c_str(),&changed);settle();checkpoint(hashes,602);
  pointer={1040,355};pressed=true;for(int i=0;i<5;i++)tick();checkpoint(hashes,603);pressed=false;settle();checkpoint(hashes,604);
  player_ui_keyboard(LV_KEY_DOWN);settle();checkpoint(hashes,605);player_ui_keyboard(LV_KEY_DOWN);settle();checkpoint(hashes,606);
  // Recache every currently visible row while keyboard focus exists, then
  // move that focus. A baked highlight would remain in an old cached row.
  for(int i=0;i<8;i++){player_ui_set_thumbnail(paths[i].c_str(),&changed);tracks[i].duration_ms+=1000;player_ui_update_track(i,&tracks[i]);}settle();checkpoint(hashes,610);
  player_ui_keyboard(LV_KEY_DOWN);settle();checkpoint(hashes,611);player_ui_keyboard(LV_KEY_UP);settle();checkpoint(hashes,612);
  state.covers=false;player_ui_set_state(&state);settle();checkpoint(hashes,607);state.covers=true;player_ui_set_state(&state);settle();checkpoint(hashes,608);player_ui_clear_thumbnails();settle();checkpoint(hashes,609);
  fclose(hashes);
  // Lifecycle guard: obscured/black screens must not waste animation work.
  player_ui_set_sleeping(true);settle();auto sleeping=visual_calls;settle();assert(visual_calls==sleeping);player_ui_set_sleeping(false);player_ui_set_overlay(true);settle();auto hidden=visual_calls;settle();assert(visual_calls==hidden);
  printf("{\"mode\":%d,\"dragon\":%d,\"animations\":%s,\"simulated_ms\":10200,\"steps\":%u,\"objects\":%u,\"scroll_y\":%d,\"source_updates\":%u,\"thumbnail_arrivals\":%llu,\"flushed_pixels\":%llu,\"draw_calls\":%llu,\"draw_pixels\":%llu,\"label_draw_calls\":%llu,\"label_draw_pixels\":%llu,\"dragon_draw_calls\":%llu,\"visual_draw_calls\":%llu,\"handler_us_median\":%.3f,\"handler_us_p95\":%.3f,\"handler_us_max\":%.3f,\"handler_us_total\":%.3f,\"render_us_total\":%.3f,\"render_us_median\":%.3f,\"render_us_p95\":%.3f}\n",mode,dragon,animations?"true":"false",samples,objects,moved,measured_source,(unsigned long long)measured_thumbs,(unsigned long long)measured_flush,(unsigned long long)measured_draws,(unsigned long long)measured_draw_pixels,(unsigned long long)measured_label_draws,(unsigned long long)measured_label_pixels,(unsigned long long)measured_dragon,(unsigned long long)measured_visual,percentile(handler_us,.5),percentile(handler_us,.95),percentile(handler_us,1),total(handler_us),total(measured_render),percentile(measured_render,.5),percentile(measured_render,.95));
}
