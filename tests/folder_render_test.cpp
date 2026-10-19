#undef NDEBUG /* zig -O1/-O2 predefines NDEBUG; keep assert() checks active */
// Same production UI and folder implementation, with the two modal parents
// used before/after the fix. This measures host work, not Tab5 frame rate.
#include "player_ui.h"
#include "src/draw/lv_draw_private.h"
#include "src/misc/lv_area_private.h"
#include "src/misc/lv_event_private.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
static uint16_t pixels[1280*720],buffer[1280*480];
static uint64_t player_draw_calls,player_draw_pixels;
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p){
 auto *src=(uint16_t*)p;
 for(int y=a->y1;y<=a->y2;y++)for(int x=a->x1;x<=a->x2;x++)pixels[y*1280+x]=*src++;
 lv_display_flush_ready(d);
}
static void count_draw(lv_event_t *e){
 ++player_draw_calls;
 lv_area_t a,clipped;lv_obj_get_coords(lv_event_get_target_obj(e),&a);
 auto *layer=lv_event_get_layer(e);
 if(lv_area_intersect(&clipped,&a,&layer->_clip_area))player_draw_pixels+=lv_area_get_size(&clipped);
}
static void observe(lv_obj_t*o){
 lv_obj_add_event_cb(o,count_draw,LV_EVENT_DRAW_MAIN,nullptr);
 for(unsigned i=0;i<lv_obj_get_child_count(o);++i)observe(lv_obj_get_child(o,i));
}
static void step(){lv_tick_inc(17);lv_timer_handler();lv_refr_now(lv_display_get_default());}
static void background_oracle(const std::vector<const char*>&names){
 unsigned comparisons=0,cache_checks=0;uint64_t old_fill=0,new_fill=0;
 for(size_t count:{size_t(0),size_t(1),size_t(2),size_t(8),size_t(9),size_t(11),size_t(50),size_t(10000)}){
  auto *dialog=lv_obj_create(lv_screen_active());lv_obj_set_size(dialog,870,630);lv_obj_center(dialog);
  auto *list=player_ui_create_folder_list(dialog,names.data(),count,count?count-1:0);step();
  for(int position:{-20,0,1,13,51,52,53,54,55,129,int(count*27),int(count*54)}){
   lv_obj_scroll_to_y(list,position,LV_ANIM_OFF);step();
   // Independently capture the live label/background again, using LVGL's
   // original compact snapshot, and compare every visible pixel to the padded
   // cache before using that cache in the background oracle.
   for(unsigned i=0;i<lv_obj_get_child_count(list);++i){
    auto *box=lv_obj_get_child(list,i);lv_obj_t *picture=nullptr,*caption=nullptr;
    for(unsigned j=0;j<lv_obj_get_child_count(box);++j){auto *child=lv_obj_get_child(box,j);if(lv_obj_check_type(child,&lv_image_class))picture=child;if(lv_obj_check_type(child,&lv_label_class))caption=child;}
    if(!picture||!caption||lv_obj_has_flag(box,LV_OBJ_FLAG_HIDDEN)||lv_obj_has_flag(picture,LV_OBJ_FLAG_HIDDEN))continue;
    auto *padded=static_cast<const lv_draw_buf_t*>(lv_image_get_src(picture));assert(padded&&padded->header.stride==1592&&padded->header.w==795);
    lv_obj_add_flag(picture,LV_OBJ_FLAG_HIDDEN);lv_obj_remove_flag(caption,LV_OBJ_FLAG_HIDDEN);lv_obj_set_style_bg_opa(box,255,0);
    auto *compact=lv_snapshot_take(box,LV_COLOR_FORMAT_RGB565);assert(compact&&compact->header.stride==1590);
    for(unsigned y=0;y<52;++y)assert(!memcmp(compact->data+y*1590,padded->data+y*1592,1590));
    lv_draw_buf_destroy(compact);++cache_checks;
    lv_obj_remove_flag(picture,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(caption,LV_OBJ_FLAG_HIDDEN);lv_obj_set_style_bg_opa(box,0,0);
   }
   step();
   for(bool fallback:{false,true}){
    if(fallback){
     for(unsigned i=0;i<lv_obj_get_child_count(list);++i){auto *box=lv_obj_get_child(list,i);
      for(unsigned j=0;j<lv_obj_get_child_count(box);++j){auto *child=lv_obj_get_child(box,j);if(lv_obj_check_type(child,&lv_image_class))lv_obj_add_flag(child,LV_OBJ_FLAG_HIDDEN);if(lv_obj_check_type(child,&lv_label_class)){lv_obj_remove_flag(child,LV_OBJ_FLAG_HIDDEN);lv_obj_set_style_bg_opa(box,255,0);}}
     }step();
    }
    lv_obj_invalidate(list);lv_refr_now(lv_display_get_default());std::vector<uint16_t> optimized(pixels,pixels+1280*720);
    lv_event_dsc_t *custom=nullptr;for(unsigned i=0;i<lv_obj_get_event_count(list);++i){auto *e=lv_obj_get_event_dsc(list,i);if(e->filter==(LV_EVENT_DRAW_MAIN|LV_EVENT_PREPROCESS))custom=e;}
    assert(custom);auto callback=custom->cb;auto *user=custom->user_data;assert(lv_obj_remove_event_dsc(list,custom));
    lv_obj_invalidate(list);lv_refr_now(lv_display_get_default());assert(std::equal(optimized.begin(),optimized.end(),pixels));++comparisons;
    lv_obj_add_event_cb(list,callback,static_cast<lv_event_code_t>(LV_EVENT_DRAW_MAIN|LV_EVENT_PREPROCESS),user);
    if(!fallback&&count==10000&&position==129){
     lv_area_t viewport;lv_obj_get_coords(list,&viewport);old_fill=lv_area_get_size(&viewport);new_fill=old_fill;
     for(unsigned i=0;i<lv_obj_get_child_count(list);++i){auto *box=lv_obj_get_child(list,i);if(lv_obj_has_flag(box,LV_OBJ_FLAG_HIDDEN))continue;
      for(unsigned j=0;j<lv_obj_get_child_count(box);++j){auto *child=lv_obj_get_child(box,j);if(!lv_obj_check_type(child,&lv_image_class)||lv_obj_has_flag(child,LV_OBJ_FLAG_HIDDEN))continue;lv_area_t row,clipped;lv_obj_get_coords(box,&row);if(lv_area_intersect(&clipped,&row,&viewport))new_fill-=lv_area_get_size(&clipped);}
     }
    }
    if(fallback){
     for(unsigned i=0;i<lv_obj_get_child_count(list);++i){auto *box=lv_obj_get_child(list,i);
      for(unsigned j=0;j<lv_obj_get_child_count(box);++j){auto *child=lv_obj_get_child(box,j);if(lv_obj_check_type(child,&lv_image_class)&&lv_image_get_src(child))lv_obj_remove_flag(child,LV_OBJ_FLAG_HIDDEN);if(lv_obj_check_type(child,&lv_label_class)){lv_obj_add_flag(child,LV_OBJ_FLAG_HIDDEN);lv_obj_set_style_bg_opa(box,0,0);}}
     }step();
    }
   }
  }
  lv_obj_delete(dialog);step();
 }
 printf("PASS: %u full-screen background comparisons including live-label fallback; %u padded caches match independent compact snapshots byte-for-byte.\n",comparisons,cache_checks);
 printf("Folder full viewport background fill: %llu -> %llu pixels (remaining opaque row images unchanged).\n",old_fill,new_fill);
}
struct Result {std::vector<uint16_t> image;std::vector<std::vector<uint16_t>> checkpoints;uint64_t calls,pixels,us;};
static Result run(bool top,const std::vector<const char*>&names){
 player_ui_set_overlay(true);
 auto *dialog=lv_obj_create(top?lv_layer_top():lv_screen_active());
 lv_obj_set_size(dialog,870,630);lv_obj_center(dialog);
 lv_obj_set_style_bg_color(dialog,lv_color_hex(0x141615),0);
 lv_obj_set_style_bg_opa(dialog,255,0);lv_obj_set_style_shadow_width(dialog,0,0);
 lv_obj_set_style_border_color(dialog,lv_color_hex(0xADAEA6),0);
 lv_obj_set_style_border_width(dialog,2,0);lv_obj_set_style_radius(dialog,6,0);
 lv_obj_remove_flag(dialog,LV_OBJ_FLAG_SCROLLABLE);
 auto *list=player_ui_create_folder_list(dialog,names.data(),names.size(),0);
 step();player_draw_calls=player_draw_pixels=0;
 uint64_t us=0;std::vector<std::vector<uint16_t>> checkpoints;
 for(int i=1;i<=300;++i){
  const auto start=std::chrono::steady_clock::now();
  lv_obj_scroll_to_y(list,i*7,LV_ANIM_OFF);step();
  us+=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
  if(i%50==0)checkpoints.emplace_back(pixels,pixels+1280*720);
 }
 Result result{{pixels,pixels+1280*720},std::move(checkpoints),player_draw_calls,player_draw_pixels,us};
 lv_obj_delete(dialog);player_ui_set_overlay(false);step();return result;
}
int main(){
 lv_init();auto*d=lv_display_create(1280,720);lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);
 lv_display_set_buffers(d,buffer,nullptr,sizeof buffer,LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(d,flush);
 // Since 1.3.16 the real player is prepared behind a separate startup screen.
 // Exercise folder raster caches in that exact inactive-screen state before
 // any first Files render, not only updates on an already visible player.
 auto *startup_screen=lv_screen_active();
 auto *main_screen=lv_obj_create(nullptr);
 lv_obj_set_style_bg_color(main_screen,lv_color_black(),0);
 player_ui_create(main_screen,nullptr,nullptr);
 player_ui_state_t state{};state.title="Aurora Protocol";state.artist="Hikari Collective";state.album="Neon Tides";
 state.clock_text="14:28";state.firmware="1.3.6";state.speed_percent=100;state.duration_ms=276000;
 state.sample_rate=44100;state.bitrate_kbps=320;state.format="MP3";state.volume=65;state.battery_percent=78;
 state.dragon_speed=10;state.clock24=true;state.brightness=100;state.animations=false;
 player_ui_set_state(&state);
 std::vector<std::string> storage;storage.reserve(10000);
 for(int i=0;i<10000;++i)storage.push_back("Collection / Artist "+std::to_string(i));
 std::vector<const char*> names;for(const auto&s:storage)names.push_back(s.c_str());
 player_ui_track_t boot_track{"Saved track","Saved artist",123000,false,"/sdcard/Music/Test.mp3"};
 player_ui_set_playlist(&boot_track,1,0);
 auto *boot_folders=player_ui_prepare_folders(names.data(),names.size());assert(boot_folders);
 player_ui_apply_folders(boot_folders,0);player_ui_free_folders(boot_folders);
 state.library_tab=1;state.track_count=1;player_ui_set_state(&state);
 assert(lv_screen_active()==startup_screen&&!lv_obj_is_visible(player_ui_root()));
 lv_screen_load(main_screen);lv_obj_delete(startup_screen);step();
 std::vector<uint16_t> first_files(pixels,pixels+1280*720);
 boot_folders=player_ui_prepare_folders(names.data(),names.size());assert(boot_folders);
 player_ui_apply_folders(boot_folders,0);player_ui_free_folders(boot_folders);step();
 assert(std::equal(first_files.begin(),first_files.end(),pixels));
 puts("PASS: 10000 folders prepared on an inactive startup screen render identically to fresh active-screen caches on first Files open.");
 state.library_tab=0;state.track_count=0;player_ui_set_playlist(nullptr,0,0);player_ui_set_state(&state);step();observe(player_ui_root());
 const auto before=run(true,names),after=run(false,names);
 assert(before.image==after.image);assert(before.checkpoints==after.checkpoints);
 assert(before.calls>0);assert(after.calls==0);assert(after.pixels==0);
 printf("PASS: 300 scroll frames with 10000 folders; six complete RGB565 checkpoints are byte-identical.\n");
 printf("Before layer_top: player draw calls=%llu, covered player pixels=%llu, host time=%llu us\n",before.calls,before.pixels,before.us);
 printf("After active screen: player draw calls=%llu, covered player pixels=%llu, host time=%llu us\n",after.calls,after.pixels,after.us);
 puts("Host duration compares rendering work only; not a physical Tab5 FPS measurement.");
 background_oracle(names);
 player_ui_track_t track{"Existing track","Existing artist",123000,false,"/sdcard/Test.mp3"};
 player_ui_set_playlist(&track,1,0);state.track_count=1;state.library_tab=0;player_ui_set_state(&state);step();
 auto *prepared=player_ui_prepare_folders(names.data(),names.size());assert(prepared);
 player_ui_apply_folders(prepared,0);player_ui_free_folders(prepared);state.library_tab=1;player_ui_set_state(&state);step();
 std::vector<uint16_t> partial(pixels,pixels+1280*720);lv_obj_invalidate(player_ui_root());lv_refr_now(lv_display_get_default());
 for(size_t i=0;i<partial.size();i++)if(partial[i]!=pixels[i]){printf("RGB565 embedded stale %u,%u %04x -> %04x\n",unsigned(i%1280),unsigned(i/1280),partial[i],pixels[i]);fflush(stdout);assert(false);}
 puts("PASS: first embedded Files transition in production RGB565 matches a complete redraw.");
}

