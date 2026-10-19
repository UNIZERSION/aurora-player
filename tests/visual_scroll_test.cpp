#undef NDEBUG /* zig -O1/-O2 predefines NDEBUG; keep assert() checks active */
#define main original_preview_main
#include "aurora_ui_preview.cpp"
#undef main
int main(){
 setvbuf(stdout,nullptr,_IONBF,0);target_rgb565=true;lv_init();
 auto *display=lv_display_create(1280,720);lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
 lv_display_set_buffers(display,buffer,nullptr,sizeof(buffer)/2,LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(display,flush);
 player_ui_create(lv_screen_active(),apply,nullptr);
 state.title="Scroll test";state.artist="Test";state.album="Test";state.clock_text="14:28";state.firmware="1.3.14";
 state.animations=true;state.playing=true;state.volume=65;state.speed_percent=100;state.dragon_speed=10;
 std::vector<player_ui_track_t> tracks(1000,{"Track","Artist",200000,false,"/Music/test.mp3"});
 player_ui_set_playlist(tracks.data(),tracks.size(),0);state.track_count=tracks.size();player_ui_set_state(&state);
 player_ui_set_visual_source(visual_source,nullptr);step(8);
 // Queue labels may be raster-cached; their owning list remains the touch target.
 auto *list=lv_obj_get_parent(lv_obj_get_parent(find_text(player_ui_root(),"Track",false)));assert(list);
 const unsigned baseline_objects=objects(player_ui_root());
 for(int mode=0;mode<3;++mode){
  state.visual_mode=mode;player_ui_set_state(&state);step(8);
  const unsigned before=visual_source_calls;
  // Every 25ms scroll event reproduces a finger continuously moving the list.
  for(int i=0;i<80;++i){lv_obj_scroll_to_y(list,300+i*5,LV_ANIM_OFF);lv_obj_send_event(list,LV_EVENT_SCROLL,nullptr);step(1);}
  const unsigned updates=visual_source_calls-before;
  printf("Visualizer %d: %u source updates during 2000ms continuous scroll\n",mode,updates);assert(updates>=20);
  assert(objects(player_ui_root())==baseline_objects);
 }
 player_ui_set_sleeping(true);step(8);const auto sleeping=visual_source_calls;step(12);assert(visual_source_calls==sleeping);
 player_ui_set_sleeping(false);player_ui_set_overlay(true);step(8);const auto hidden=visual_source_calls;step(12);assert(visual_source_calls==hidden);
 puts("PASS: all three visualizers remain live during continuous scrolling; sleep/modal still suspend work; bounded widget count.");
}
