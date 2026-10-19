#pragma once
#include "lvgl.h"
#include "aurora_fonts.h"
#include "audio_eq.h"
#include "eq_presets.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

// Call only under the LVGL lock. The callback enqueues controller work; this
// view performs no audio, storage or device operations. Its state belongs to
// the supplied panel and is released when the panel is deleted.
enum EqDialogAction { EQ_BAND, EQ_PREAMP, EQ_ENABLE, EQ_RESET, EQ_SELECT_SLOT, EQ_LOAD, EQ_SAVE, EQ_CLOSE, EQ_HEADROOM, EQ_FACTORY };
using EqDialogCallback=void(*)(int kind,int index,int value);

namespace aurora_eq_detail {
constexpr uint32_t lime=0xdfee79,white=0xeeeFE8,muted=0xa8b1ad,dark=0x141615;
constexpr unsigned frequencies[11]={31,63,125,250,500,1000,2000,4000,8000,16000,20000};
constexpr const char *names[11]={"31 Hz","63 Hz","125 Hz","250 Hz","500 Hz","1 kHz","2 kHz","4 kHz","8 kHz","16 kHz","20 kHz"};
struct State;
struct Binding {State *owner{};int kind{},index{};};
struct State {
 lv_obj_t *panel{},*sliders[11]{},*values[11]{},*frequencies[11]{},*enable{},*enable_label{},*slots[4]{},*status{},*rate{},*limit{},*factory{};
 Binding bindings[24]{};unsigned binding_count=0;EqDialogCallback callback{};
 audio_eq_config_t config{};bool updating=false;
};
inline void destroy(lv_event_t *e){delete static_cast<State*>(lv_event_get_user_data(e));}
inline State *state(lv_obj_t *panel){
 if(!panel)return nullptr;
 for(uint32_t i=0;i<lv_obj_get_event_count(panel);i++){
  auto *d=lv_obj_get_event_dsc(panel,i);
  if(lv_event_dsc_get_cb(d)==destroy)return static_cast<State*>(lv_event_dsc_get_user_data(d));
 }
 return nullptr;
}
inline lv_obj_t *text(lv_obj_t *panel,const char *caption,int x,int y,int width,bool centered=false,uint32_t color=white){
 auto *o=lv_label_create(panel);lv_obj_set_pos(o,x,y);lv_obj_set_width(o,width);
 lv_obj_set_style_text_font(o,&aurora_sans18,0);lv_obj_set_style_text_color(o,lv_color_hex(color),0);
 lv_obj_set_style_text_align(o,centered?LV_TEXT_ALIGN_CENTER:LV_TEXT_ALIGN_LEFT,0);
 lv_label_set_long_mode(o,LV_LABEL_LONG_DOT);lv_label_set_text(o,caption);return o;
}
inline void label_text(lv_obj_t *o,const char *value){if(strcmp(lv_label_get_text(o),value))lv_label_set_text(o,value);}
inline void text_color(lv_obj_t *o,uint32_t value){if(!lv_color_eq(lv_obj_get_style_text_color(o,0),lv_color_hex(value)))lv_obj_set_style_text_color(o,lv_color_hex(value),0);}
inline void background(lv_obj_t *o,uint32_t value,lv_style_selector_t part=LV_PART_MAIN){if(!lv_color_eq(lv_obj_get_style_bg_color(o,part),lv_color_hex(value)))lv_obj_set_style_bg_color(o,lv_color_hex(value),part);}
inline void gain_text(lv_obj_t *o,int gain){char value[16];snprintf(value,sizeof value,gain>0?"+%d dB":"%d dB",gain);label_text(o,value);}
inline Binding *binding(State *s,int kind,int index){auto *b=&s->bindings[s->binding_count++];*b={s,kind,index};return b;}
inline void slider_event(lv_event_t *e){
 auto *b=static_cast<Binding*>(lv_event_get_user_data(e));auto *s=b->owner;if(s->updating)return;
 const int gain=lv_slider_get_value(lv_event_get_target_obj(e));
 s->config.gain_db[b->index]=gain;gain_text(s->values[b->index],gain);
 if(s->callback)s->callback(b->kind,b->index,gain);
}
inline void click_event(lv_event_t *e){
 auto *b=static_cast<Binding*>(lv_event_get_user_data(e));auto *s=b->owner;if(s->updating||!s->callback)return;
 const int value=b->kind==EQ_ENABLE?!s->config.enabled:0;
 s->callback(b->kind,b->index,value);
}
inline void factory_event(lv_event_t *e){
 auto *b=static_cast<Binding*>(lv_event_get_user_data(e));auto *s=b->owner;if(s->updating)return;
 const int selected=lv_dropdown_get_selected(s->factory);
 if(selected<=0){lv_dropdown_set_selected(s->factory,player::eq_factory_match(s->config)+1);return;}
 if(selected<=int(player::EQ_FACTORY_PRESET_COUNT)&&s->callback)s->callback(EQ_FACTORY,selected-1,0);
}
inline lv_obj_t *factory_selector(State *s){
 auto *o=lv_dropdown_create(s->panel);lv_obj_set_pos(o,780,12);lv_obj_set_size(o,232,48);lv_dropdown_set_dir(o,LV_DIR_BOTTOM);
 lv_dropdown_set_options(o,"Custom");
 for(unsigned i=0;i<player::EQ_FACTORY_PRESET_COUNT;i++)lv_dropdown_add_option(o,player::eq_factory_presets[i].name,LV_DROPDOWN_POS_LAST);
 lv_obj_set_style_radius(o,7,0);lv_obj_set_style_pad_hor(o,16,0);lv_obj_set_style_pad_ver(o,12,0);lv_obj_set_style_shadow_width(o,0,0);
 lv_obj_set_style_border_width(o,1,0);lv_obj_set_style_border_color(o,lv_color_hex(0x788177),0);lv_obj_set_style_bg_color(o,lv_color_hex(0x252d28),0);
 lv_obj_set_style_text_color(o,lv_color_hex(white),0);lv_obj_set_style_text_font(o,&aurora_sans18,0);
 // The content font contains only text; the dropdown arrow needs its symbol font.
 lv_obj_set_style_text_font(o,&lv_font_montserrat_18,LV_PART_INDICATOR);
 auto *list=lv_dropdown_get_list(o);lv_obj_set_style_max_height(list,448,0);lv_obj_set_style_pad_ver(list,14,0);lv_obj_set_style_pad_hor(list,16,0);
 lv_obj_set_style_text_font(list,&aurora_sans18,0);lv_obj_set_style_text_color(list,lv_color_hex(white),0);lv_obj_set_style_bg_color(list,lv_color_hex(0x1c241f),0);
 lv_obj_set_style_border_width(list,1,0);lv_obj_set_style_border_color(list,lv_color_hex(0x788177),0);lv_obj_set_style_shadow_width(list,0,0);
 const int line_space=std::max(0,44-int(lv_font_get_line_height(&aurora_sans18)));
 lv_obj_set_style_text_line_space(list,line_space,0);lv_obj_set_style_text_line_space(list,line_space,LV_PART_SELECTED);
 for(unsigned state:{0U,unsigned(LV_STATE_CHECKED),unsigned(LV_STATE_PRESSED),unsigned(LV_STATE_CHECKED|LV_STATE_PRESSED)}){
  const lv_style_selector_t selected=LV_PART_SELECTED|state;
  lv_obj_set_style_bg_color(list,lv_color_hex(lime),selected);lv_obj_set_style_bg_opa(list,LV_OPA_COVER,selected);lv_obj_set_style_text_color(list,lv_color_hex(dark),selected);
 }
 lv_obj_add_event_cb(o,factory_event,LV_EVENT_VALUE_CHANGED,binding(s,EQ_FACTORY,-1));return o;
}
inline lv_obj_t *button(State *s,const char *caption,int x,int y,int width,int kind,int index=-1){
 auto *o=lv_button_create(s->panel);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,width,48);
 lv_obj_set_style_radius(o,7,0);lv_obj_set_style_pad_all(o,0,0);lv_obj_set_style_shadow_width(o,0,0);
 lv_obj_set_style_border_width(o,1,0);lv_obj_set_style_border_color(o,lv_color_hex(0x788177),0);
 lv_obj_set_style_bg_color(o,lv_color_hex(0x252d28),0);lv_obj_set_style_bg_color(o,lv_color_hex(0x455138),LV_STATE_PRESSED);
 lv_obj_set_style_text_color(o,lv_color_hex(white),0);lv_obj_set_style_text_font(o,&aurora_sans18,0);
 auto *label=lv_label_create(o);lv_label_set_text(label,caption);lv_obj_center(label);
 lv_obj_add_event_cb(o,click_event,LV_EVENT_CLICKED,binding(s,kind,index));return o;
}
inline lv_obj_t *slider(State *s,int x,int y,int height,int low,int high,int kind,int index){
 auto *o=lv_slider_create(s->panel);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,8,height);
 lv_slider_set_range(o,low,high);if(low<0&&high>0)lv_slider_set_mode(o,LV_SLIDER_MODE_SYMMETRICAL);
 // A slim rail keeps the established hardware aesthetic; the 76 px touch
 // target is wider than a fingertip, and each target has its own 103 px lane.
 lv_obj_set_ext_click_area(o,34);lv_obj_set_style_radius(o,4,LV_PART_MAIN);
 lv_obj_set_style_bg_color(o,lv_color_hex(0x363f38),LV_PART_MAIN);
 lv_obj_set_style_bg_color(o,lv_color_hex(lime),LV_PART_INDICATOR);
 lv_obj_set_style_bg_color(o,lv_color_hex(white),LV_PART_KNOB);lv_obj_set_style_radius(o,4,LV_PART_KNOB);
 lv_obj_set_style_pad_hor(o,13,LV_PART_KNOB);lv_obj_set_style_pad_ver(o,5,LV_PART_KNOB);
 lv_obj_add_event_cb(o,slider_event,LV_EVENT_VALUE_CHANGED,binding(s,kind,index));return o;
}
inline void rule(lv_obj_t *panel,int x,int y,int width,int height,uint32_t color){
 auto *o=lv_obj_create(panel);lv_obj_remove_style_all(o);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,width,height);
 lv_obj_remove_flag(o,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE));
 lv_obj_set_style_bg_color(o,lv_color_hex(color),0);lv_obj_set_style_bg_opa(o,255,0);
}
inline void selected(lv_obj_t *o,bool on){
 background(o,on?lime:0x252d28);text_color(o,on?dark:white);
 const auto border=lv_color_hex(on?lime:0x788177);if(!lv_color_eq(lv_obj_get_style_border_color(o,0),border))lv_obj_set_style_border_color(o,border,0);
}
}

