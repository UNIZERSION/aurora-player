#pragma once
#include "lvgl.h"
#include "aurora_fonts.h"
#include "audio_fx.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

// Build/refresh only under LVGL lock. Callback must enqueue/coalesce actions;
// this view never performs DSP, filesystem or device work.
enum ReverbDialogAction { REVERB_AMOUNT, REVERB_DECAY, REVERB_TONE, REVERB_LOW_CUT, REVERB_RESET, REVERB_ENABLE, REVERB_CLOSE };
using ReverbDialogCallback=void(*)(int kind,int value);
namespace aurora_reverb_detail {
constexpr uint32_t lime=0xdfee79,white=0xeeeFe8,muted=0xa8b1ad,dark=0x141615;
struct State;
struct Binding {State *owner{};int kind{};};
struct State {
 lv_obj_t *panel{},*sliders[4]{},*values[4]{},*enable{},*enable_label{},*status{};
 Binding bindings[7]{};unsigned bindings_used=0;ReverbDialogCallback callback{};
 audio_reverb_config_t config{};bool updating=false,enabled=false;
};
inline void destroy(lv_event_t *e){delete static_cast<State*>(lv_event_get_user_data(e));}
inline State *state(lv_obj_t *panel){
 if(!panel)return nullptr;
 for(uint32_t i=0;i<lv_obj_get_event_count(panel);++i){auto *d=lv_obj_get_event_dsc(panel,i);if(lv_event_dsc_get_cb(d)==destroy)return static_cast<State*>(lv_event_dsc_get_user_data(d));}return nullptr;
}
inline void caption(lv_obj_t *o,const char *s){if(strcmp(lv_label_get_text(o),s))lv_label_set_text(o,s);}
inline lv_obj_t *text(lv_obj_t *p,const char *s,int x,int y,int width,uint32_t color=white){
 auto *o=lv_label_create(p);lv_obj_set_pos(o,x,y);lv_obj_set_width(o,width);lv_obj_set_style_text_font(o,&aurora_sans18,0);
 lv_obj_set_style_text_color(o,lv_color_hex(color),0);lv_label_set_long_mode(o,LV_LABEL_LONG_DOT);lv_label_set_text(o,s);return o;
}
inline Binding *bind(State *s,int kind){auto *b=&s->bindings[s->bindings_used++];*b={s,kind};return b;}
inline void value_text(State *s,int kind,int value){
 char text[24];if(kind==REVERB_AMOUNT)snprintf(text,sizeof text,"%d%%",value);
 else if(kind==REVERB_DECAY)snprintf(text,sizeof text,"%d.%d s",value/1000,(value%1000)/100);
 else if(!value)snprintf(text,sizeof text,"0 Hz (off)");
 else if(value>=1000)snprintf(text,sizeof text,"%d.%d kHz",value/1000,(value%1000)/100);
 else snprintf(text,sizeof text,"%d Hz",value);
 caption(s->values[kind],text);
}
inline void changed(lv_event_t *e){
 auto *b=static_cast<Binding*>(lv_event_get_user_data(e));auto *s=b->owner;if(s->updating)return;
 auto *slider=lv_event_get_target_obj(e);const int raw=lv_slider_get_value(slider);
 int value=b->kind==REVERB_AMOUNT?raw:raw*100;
 if(b->kind==REVERB_TONE)value=std::clamp(value,int(s->config.low_cut_hz)+100,20000);
 if(b->kind==REVERB_LOW_CUT)value=std::clamp(value,0,int(s->config.tone_hz)-100);
 if(b->kind==REVERB_TONE)s->config.tone_hz=value;
 if(b->kind==REVERB_LOW_CUT)s->config.low_cut_hz=value;
 if(b->kind==REVERB_TONE||b->kind==REVERB_LOW_CUT)lv_slider_set_value(slider,value/100,LV_ANIM_OFF);
 value_text(s,b->kind,value);if(s->callback)s->callback(b->kind,value);
}
inline void clicked(lv_event_t *e){
 auto *b=static_cast<Binding*>(lv_event_get_user_data(e));auto *s=b->owner;
 if(!s->updating&&s->callback)s->callback(b->kind,b->kind==REVERB_ENABLE?!s->enabled:0);
}
inline lv_obj_t *button(State *s,const char *label,int x,int y,int width,int kind){
 auto *o=lv_button_create(s->panel);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,width,58);
 lv_obj_set_style_radius(o,7,0);lv_obj_set_style_pad_all(o,0,0);lv_obj_set_style_shadow_width(o,0,0);
 lv_obj_set_style_border_width(o,1,0);lv_obj_set_style_border_color(o,lv_color_hex(0x788177),0);
 lv_obj_set_style_bg_color(o,lv_color_hex(0x252d28),0);lv_obj_set_style_bg_color(o,lv_color_hex(0x455138),LV_STATE_PRESSED);
 lv_obj_set_style_text_color(o,lv_color_hex(white),0);lv_obj_set_style_text_font(o,&aurora_sans18,0);
 auto *t=lv_label_create(o);lv_label_set_text(t,label);lv_obj_center(t);lv_obj_add_event_cb(o,clicked,LV_EVENT_CLICKED,bind(s,kind));return o;
}
}
inline void reverb_dialog_refresh(lv_obj_t *panel,const audio_reverb_config_t &config,bool enabled,const char *status){
 using namespace aurora_reverb_detail;auto *s=state(panel);if(!s)return;s->updating=true;
 auto next=config;
 if(lv_obj_has_state(s->sliders[REVERB_TONE],LV_STATE_PRESSED)||lv_obj_has_state(s->sliders[REVERB_LOW_CUT],LV_STATE_PRESSED)){
  next.tone_hz=s->config.tone_hz;next.low_cut_hz=s->config.low_cut_hz;
 }
 s->config=next;s->enabled=enabled;
 const int values[]={next.amount,next.decay_ms,next.tone_hz,next.low_cut_hz};
 for(unsigned i=0;i<4;++i)if(!lv_obj_has_state(s->sliders[i],LV_STATE_PRESSED)){
  const int raw=i?values[i]/100:values[i];if(lv_slider_get_value(s->sliders[i])!=raw)lv_slider_set_value(s->sliders[i],raw,LV_ANIM_OFF);value_text(s,i,values[i]);
 }
 caption(s->enable_label,enabled?"Reverb ON":"Reverb OFF");
 const auto bg=lv_color_hex(enabled?lime:0x252d28),fg=lv_color_hex(enabled?dark:white);
 if(!lv_color_eq(lv_obj_get_style_bg_color(s->enable,0),bg))lv_obj_set_style_bg_color(s->enable,bg,0);
 if(!lv_color_eq(lv_obj_get_style_text_color(s->enable_label,0),fg))lv_obj_set_style_text_color(s->enable_label,fg,0);
 caption(s->status,status&&*status?status:"Changes are saved automatically.");s->updating=false;
}
inline void reverb_dialog_build(lv_obj_t *panel,const audio_reverb_config_t &config,bool enabled,const char *status,ReverbDialogCallback callback){
 using namespace aurora_reverb_detail;if(!panel||state(panel))return;auto *s=new(std::nothrow) State;if(!s)return;
 s->panel=panel;s->callback=callback;lv_obj_add_event_cb(panel,destroy,LV_EVENT_DELETE,s);
 lv_obj_set_size(panel,940,560);lv_obj_center(panel);lv_obj_set_style_pad_all(panel,0,0);lv_obj_remove_flag(panel,LV_OBJ_FLAG_SCROLLABLE);
 if(lv_obj_get_child_count(panel)){auto *title=lv_obj_get_child(panel,0);if(lv_obj_has_class(title,&lv_label_class)){lv_obj_set_pos(title,32,30);lv_obj_set_width(title,600);lv_obj_set_style_text_font(title,&aurora_sans18,0);}}
 s->enable=button(s,"Reverb OFF",722,16,186,REVERB_ENABLE);s->enable_label=lv_obj_get_child(s->enable,0);
 const char *names[]={"Amount","Decay","High cut","Low cut"};
 const char *hints[]={"Dry to spacious","Short room to long tail","Upper reverb limit","Lower reverb limit"};
 const int low[]={0,5,1,0},high[]={60,40,200,199};
 for(unsigned i=0;i<4;++i){
  const int y=100+int(i)*81;text(panel,names[i],32,y,180);text(panel,hints[i],32,y+29,230,muted);
  s->values[i]=text(panel,"",764,y,144,lime);lv_obj_set_style_text_align(s->values[i],LV_TEXT_ALIGN_RIGHT,0);
  auto *slider=lv_slider_create(panel);s->sliders[i]=slider;lv_obj_set_pos(slider,290,y+33);lv_obj_set_size(slider,602,9);
  lv_slider_set_range(slider,low[i],high[i]);lv_obj_set_ext_click_area(slider,25);lv_obj_set_style_radius(slider,5,LV_PART_MAIN);
  lv_obj_set_style_bg_color(slider,lv_color_hex(0x363f38),LV_PART_MAIN);lv_obj_set_style_bg_color(slider,lv_color_hex(lime),LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider,lv_color_hex(white),LV_PART_KNOB);lv_obj_set_style_radius(slider,5,LV_PART_KNOB);
  lv_obj_set_style_pad_hor(slider,10,LV_PART_KNOB);lv_obj_set_style_pad_ver(slider,12,LV_PART_KNOB);
  lv_obj_add_event_cb(slider,changed,LV_EVENT_VALUE_CHANGED,bind(s,i));
 }
 button(s,"Reset",32,444,186,REVERB_RESET);button(s,"Close",722,444,186,REVERB_CLOSE);
 s->status=text(panel,"",240,461,458,muted);lv_label_set_long_mode(s->status,LV_LABEL_LONG_WRAP);lv_obj_set_height(s->status,58);
 reverb_dialog_refresh(panel,config,enabled,status);
}
