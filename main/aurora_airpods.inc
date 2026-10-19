#pragma once
#include "lvgl.h"
#include "aurora_fonts.h"
#include <cstdio>
#include <cstring>
#include <new>

// The controller supplies actual Pico capability/channel reports and confirmed
// headphone values. This view only emits requests; it never predicts success.
enum AirPodsDialogAction { AIRPODS_SET_MODE, AIRPODS_SET_AUTOPAUSE, AIRPODS_CONNECT, AIRPODS_CLOSE };
enum AirPodsListeningMode { AIRPODS_MODE_UNKNOWN=-1, AIRPODS_MODE_OFF=1, AIRPODS_MODE_ANC=2,
                           AIRPODS_MODE_TRANSPARENCY=3, AIRPODS_MODE_ADAPTIVE=4 };
struct AirPodsDialogState {
 bool capable=false,headphones_connected=false,channel_connected=false,connecting=false,pending=false;
 int mode=AIRPODS_MODE_UNKNOWN,battery_left=-1,battery_right=-1,battery_case=-1,ears=-1;
 bool auto_pause=false,auto_pause_known=false;
 const char *status=nullptr; // copied into the label during build/refresh
};
using AirPodsDialogCallback=void(*)(int action,int value);

namespace aurora_airpods_detail {
constexpr uint32_t lime=0xdfee79,white=0xeeefe8,muted=0xa8b1ad,dark=0x141615;
struct State;
struct Binding {State *owner{};int action{},value{};};
struct State {
 lv_obj_t *panel{},*status{},*channel{},*batteries[3]{},*modes[4]{},*autopause{},*autopause_label{},*connect{},*connect_label{},*ears{},*help{};
 Binding bindings[7]{};unsigned bindings_used=0;AirPodsDialogCallback callback{};
 AirPodsDialogState current{};bool updating=false;
};
inline void destroy(lv_event_t *event){delete static_cast<State*>(lv_event_get_user_data(event));}
inline State *state(lv_obj_t *panel){
 if(!panel)return nullptr;
 for(uint32_t i=0;i<lv_obj_get_event_count(panel);++i){auto *descriptor=lv_obj_get_event_dsc(panel,i);if(lv_event_dsc_get_cb(descriptor)==destroy)return static_cast<State*>(lv_event_dsc_get_user_data(descriptor));}
 return nullptr;
}
inline void caption(lv_obj_t *object,const char *text){if(strcmp(lv_label_get_text(object),text))lv_label_set_text(object,text);}
inline lv_obj_t *text(lv_obj_t *parent,const char *value,int x,int y,int width,uint32_t ink=white,const lv_font_t *font=&aurora_sans18){
 auto *object=lv_label_create(parent);lv_obj_set_pos(object,x,y);lv_obj_set_width(object,width);
 lv_obj_set_style_text_color(object,lv_color_hex(ink),0);lv_obj_set_style_text_font(object,font,0);
 lv_label_set_long_mode(object,LV_LABEL_LONG_DOT);lv_label_set_text(object,value);return object;
}
inline void ink(lv_obj_t *object,uint32_t value){const auto color=lv_color_hex(value);if(!lv_color_eq(lv_obj_get_style_text_color(object,0),color))lv_obj_set_style_text_color(object,color,0);}
inline void background(lv_obj_t *object,uint32_t value,lv_style_selector_t selector=0){
 const auto color=lv_color_hex(value);lv_style_value_t previous{};
 if(lv_obj_get_local_style_prop(object,LV_STYLE_BG_COLOR,&previous,selector)!=LV_STYLE_RES_FOUND||!lv_color_eq(previous.color,color))lv_obj_set_style_bg_color(object,color,selector);
}
inline void active(lv_obj_t *object,bool selected){
 background(object,selected?lime:0x252d28);background(object,selected?lime:0x1c211d,LV_STATE_DISABLED);
 ink(lv_obj_get_child(object,0),selected?dark:white);
}
inline void enabled(lv_obj_t *object,bool value){
 if(value)lv_obj_remove_state(object,LV_STATE_DISABLED);else lv_obj_add_state(object,LV_STATE_DISABLED);
}
inline bool controls_available(const State *s){return s->current.capable&&s->current.headphones_connected&&s->current.channel_connected&&!s->current.pending&&!s->current.connecting;}
inline bool connection_available(const State *s){return s->current.capable&&s->current.headphones_connected&&!s->current.channel_connected&&!s->current.connecting&&!s->current.pending;}
inline void clicked(lv_event_t *event){
 auto *binding=static_cast<Binding*>(lv_event_get_user_data(event));auto *s=binding->owner;
 if(s->updating||!s->callback)return;
 if(binding->action!=AIRPODS_CLOSE){
  if(binding->action==AIRPODS_CONNECT?!connection_available(s):!controls_available(s))return;
  if(binding->action==AIRPODS_SET_AUTOPAUSE&&!s->current.auto_pause_known)return;
 }
 const int value=binding->action==AIRPODS_SET_AUTOPAUSE?!s->current.auto_pause:binding->value;
 s->callback(binding->action,value);
}
inline lv_obj_t *button(State *s,const char *name,int x,int y,int width,int height,int action,int value=0){
 auto *object=lv_button_create(s->panel);lv_obj_set_pos(object,x,y);lv_obj_set_size(object,width,height);
 lv_obj_set_style_radius(object,7,0);lv_obj_set_style_pad_all(object,0,0);lv_obj_set_style_shadow_width(object,0,0);
 lv_obj_set_style_border_width(object,1,0);lv_obj_set_style_border_color(object,lv_color_hex(0x788177),0);
 lv_obj_set_style_bg_color(object,lv_color_hex(0x252d28),0);lv_obj_set_style_bg_color(object,lv_color_hex(0x455138),LV_STATE_PRESSED);
 lv_obj_set_style_bg_color(object,lv_color_hex(0x1c211d),LV_STATE_DISABLED);
 lv_obj_set_style_text_font(object,&aurora_sans18,0);
 auto *label=lv_label_create(object);lv_label_set_text(label,name);lv_obj_set_style_text_color(label,lv_color_hex(white),0);lv_obj_center(label);
 auto *binding=&s->bindings[s->bindings_used++];*binding={s,action,value};lv_obj_add_event_cb(object,clicked,LV_EVENT_CLICKED,binding);return object;
}
inline const char *ear_caption(int ears){
 switch(ears){case 0:return "In-ear detection: neither ear";case 1:return "In-ear detection: one ear";case 2:return "In-ear detection: both ears";default:return "In-ear detection: --";}
}
}