// Live emergency-limiter gain reduction, independent of slower configuration
// snapshots. Paused/stopped playback clears a stale reduction. EQ OFF may still
// have a real limiter release tail: show the measured reduction until it ends.
inline void eq_dialog_meter(lv_obj_t *panel,float reduction_db,bool active){
 using namespace aurora_eq_detail;auto *s=aurora_eq_detail::state(panel);if(!s||!s->limit)return;
 if(!active||!std::isfinite(reduction_db)||reduction_db<0)reduction_db=0;
 const int tenths=int(std::lround(std::clamp(reduction_db,0.f,60.f)*10.f));
 char value[28];snprintf(value,sizeof value,tenths?"LIMIT  -%d.%d dB":"LIMIT  %d.%d dB",tenths/10,tenths%10);
 label_text(s->limit,value);text_color(s->limit,tenths>=5?0xffb169:muted);
}

inline void eq_dialog_refresh(lv_obj_t *panel,const audio_eq_config_t &config,unsigned selected_slot,uint32_t sample_rate,const char *status){
 using namespace aurora_eq_detail;auto *s=aurora_eq_detail::state(panel);if(!s)return;
 s->updating=true;s->config=config;bool unavailable=false;
 for(unsigned i=0;i<11;i++){
  const bool available=!sample_rate||uint64_t(frequencies[i])*40<uint64_t(sample_rate)*19;
  unavailable|=!available;
  const int gain=std::clamp<int>(config.gain_db[i],-12,12);
  // Controller snapshots must not pull a held slider away from the finger.
  if(!lv_obj_has_state(s->sliders[i],LV_STATE_PRESSED)){lv_slider_set_value(s->sliders[i],gain,LV_ANIM_OFF);gain_text(s->values[i],gain);}
  text_color(s->frequencies[i],available?white:0x747d77);text_color(s->values[i],available?lime:muted);
  background(s->sliders[i],available&&config.enabled?lime:0x7d8870,LV_PART_INDICATOR);
 }
 selected(s->enable,config.enabled);label_text(s->enable_label,config.enabled?"EQ ON":"EQ OFF");
 const int factory=player::eq_factory_match(config)+1;if(lv_dropdown_get_selected(s->factory)!=unsigned(factory))lv_dropdown_set_selected(s->factory,factory);
 for(unsigned i=0;i<4;i++)selected(s->slots[i],i==std::min(selected_slot,3U));
 label_text(s->status,status&&*status?status:"Choose a factory curve or a User slot. Flat resets the band gains.");
 char rate[180];
 if(sample_rate)snprintf(rate,sizeof rate,"%.1f kHz%s",sample_rate/1000.,unavailable?" / dimmed bands bypassed":"");
 else rate[0]='\0';
 label_text(s->rate,rate);s->updating=false;
}

