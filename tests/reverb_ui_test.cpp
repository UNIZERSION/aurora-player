#undef NDEBUG
#include "aurora_reverb.inc"
#include <cassert>
#include <string>
#include <vector>
static uint32_t pixels[1280*720];static uint16_t buffer[1280*80];
static lv_point_t pointer{};static bool pressed=false,enabled=false,closed=false;static unsigned calls=0;
static lv_obj_t *panel;static audio_reverb_config_t config{1500,1400,38,0};
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *data){
 auto *p=(uint16_t*)data;for(int y=a->y1;y<=a->y2;y++)for(int x=a->x1;x<=a->x2;x++){const auto v=*p++;pixels[y*1280+x]=((uint32_t(v>>11)*255/31)<<16)|((uint32_t((v>>5)&63)*255/63)<<8)|(uint32_t(v&31)*255/31);}lv_display_flush_ready(d);
}
static void read_pointer(lv_indev_t*,lv_indev_data_t *data){data->point=pointer;data->state=pressed?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;}
static void step(int count=3){for(int i=0;i<count;i++){lv_tick_inc(25);lv_timer_handler();}}
static void tap(int x,int y){pointer={x,y};pressed=true;step(4);pressed=false;step(3);}
static void save(const std::string &path){FILE *f=fopen(path.c_str(),"wb");assert(f);fprintf(f,"P6\n1280 720\n255\n");for(auto p:pixels){const uint8_t c[]={uint8_t(p>>16),uint8_t(p>>8),uint8_t(p)};assert(fwrite(c,1,3,f)==3);}fclose(f);}
static void changed(int kind,int value){
 ++calls;switch(kind){case REVERB_AMOUNT:config.amount=value;break;case REVERB_DECAY:config.decay_ms=value;break;case REVERB_TONE:config.tone_hz=value;break;case REVERB_LOW_CUT:config.low_cut_hz=value;break;
 case REVERB_ENABLE:enabled=value;break;case REVERB_RESET:config={1500,1400,38,0};break;case REVERB_CLOSE:closed=true;return;default:assert(false);}
 reverb_dialog_refresh(panel,config,enabled,"Changes are saved automatically.");
}
static void tap_button(int x,int y){lv_area_t area;lv_obj_get_coords(panel,&area);tap(area.x1+x,area.y1+y);}
int main(int argc,char **argv){
 assert(argc==2);lv_init();auto *display=lv_display_create(1280,720);lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);lv_display_set_buffers(display,buffer,nullptr,sizeof buffer,LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(display,flush);
 auto *input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(input,read_pointer);
 lv_obj_set_style_bg_color(lv_screen_active(),lv_color_hex(0x101310),0);panel=lv_obj_create(lv_screen_active());lv_obj_set_style_bg_color(panel,lv_color_hex(0x141615),0);
 lv_obj_set_style_border_width(panel,1,0);lv_obj_set_style_border_color(panel,lv_color_hex(0x788177),0);auto *title=lv_label_create(panel);lv_label_set_text(title,"REVERB SETTINGS");lv_obj_set_style_text_color(title,lv_color_hex(0xeeefe8),0);
 reverb_dialog_build(panel,config,false,nullptr,changed);step();auto *s=aurora_reverb_detail::state(panel);assert(s&&s->bindings_used==7);
 lv_area_t area;lv_obj_get_coords(panel,&area);assert(area.x1==170&&area.y1==80&&area.x2==1109&&area.y2==639);
 const unsigned initial=calls;for(unsigned i=0;i<20;i++)reverb_dialog_refresh(panel,config,false,nullptr);assert(calls==initial);
 save(std::string(argv[1])+"/reverb-default.ppm");
 for(unsigned i=0;i<4;i++){
  lv_area_t rail;lv_obj_get_coords(s->sliders[i],&rail);assert(lv_area_get_width(&rail)==602);const int y=(rail.y1+rail.y2)/2;
  pointer={(rail.x1+rail.x2)/2,y};pressed=true;step(2);
  for(int x=rail.x1;x<=rail.x2;x+=30){pointer={x,y};step(1);}pointer={rail.x2,y};step(2);
  const int held=lv_slider_get_value(s->sliders[i]);auto stale=config;stale.amount=0;stale.decay_ms=500;stale.tone_hz=600;stale.low_cut_hz=0;reverb_dialog_refresh(panel,stale,false,nullptr);assert(lv_slider_get_value(s->sliders[i])==held);
  pressed=false;step(3);assert(held==(i==0?60:i==1?40:i==2?200:199));
 }
 // A high edge cannot be dragged through the lower edge.
 {lv_area_t rail;lv_obj_get_coords(s->sliders[REVERB_TONE],&rail);tap(rail.x1,(rail.y1+rail.y2)/2);assert(config.tone_hz==20000);}
 assert(config.amount==60&&config.decay_ms==4000&&config.tone_hz==20000&&config.low_cut_hz==19900);tap_button(815,45);assert(enabled);
 save(std::string(argv[1])+"/reverb-adjusted.ppm");tap_button(125,473);assert(config.amount==38&&config.decay_ms==1500&&config.tone_hz==1400);
 lv_refr_now(display);const std::vector<uint32_t> before(pixels,pixels+1280*720);lv_obj_invalidate(lv_screen_active());lv_refr_now(display);assert(std::equal(before.begin(),before.end(),pixels));
 tap_button(815,473);assert(closed);lv_obj_delete(panel);assert(lv_obj_get_child_count(lv_screen_active())==0);
 puts("PASS real LVGL reverb modal: 4 live touch sliders, 0..20kHz bounds and band ordering, held-slider protection, ON/OFF, Reset, Close, balanced940x560 layout, no stale rendering, clean widget teardown. DSP/persistence tested separately.");return 0;
}