inline void airpods_dialog_refresh(lv_obj_t *panel,const AirPodsDialogState &snapshot){
 using namespace aurora_airpods_detail;auto *s=state(panel);if(!s)return;s->updating=true;s->current=snapshot;s->current.status=nullptr;
 const bool connected=snapshot.capable&&snapshot.headphones_connected&&snapshot.channel_connected;
 const char *channel=!snapshot.capable?"Controls unavailable":!snapshot.headphones_connected?"Headphones disconnected":snapshot.connecting?"Connecting controls...":connected?"Controls connected":"Controls disconnected";
 caption(s->channel,channel);ink(s->channel,connected?lime:muted);
 const char *status=!snapshot.capable?"Update Pico to a firmware version with AirPods controls.":
  !snapshot.headphones_connected?"Connect your AirPods in Bluetooth settings, then return here.":
  snapshot.connecting?"Connecting to headphone controls. Music can keep playing.":
  snapshot.pending?"Waiting for confirmation from your headphones...":
  connected?"Choose a listening mode. The selected mode follows the headphones.":
  "AirPods controls are not connected. Tap Connect to try again.";
 if(snapshot.status&&*snapshot.status&&snapshot.capable)status=snapshot.status;
 caption(s->status,status);
 const int batteries[]={snapshot.battery_left,snapshot.battery_right,snapshot.battery_case};
 for(unsigned i=0;i<3;++i){char value[16];if(connected&&batteries[i]>=0&&batteries[i]<=100)snprintf(value,sizeof value,"%d%%",batteries[i]);else snprintf(value,sizeof value,"--");caption(s->batteries[i],value);}
 for(unsigned i=0;i<4;++i){active(s->modes[i],connected&&snapshot.mode==int(i)+1);enabled(s->modes[i],controls_available(s));}
 caption(s->autopause_label,snapshot.capable&&snapshot.auto_pause_known?(snapshot.auto_pause?"Auto pause: ON":"Auto pause: OFF"):"Auto pause: --");
 active(s->autopause,snapshot.capable&&snapshot.auto_pause_known&&snapshot.auto_pause);enabled(s->autopause,controls_available(s)&&snapshot.auto_pause_known);
 caption(s->ears,ear_caption(connected?snapshot.ears:-1));
 caption(s->connect_label,connected?"Connected":snapshot.connecting?"Connecting...":"Connect");enabled(s->connect,connection_available(s));
 caption(s->help,snapshot.capable?"Auto pause responds to removing or replacing a detected earbud.":"Music playback and standard headset buttons remain available.");
 s->updating=false;
}