inline void eq_dialog_build(lv_obj_t *panel,const audio_eq_config_t &config,unsigned selected_slot,uint32_t sample_rate,const char *status,EqDialogCallback callback){
 using namespace aurora_eq_detail;if(!panel||aurora_eq_detail::state(panel))return;
 auto *s=new(std::nothrow) State;if(!s)return;s->panel=panel;s->callback=callback;
 lv_obj_add_event_cb(panel,destroy,LV_EVENT_DELETE,s);
 lv_obj_set_size(panel,1180,630);lv_obj_center(panel);lv_obj_set_style_pad_all(panel,0,0);
 lv_obj_remove_flag(panel,LV_OBJ_FLAG_SCROLLABLE);
 // The controller supplies the existing dialog title; only this new modal is
 // laid out here. No main-panel controls, artwork or assets are changed.
 if(lv_obj_get_child_count(panel)){auto *title=lv_obj_get_child(panel,0);if(lv_obj_has_class(title,&lv_label_class)){lv_obj_set_pos(title,24,24);lv_obj_set_width(title,740);lv_obj_set_style_text_font(title,&aurora_sans18,0);}}
 s->factory=factory_selector(s);
 s->enable=button(s,"EQ ON",1024,12,132,EQ_ENABLE);s->enable_label=lv_obj_get_child(s->enable,0);
 rule(panel,24,76,1132,1,0x485147);text(panel,"BAND GAIN  /  -12 TO +12 dB",24,82,670,false,muted);
 s->limit=text(panel,"LIMIT  0.0 dB",916,82,240,true,muted);
 for(int y:{146,268,390})rule(panel,38,y,1104,1,y==268?0x647052:0x2b342c);
 for(unsigned i=0;i<11;i++){
  const int x=24+int(i)*103;
  s->values[i]=text(panel,"0 dB",x,112,102,true,lime);
  s->sliders[i]=slider(s,x+47,146,244,-12,12,EQ_BAND,i);
  s->frequencies[i]=text(panel,names[i],x,412,102,true);
 }
 rule(panel,24,454,1132,1,0x485147);
 for(unsigned i=0;i<4;i++){char name[12];snprintf(name,sizeof name,"User %u",i+1);s->slots[i]=button(s,name,24+int(i)*136,474,124,EQ_SELECT_SLOT,i);}
 button(s,"Load",580,474,130,EQ_LOAD);button(s,"Save",722,474,130,EQ_SAVE);button(s,"Flat",864,474,130,EQ_RESET);button(s,"Close",1006,474,150,EQ_CLOSE);
 s->status=text(panel,"",24,544,1132,false,white);lv_label_set_long_mode(s->status,LV_LABEL_LONG_WRAP);lv_obj_set_height(s->status,43);
 s->rate=text(panel,"",24,594,1132,false,muted);
 eq_dialog_refresh(panel,config,selected_slot,sample_rate,status);
}
