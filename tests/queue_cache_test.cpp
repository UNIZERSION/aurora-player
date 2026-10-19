#undef NDEBUG /* zig -O1/-O2 predefines NDEBUG; keep assert() checks active */
#include "player_ui.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static bool reject_row_allocation=false,reject_row_snapshot=false;
static unsigned row_snapshots=0;
static lv_draw_buf_t *controlled_draw_buf(uint32_t width,uint32_t height,lv_color_format_t format,uint32_t stride){
 if(reject_row_allocation&&width==358&&height==72)return nullptr;
 return lv_draw_buf_create(width,height,format,stride);
}
static lv_result_t controlled_snapshot(lv_obj_t *object,lv_color_format_t format,lv_draw_buf_t *buffer){
 if(lv_obj_get_width(object)==358&&lv_obj_get_height(object)==72)++row_snapshots;
 if(reject_row_snapshot&&lv_obj_get_width(object)==358&&lv_obj_get_height(object)==72)return LV_RESULT_INVALID;
 return lv_snapshot_take_to_draw_buf(object,format,buffer);
}
#define lv_draw_buf_create controlled_draw_buf
#define lv_snapshot_take_to_draw_buf controlled_snapshot
#include "../main/player_ui.cpp"
#undef lv_draw_buf_create
#undef lv_snapshot_take_to_draw_buf