inline void airpods_dialog_build(lv_obj_t *panel,const AirPodsDialogState &snapshot,AirPodsDialogCallback callback){
 using namespace aurora_airpods_detail;if(!panel||state(panel))return;auto *s=new(std::nothrow) State;if(!s)return;
 s->panel=panel;s->callback=callback;lv_obj_add_event_cb(panel,destroy,LV_EVENT_DELETE,s);
 lv_obj_set_size(panel,940,560);lv_obj_center(panel);lv_obj_set_style_pad_all(panel,0,0);lv_obj_remove_flag(panel,LV_OBJ_FLAG_SCROLLABLE);
 if(lv_obj_get_child_count(panel)){auto *title=lv_obj_get_child(panel,0);if(lv_obj_has_class(title,&lv_label_class)){lv_obj_set_pos(title,32,30);lv_obj_set_width(title,420);lv_obj_set_style_text_font(title,&aurora_sans20,0);}}
 s->channel=text(panel,"",530,31,378,muted);lv_obj_set_style_text_align(s->channel,LV_TEXT_ALIGN_RIGHT,0);
 s->status=text(panel,"",32,80,876,muted);lv_label_set_long_mode(s->status,LV_LABEL_LONG_WRAP);lv_obj_set_height(s->status,48);
 const char *names[]={"Left earbud","Right earbud","Case"};
 for(unsigned i=0;i<3;++i){
  auto *card=lv_obj_create(panel);lv_obj_remove_style_all(card);lv_obj_set_pos(card,32+int(i)*300,144);lv_obj_set_size(card,276,92);
  lv_obj_set_style_bg_color(card,lv_color_hex(0x1d2520),0);lv_obj_set_style_bg_opa(card,LV_OPA_COVER,0);lv_obj_set_style_radius(card,7,0);
  lv_obj_set_style_border_width(card,1,0);lv_obj_set_style_border_color(card,lv_color_hex(0x475344),0);lv_obj_remove_flag(card,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE));
  text(card,names[i],18,12,240,muted);s->batteries[i]=text(card,"--",18,43,240,white,&aurora_mono29);
 }
 text(panel,"Listening mode",32,262,876);
 const char *modes[]={"Off","Noise cancellation","Transparency","Adaptive"};
 for(unsigned i=0;i<4;++i)s->modes[i]=button(s,modes[i],32+int(i)*223,298,207,64,AIRPODS_SET_MODE,int(i)+1);
 s->autopause=button(s,"Auto pause: OFF",32,403,248,58,AIRPODS_SET_AUTOPAUSE);s->autopause_label=lv_obj_get_child(s->autopause,0);
 s->ears=text(panel,"",304,410,390,muted);s->help=text(panel,"",304,439,390,muted,&aurora_sans16);lv_label_set_long_mode(s->help,LV_LABEL_LONG_WRAP);lv_obj_set_height(s->help,42);
 s->connect=button(s,"Connect",722,403,186,58,AIRPODS_CONNECT);s->connect_label=lv_obj_get_child(s->connect,0);
 button(s,"Close",722,492,186,48,AIRPODS_CLOSE);
 text(panel,"Headphone features depend on the model and its firmware.",32,505,668,muted,&aurora_sans16);
 airpods_dialog_refresh(panel,snapshot);
}
