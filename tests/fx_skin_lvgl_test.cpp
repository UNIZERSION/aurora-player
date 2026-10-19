#undef NDEBUG /* zig -O1/-O2 predefines NDEBUG; keep assert() checks active */
#define main existing_preview_main
#include "aurora_ui_preview.cpp"
#undef main
#include "asset_pack.h"
#include "aurora_fx_skin.hpp"
extern "C" {
extern const uint8_t control_start[] asm("_binary_aurora_controls_pack_start");
extern const uint8_t control_end[] asm("_binary_aurora_controls_pack_end");
}
static uint32_t rgb565_to_pixel(uint16_t v){return 0xff000000|((uint32_t(v>>11)*255/31)<<16)|((uint32_t((v>>5)&63)*255/63)<<8)|(uint32_t(v&31)*255/31);}
static void check_skin(const std::vector<uint8_t>&original,int id,int frame){
 const auto&r=aurora_controls[id];
 for(int y=0;y<r.h;y++)for(int x=0;x<r.w;x++){
  if(id>=14&&x>=8&&x<r.w-8&&y>=6&&y<r.h-6)continue;
  uint16_t value;memcpy(&value,&original[r.offset+frame*r.stride*r.h+y*r.stride+x*2],2);
  assert(pixels[(r.y+y)*1280+r.x+x]==rgb565_to_pixel(value));
 }
}
int main(int argc,char**argv){
 setvbuf(stdout,nullptr,_IONBF,0);target_rgb565=true;std::string dir=argc>1?argv[1]:".";
 std::vector<uint8_t> original(AURORA_CONTROLS_BYTES);assert(aurora_unpack(control_start,size_t(control_end-control_start),original.data(),original.size()));auto prepared=original;aurora_fx_skin::prepare(prepared.data());
 for(int id=0;id<24;id++)for(int frame=0;frame<4;frame++){
  const auto&r=aurora_controls[id];
  for(int y=0;y<r.h;y++)for(int x=0;x<r.stride;x++){
   const bool interior=(id==14||id==15)&&y>=8&&y<r.h-8&&x>=36&&x<(r.w-18)*2;
   if(!interior){size_t at=r.offset+frame*r.stride*r.h+y*r.stride+x;assert(prepared[at]==original[at]);}
  }
 }
 assert(aurora_fx_skin::label_font.line_height==14&&aurora_fx_skin::foreground(false,false)==0xc2c7c0&&aurora_fx_skin::foreground(true,false)==0xe0ef72);
 lv_init();auto*d=lv_display_create(1280,720);lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);lv_display_set_buffers(d,buffer,nullptr,sizeof(buffer)/2,LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(d,flush);
 auto*input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(input,read_pointer);player_ui_create(lv_screen_active(),apply,nullptr);
 state.title="Aurora Protocol";state.artist="Hikari Collective";state.album="Neon Tides";state.clock_text="14:28";state.firmware="1.3.11";state.speed_percent=100;state.position_ms=137000;state.duration_ms=276000;state.sample_rate=44100;state.bitrate_kbps=320;state.format="MP3";state.volume=65;state.battery_percent=78;state.battery_charging=true;state.bluetooth_available=true;state.dragon_type=1;state.dragon_speed=10;state.dragon_colors=true;state.clock24=true;state.brightness=100;state.animations=false;state.covers=true;
 player_ui_set_state(&state);step(8);save(dir+"/aurora-fx-main.ppm");
 auto bt_off=std::vector<uint32_t>(pixels,pixels+1280*720);save(dir+"/bluetooth-off.ppm");state.bluetooth_enabled=true;player_ui_set_state(&state);step();save(dir+"/bluetooth-on.ppm");unsigned bt_changed=0;
 for(int y=0;y<720;y++)for(int x=0;x<1280;x++)if(pixels[y*1280+x]!=bt_off[y*1280+x]){assert(x>=1058&&x<1084&&y>=32&&y<67);++bt_changed;}
 assert(bt_changed>350&&bt_changed<26*35);const auto center=pixels[49*1280+1070]&0xffffff;assert(center<0x202020);state.bluetooth_available=false;player_ui_set_state(&state);step();assert(std::equal(bt_off.begin(),bt_off.end(),pixels));state.bluetooth_available=true;player_ui_set_state(&state);step();tap(1071,49,PLAYER_UI_BLUETOOTH);state.bluetooth_enabled=false;player_ui_set_state(&state);step();
 for(int id=12;id<=15;id++){
  const auto&r=aurora_controls[id];state.effect=id==12?1:0;player_ui_set_state(&state);step(8);check_skin(original,id,0);save(dir+"/fx-"+std::to_string(id)+"-off.ppm");
  pointer={r.x+r.w/2,r.y+r.h/2};size_t before=actions.size();pressed=true;step(4);assert(actions.size()==before);check_skin(original,id,2);save(dir+"/fx-"+std::to_string(id)+"-pressed.ppm");
  pressed=false;step(5);assert(actions.size()==before+1&&actions.back()==PLAYER_UI_EFFECT&&values.back()==id-12);check_skin(original,id,1);save(dir+"/fx-"+std::to_string(id)+"-on.ppm");
  before=actions.size();pressed=true;step(4);assert(actions.size()==before);check_skin(original,id,3);save(dir+"/fx-"+std::to_string(id)+"-pressed-on.ppm");pressed=false;step(5);assert(actions.size()==before+1&&values.back()==id-12);no_trails();
 }
 puts("PASS: all 24 original sprite records unchanged outside only two glyph interiors; Normal/Bass+ raster exact in four states; Reverb/EQ original frames exact in four states; matching Arial Bold12/color/pressed motion; Bluetooth centered 26x35 grey/green badge, dark rune, unavailable fallback, unchanged touch; callbacks and clean redraw.");
 lv_obj_delete(player_ui_root());return 0;
}