static uint16_t display_buffer[1280*128];
static void flush_test(lv_display_t *display,const lv_area_t *,uint8_t *){lv_display_flush_ready(display);}
static std::vector<uint8_t> pixels(const Row &row){return {row.cache->data,row.cache->data+row.cache->header.stride*row.cache->header.h};}
static void assert_live_fallback(const Row &row){
 assert(!row.cached&&!row.capturing);
 for(auto *o:{row.image,row.title,row.artist,row.duration,row.menu})assert(!lv_obj_has_flag(o,LV_OBJ_FLAG_HIDDEN));
 assert(lv_obj_has_flag(row.marker,LV_OBJ_FLAG_HIDDEN)!=row.selected);
}
int main(){
 setvbuf(stdout,nullptr,_IONBF,0);lv_init();
 auto *display=lv_display_create(1280,720);lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
 lv_display_set_buffers(display,display_buffer,nullptr,sizeof display_buffer,LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(display,flush_test);
 player_ui_create(lv_screen_active(),nullptr,nullptr);
 player_ui_state_t state{};state.title="Cache regression";state.artist="Artist";state.album="Album";state.clock_text="12:34";
 state.covers=true;state.sample_rate=44100;state.format="MP3";state.volume=65;state.battery_percent=70;state.track_index=0;
 state.animations=false;state.dragon_speed=10;state.speed_percent=100;player_ui_set_state(&state);
 std::vector<std::string> paths(1000),titles(1000);std::vector<player_ui_track_t> tracks(1000);
 for(size_t i=0;i<tracks.size();++i){paths[i]="/Music/track-"+std::to_string(i)+".mp3";titles[i]="Track "+std::to_string(i)+" with a longer title";tracks[i]={titles[i].c_str(),"Artist",uint32_t(190000+i),false,paths[i].c_str()};}
 player_ui_set_playlist(tracks.data(),tracks.size(),0);lv_obj_update_layout(player_ui_root());
 lv_draw_buf_t *allocations[ROWS];size_t bytes=0;
 for(int i=0;i<ROWS;++i){const auto &r=ui.rows[i];assert(r.cached&&r.cache);assert(r.cache->header.w==358&&r.cache->header.h==72&&r.cache->header.stride==716);allocations[i]=r.cache;bytes+=r.cache->header.stride*r.cache->header.h;}
 assert(bytes==412416);
 lv_refr_now(display);const unsigned saved_snapshots=row_snapshots;
 refresh_rows(true);assert(row_snapshots==saved_snapshots);
 for(int offset:{0,1,15,71,72,73,500,20000,50,0}){
  lv_obj_scroll_to_y(ui.playlist,offset,LV_ANIM_OFF);refresh_rows();lv_obj_update_layout(player_ui_root());
  for(int i=0;i<ROWS;++i){assert(ui.rows[i].cache==allocations[i]);assert(ui.rows[i].cached);}
 }
 auto &r=ui.rows[0];assert(r.index==0);auto before=pixels(r);
 uint16_t cover_pixels[56*56];std::fill_n(cover_pixels,56*56,uint16_t(0xf800));lv_image_dsc_t cover{};raw(cover,reinterpret_cast<uint8_t*>(cover_pixels),56,56,112);
 player_ui_set_thumbnail(paths[0].c_str(),&cover);assert(r.cached&&pixels(r)!=before);before=pixels(r);
 // Same path and same image descriptor, different bytes: no stale snapshot.
 std::fill_n(cover_pixels,56*56,uint16_t(0x07e0));player_ui_set_thumbnail(paths[0].c_str(),&cover);assert(pixels(r)!=before);before=pixels(r);
 auto changed=tracks[0];changed.title="Updated title";changed.duration_ms=300000;player_ui_update_track(0,&changed);assert(pixels(r)!=before);before=pixels(r);
 player_ui_clear_thumbnails();assert(pixels(r)!=before&&lv_image_get_src(r.image)==&dragon_thumb);
 player_ui_set_thumbnail(paths[0].c_str(),&cover);before=pixels(r);
 player_ui_set_thumbnail(paths[0].c_str(),nullptr);assert(pixels(r)!=before&&lv_image_get_src(r.image)==&dragon_thumb);
 player_ui_clear_thumbnails();
 for(int i=0;i<ROWS;++i)player_ui_set_thumbnail(paths[i].c_str(),&cover);
 // Rows 5..7 are prefetched but outside the 328px viewport. Reusing an LRU
 // descriptor must replace their rasterized art too, not leave its old owner.
 before=pixels(ui.rows[5]);const auto *old_image=lv_image_get_src(ui.rows[5].image);
 player_ui_set_thumbnail(paths[900].c_str(),&cover);
 assert(lv_image_get_src(ui.rows[5].image)!=old_image&&lv_image_get_src(ui.rows[5].image)==&dragon_thumb);
 assert(pixels(ui.rows[5])!=before);
 lv_image_cache_drop(r.cache);lv_draw_buf_destroy(r.cache);r.cache=nullptr;
 reject_row_allocation=true;cache_row(r);assert_live_fallback(r);
 reject_row_allocation=false;cache_row(r);assert(r.cached);assert(r.cache);
 reject_row_snapshot=true;cache_row(r);assert_live_fallback(r);
 reject_row_snapshot=false;cache_row(r);assert(r.cached);
 for(auto *o:{r.image,r.title,r.artist,r.duration,r.marker})assert(lv_obj_has_flag(o,LV_OBJ_FLAG_HIDDEN));
 assert(!lv_obj_has_flag(r.menu,LV_OBJ_FLAG_HIDDEN));
 lv_obj_scroll_to_y(ui.playlist,71900,LV_ANIM_OFF);refresh_rows();
 player_ui_set_playlist(nullptr,0,0);for(auto &row:ui.rows)assert(!row.initialized&&lv_obj_has_flag(row.box,LV_OBJ_FLAG_HIDDEN));
 player_ui_set_playlist(tracks.data(),1,0);assert(ui.rows[0].cached&&ui.rows[0].initialized&&ui.rows[0].index==0);
 player_ui_set_playlist(tracks.data(),tracks.size(),0);for(auto &row:ui.rows)assert(row.cached&&row.initialized);
 lv_obj_delete(ui.root);for(auto &row:ui.rows)assert(row.cache==nullptr&&!row.cached);
 puts("PASS: Queue cache has 8 reusable 358x72 RGB565 rows (412416 pixel bytes); thumbnails, reused descriptors, metadata and clear invalidate caches; allocation/snapshot errors retain live labels; indicator stays live; deletion releases all caches.");
 return 0;
}
