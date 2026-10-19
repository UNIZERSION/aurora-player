#include "player_ui.h"
#include "asset_pack.h"
#include "ui_fonts.h"
#include "aurora_fonts.h"
#include "aurora_geometry.h"
#include "aurora_fx_skin.hpp"
#include "particles_response.hpp"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <new>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif
extern "C" {
extern const uint8_t shell_start[] asm("_binary_ui_shell_pack_start");
extern const uint8_t shell_end[] asm("_binary_ui_shell_pack_end");
extern const uint8_t control_start[] asm("_binary_aurora_controls_pack_start");
extern const uint8_t control_end[] asm("_binary_aurora_controls_pack_end");
extern const uint8_t dragon_start[] asm("_binary_aurora_dragon_pack_start");
extern const uint8_t dragon_end[] asm("_binary_aurora_dragon_pack_end");
extern const uint8_t thumb_start[] asm("_binary_ui_dragon_thumb_rgb565_start");
}
namespace {
constexpr uint32_t BLACK=0x070909,WHITE=0xf0f1ef,MUTED=0xa4a6a8,LIME=0xe5f478,SILVER=0xd8d8d1;
constexpr int ROWS=8,CELL=72,BANDS=56,WAVE=128,ART_X=36,ART_Y=101,ART_W=307,ART_H=307;
enum Icon { NONE,PLAY,PREV,NEXT,SHUFFLE,REPEAT,HEART,MORE,BARS,WAVEFORM,DOTS,BASS,SPACE,TAPE,GEAR,BLUETOOTH,SPEAKER };
enum Skin { CLEAR,PILL,DISC,HARDWARE,SILVER_BUTTON };
struct Control { player_ui_action_t action;int value;Icon icon;Skin skin;int asset=-1;bool active=false;bool from_template=false,source_active=false;lv_image_dsc_t sprite{},variant{};uint16_t *pixels=nullptr,*source_pixels=nullptr; };
struct Track { std::string title,artist;uint32_t duration;uint64_t key;bool favorite; };
struct Row { lv_obj_t *box{},*image{},*number{},*title{},*artist{},*duration{},*menu{},*marker{};lv_draw_buf_t *cache{};size_t index=SIZE_MAX;bool selected=false,initialized=false,capturing=false,cached=false; };
struct Thumbnail { uint64_t key=0;uint32_t used=0,checked=0;bool missing=false;uint16_t *pixels{};lv_image_dsc_t image{}; };
struct FolderList;
lv_obj_t *create_embedded_folders();
struct Ui {
 lv_obj_t *root{},*title{},*artist{},*album{},*codec{},*format{},*format_info{},*counter{},*elapsed{},*duration{},*seek{},*firmware{};
 lv_obj_t *play{},*shuffle{},*repeat{},*favorite{},*mute{},*volume{},*volume_slider{},*clock{},*battery{},*bluetooth{};
 lv_obj_t *cover_box{},*cover{},*spectrum{},*playlist{},*spacer{},*tabs[3]{},*effects[4]{},*formats[5]{},*visuals[3]{};
 lv_obj_t *notice{},*notice_label{},*menu{};
 lv_obj_t *battery_percent{},*volume_percent{},*play_state{},*led{},*caption{},*volume_text{},*sample{},*bitrate{},*depth{},*bt_text{},*charge{},*battery_icon{},*list_label{},*list_count{},*queue_status{},*queue_duration{},*empty{};
 lv_obj_t *settings_pages[7]{},*settings_back{},*settings_heading{},*dim_buttons[4]{},*sort_buttons[3]{},*cover_toggle{},*power_level{},*power_details{},*jack_status{},*settings{},*settings_tab{},*dragon{},*dragon_types[2]{},*dragon_speed{},*dragon_colors{},*brightness{},*animations{},*clock_format{},*speed_value{},*brightness_value{},*track_speed{},*track_speed_value{};
 lv_obj_t *saver{};
 lv_obj_t *lock_panel{},*lock_list{},*lock_image{},*lock_title{},*lock_message{},*lock_dots[4]{},*lock_keys[12]{},*lock_cancel{},*lock_clock{},*lock_enabled_button{},*lock_change_button{},*lock_background_label{};
 const lv_image_dsc_t *lock_background{};bool lock_enabled=false,lock_can_cancel=false,lock_input_enabled=false;int lock_panel_kind=0;
 bool sleeping=false,settings_open=false,overlay=false,knob_drag=false,knob_moved=false;int knob_value=65,knob_origin=0,knob_y=0,knob_x=0;int settings_page=0;uint64_t playlist_ms=0;uint8_t *shell_cache{},*control_cache{},*dragon_cache{};uint32_t interaction_tick=0;lv_timer_t *dragon_timer{};uint32_t dragon_tick=0;double dragon_phase=0,visual_phase=0;int dragon_frame=-1,dragon_hue=-1;uint16_t *dragon_pixels{};lv_image_dsc_t dragon_image{};lv_timer_t *notice_timer{};
 player_ui_action_cb callback{};void *context{};player_ui_visual_source_cb visual_source{};void *visual_context{};player_ui_state_t state{};
 std::vector<Track> tracks;Row rows[ROWS]{};Thumbnail thumbnails[8]{};uint32_t thumb_age=0;
 float bands[BANDS]{},peaks[BANDS]{};aurora_particles::Response<BANDS> particles{};uint32_t particle_tick=0;int16_t wave[WAVE]{};int first=-1;bool updating=false,dragging=false;
 player_ui_render_stats_t stats{};uint32_t refresh_started{};bool refresh_drew=false;
 const lv_image_dsc_t *artwork_source{};uint64_t artwork_key=0;
 lv_obj_t *folders{};FolderList *folder_data{};size_t folder_count=0;std::string folder_label,lock_background_name;
 int keyboard_tab=-1;size_t keyboard_row=SIZE_MAX;lv_obj_t *keyboard_widget{};
} ui;
lv_draw_buf_t *visual_buffer=nullptr;
lv_image_dsc_t shell,dragon_thumb,control_images[24][4],knob_image;
uint16_t knob_pixels[111*112];int painted_knob=-1;

lv_color_t color(uint32_t v) { return lv_color_hex(v); }
void raw(lv_image_dsc_t &d,const uint8_t *p,int w,int h,int stride) { d={};d.header.magic=LV_IMAGE_HEADER_MAGIC;d.header.cf=LV_COLOR_FORMAT_RGB565;d.header.w=w;d.header.h=h;d.header.stride=stride;d.data=p;d.data_size=stride*h; }
uint64_t key(const char *p) { if(!p||!*p) { return 0; }uint64_t h=1469598103934665603ULL;for(;*p;++p) { h=(h^static_cast<unsigned char>(*p))*1099511628211ULL; }return h?h:1; }
void cache_thumbnail(uint64_t path_key,const lv_image_dsc_t *image);
void line(lv_layer_t *l,int x1,int y1,int x2,int y2,uint32_t c,int width=1,lv_opa_t opa=255) { lv_draw_line_dsc_t d;lv_draw_line_dsc_init(&d);d.p1={x1,y1};d.p2={x2,y2};d.color=color(c);d.width=width;d.opa=opa;d.round_start=d.round_end=1;lv_draw_line(l,&d); }
void rect(lv_layer_t *l,int x,int y,int w,int h,uint32_t c,int r=0,lv_opa_t opa=255) { if(w<=0||h<=0) { return; }lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);d.bg_color=color(c);d.bg_opa=opa;d.radius=r;lv_area_t a={x,y,x+w-1,y+h-1};lv_draw_rect(l,&d,&a); }
void circle(lv_layer_t *l,int x,int y,int r,uint32_t c,int width=1,int start=0,int end=360) { lv_draw_arc_dsc_t d;lv_draw_arc_dsc_init(&d);d.center={x,y};d.radius=r;d.width=width;d.color=color(c);d.start_angle=start;d.end_angle=end;lv_draw_arc(l,&d); }
void triangle(lv_layer_t *l,int x1,int y1,int x2,int y2,int x3,int y3,uint32_t c) { lv_draw_triangle_dsc_t d;lv_draw_triangle_dsc_init(&d);d.p[0]={x1,y1};d.p[1]={x2,y2};d.p[2]={x3,y3};d.color=color(c);lv_draw_triangle(l,&d); }
void keyboard_outline(lv_event_t *e) {
 auto *o=lv_event_get_target_obj(e);lv_area_t a;lv_obj_get_coords(o,&a);
 lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);d.bg_opa=0;d.border_color=color(LIME);d.border_width=2;d.radius=4;d.border_opa=220;
 a.x1+=1;a.y1+=1;a.x2-=1;a.y2-=1;lv_draw_rect(lv_event_get_layer(e),&d,&a);
}
void keyboard_widget(lv_obj_t *o) {
 if(ui.keyboard_widget==o)return;
 if(ui.keyboard_widget)lv_obj_invalidate(ui.keyboard_widget);
 ui.keyboard_widget=o;if(o)lv_obj_invalidate(o);
}
bool keyboard_clear() {
 bool had=ui.keyboard_tab>=0||ui.keyboard_widget;
 keyboard_widget(nullptr);
 if(ui.keyboard_tab>=0){ui.keyboard_tab=-1;ui.keyboard_row=SIZE_MAX;if(ui.playlist)lv_obj_invalidate(ui.playlist);if(ui.folders)lv_obj_invalidate(ui.folders);}
 return had;
}
void keyboard_touch(lv_event_t *) { keyboard_clear(); }
void keyboard_widget_event(lv_event_t *e) {
 if(lv_event_get_target_obj(e)!=ui.keyboard_widget)return;
 if(lv_event_get_code(e)==LV_EVENT_DELETE)ui.keyboard_widget=nullptr;
 else keyboard_outline(e);
}
void keyboard_bind(lv_obj_t *o) {
 lv_obj_add_event_cb(o,keyboard_touch,LV_EVENT_PRESSED,nullptr);
 lv_obj_add_event_cb(o,keyboard_widget_event,LV_EVENT_DRAW_POST,nullptr);
 lv_obj_add_event_cb(o,keyboard_widget_event,LV_EVENT_DELETE,nullptr);
}
void keyboard_row_draw(lv_event_t *e) {
 auto *r=static_cast<Row*>(lv_event_get_user_data(e));
 if(!r->capturing&&ui.keyboard_tab==ui.state.library_tab&&ui.keyboard_tab!=1&&ui.keyboard_row==r->index)keyboard_outline(e);
}
uint32_t rainbow(int i) { static const uint32_t stops[]={0xed57c6,0xf59363,0xe6dd56,0x8cd363,0x4cddd5,0x5a92fa,0xb96aef};float p=std::clamp(i,0,BANDS-1)*6.f/(BANDS-1);int n=std::min(5,int(p));float f=p-n;uint32_t out=0;for(int shift:{16,8,0})out|=uint32_t(((stops[n]>>shift)&255)*(1-f)+((stops[n+1]>>shift)&255)*f)<<shift;return out; }

lv_obj_t *plain(lv_obj_t *p,int x,int y,int w,int h) { auto *o=lv_obj_create(p);lv_obj_remove_style_all(o);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);lv_obj_remove_flag(o,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE));return o; }
void solid(lv_obj_t *o,uint32_t c,int r=0) { lv_obj_set_style_bg_color(o,color(c),0);lv_obj_set_style_bg_opa(o,255,0);lv_obj_set_style_radius(o,r,0); }
void delete_text(lv_event_t *e) { delete static_cast<std::string *>(lv_event_get_user_data(e)); }
lv_obj_t *label(lv_obj_t *p,const char *t,int x,int y,int w,const lv_font_t *f=&lv_font_montserrat_16,uint32_t c=WHITE) { f=ui_font_for(f);auto *o=lv_label_create(p);lv_obj_set_pos(o,x,y);lv_obj_set_height(o,lv_font_get_line_height(f));if(w>0) { lv_obj_set_width(o,w); }lv_obj_set_style_text_font(o,f,0);lv_obj_set_style_text_color(o,color(c),0);lv_label_set_long_mode(o,LV_LABEL_LONG_DOT);lv_label_set_text(o,t?t:"");auto *s=new std::string(t?t:"");lv_obj_set_user_data(o,s);lv_obj_add_event_cb(o,delete_text,LV_EVENT_DELETE,s);return o; }
void text(lv_obj_t *o,const char *t) { if(!o) { return; }if(!t) { t=""; }auto *s=static_cast<std::string *>(lv_obj_get_user_data(o));if(s&&*s==t) { return; }if(s) { *s=t; }lv_label_set_text(o,t); }
void ink(lv_obj_t *o,uint32_t c) { if(!lv_color_eq(lv_obj_get_style_text_color(o,0),color(c))) { lv_obj_set_style_text_color(o,color(c),0); } }
void emit(player_ui_action_t a,int v=0) { if(ui.lock_panel&&a!=PLAYER_UI_LOCK_INPUT&&a!=PLAYER_UI_LOCK_BACKGROUND_SELECT)return;if(ui.callback&&!ui.updating) { ui.callback(a,v,ui.context); } }
void time_text(char *p,size_t n,uint32_t ms) { unsigned sec=ms/1000;snprintf(p,n,"%02u:%02u",sec/60,sec%60); }
void close_menu() { if(ui.menu) { lv_obj_delete(ui.menu);ui.menu=nullptr; } }
void show_menu(size_t index,bool row);
void icon(lv_layer_t *l,Icon id,int cx,int cy,uint32_t c,int size=20) {
 const int q=std::max(5,size/2);
 switch(id) {
 case PLAY:if(ui.state.playing) { rect(l,cx-q,cy-q-2,8,2*q+4,c,4);rect(l,cx+q-8,cy-q-2,8,2*q+4,c,4); }else { triangle(l,cx-q+2,cy-q-3,cx+q+3,cy,cx-q+2,cy+q+3,c); }break;
 case PREV:case NEXT:{int s=id==NEXT?1:-1;triangle(l,cx-s*q,cy-q,cx+s*(q-3),cy,cx-s*q,cy+q,c);line(l,cx+s*q,cy-q,cx+s*q,cy+q,c,4);break;}
 case SHUFFLE:line(l,cx-12,cy-8,cx-6,cy-8,c,2);line(l,cx-6,cy-8,cx+7,cy+8,c,2);line(l,cx+7,cy+8,cx+13,cy+8,c,2);line(l,cx-12,cy+8,cx-6,cy+8,c,2);line(l,cx-6,cy+8,cx+7,cy-8,c,2);line(l,cx+7,cy-8,cx+13,cy-8,c,2);line(l,cx+9,cy-12,cx+13,cy-8,c,2);line(l,cx+13,cy-8,cx+9,cy-4,c,2);line(l,cx+9,cy+4,cx+13,cy+8,c,2);line(l,cx+13,cy+8,cx+9,cy+12,c,2);break;
 case REPEAT:line(l,cx-11,cy+7,cx-11,cy-5,c,2);line(l,cx-11,cy-5,cx-7,cy-9,c,2);line(l,cx-7,cy-9,cx+12,cy-9,c,2);line(l,cx+12,cy-9,cx+8,cy-13,c,2);line(l,cx+12,cy-9,cx+8,cy-5,c,2);line(l,cx+11,cy-7,cx+11,cy+5,c,2);line(l,cx+11,cy+5,cx+7,cy+9,c,2);line(l,cx+7,cy+9,cx-12,cy+9,c,2);line(l,cx-12,cy+9,cx-8,cy+13,c,2);line(l,cx-12,cy+9,cx-8,cy+5,c,2);if(ui.state.repeat==1) { line(l,cx,cy-4,cx,cy+4,c,2); }break;
 case HEART:{const int p[][2]={{0,11},{-10,1},{-11,-5},{-8,-9},{-3,-9},{0,-5},{3,-9},{8,-9},{11,-5},{10,1},{0,11}};for(unsigned i=1;i<sizeof p/sizeof p[0];++i) { line(l,cx+p[i-1][0],cy+p[i-1][1],cx+p[i][0],cy+p[i][1],c,2); }break;}
 case MORE:for(int i=-1;i<=1;++i) { rect(l,cx+i*7-1,cy-1,3,3,c,2); }break;
 case BARS:case BASS:for(int i=0;i<5;++i) { int h=id==BASS?5+i*4:7+(2-abs(i-2))*6;rect(l,cx-13+i*6,cy+10-h,3,h,c,1); }break;
 case WAVEFORM:for(int i=0;i<25;++i) { line(l,cx-13+i,cy+int(sinf(i*.25f)*5),cx-12+i,cy+int(sinf((i+1)*.25f)*5),c,2); }break;
 case DOTS:for(int y=-1;y<=1;++y) { for(int x=-1;x<=1;++x) { rect(l,cx+x*8-1,cy+y*8-1,3,3,c,2); } }break;
 case SPACE:circle(l,cx,cy,8,c);line(l,cx-15,cy+7,cx+15,cy-7,c,2);line(l,cx-15,cy+7,cx-10,cy+10,c);line(l,cx+15,cy-7,cx+10,cy-10,c);break;
 case TAPE:rect(l,cx-13,cy-9,26,18,c,2);rect(l,cx-11,cy-7,22,14,0x111314,1);circle(l,cx-6,cy-2,3,c,2);circle(l,cx+6,cy-2,3,c,2);line(l,cx-7,cy+6,cx+7,cy+6,c,2);break;
 case GEAR:circle(l,cx,cy,8,c,3);circle(l,cx,cy,3,c);for(int i=0;i<8;++i) { float a=i*.785398f;line(l,cx+int(cosf(a)*8),cy+int(sinf(a)*8),cx+int(cosf(a)*12),cy+int(sinf(a)*12),c,3); }break;
 case BLUETOOTH:
  // Reference badge: a dark standard rune on a vertical rounded capsule.
  // Its 26x35 silhouette shares the battery/clock center and changes color.
  rect(l,cx-13,cy-17,26,35,c,13);
  line(l,cx-1,cy-13,cx-1,cy+13,0x090c0b,3);
  line(l,cx-1,cy-13,cx+7,cy-7,0x090c0b,3);
  line(l,cx+7,cy-7,cx-7,cy+7,0x090c0b,3);
  line(l,cx-7,cy-7,cx+7,cy+7,0x090c0b,3);
  line(l,cx+7,cy+7,cx-1,cy+13,0x090c0b,3);
  break;
 case SPEAKER:rect(l,cx-12,cy-5,6,10,c,1);triangle(l,cx-8,cy-5,cx,cy-11,cx,cy+11,c);if(ui.state.muted) { line(l,cx+4,cy-5,cx+12,cy+5,c,2);line(l,cx+4,cy+5,cx+12,cy-5,c,2); }else { circle(l,cx-3,cy,12,c,2,315,45);circle(l,cx-3,cy,17,c,1,315,45); }break;
 default:break;
 }
}
// Template sprites share the immutable RGB565 shell. Only changed state pixels
// receive a small PSRAM variant; touch objects add no replacement button skin.
void *pixel_alloc(size_t bytes) {
#ifdef ESP_PLATFORM
 return heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
 return lv_malloc(bytes);
#endif
}
void pixel_free(void *p) {
#ifdef ESP_PLATFORM
 heap_caps_free(p);
#else
 lv_free(p);
#endif
}
void draw_sprite(lv_layer_t *l,const lv_area_t &a,const lv_image_dsc_t *sprite) { lv_draw_image_dsc_t d;lv_draw_image_dsc_init(&d);d.src=sprite;lv_draw_image(l,&d,&a); }
void asset_draw(lv_layer_t *l,const lv_area_t &area,int id,bool on,bool pressed=false) {
 draw_sprite(l,area,&control_images[id][(on?1:0)+(pressed?2:0)]);
}
void control_draw(lv_event_t *e) {
 auto *o=lv_event_get_target_obj(e);auto *c=static_cast<Control *>(lv_obj_get_user_data(o));lv_area_t a;lv_obj_get_coords(o,&a);auto *l=lv_event_get_layer(e);int w=lv_area_get_width(&a),h=lv_area_get_height(&a),cx=(a.x1+a.x2)/2,cy=(a.y1+a.y2)/2;bool pressed=lv_obj_has_state(o,LV_STATE_PRESSED);if(c->asset>=0) { bool on=c->action==PLAYER_UI_PLAY_PAUSE?ui.state.playing:c->action==PLAYER_UI_MUTE?ui.state.muted:c->active;asset_draw(l,a,c->asset,on,pressed);if(c->asset==14||c->asset==15){
   const uint32_t ec=aurora_fx_skin::foreground(on,pressed);const int icon_y=a.y1+26+(pressed?1:0);
   if(c->asset==15){for(int i=0;i<3;i++){int x=cx+(i-1)*9;line(l,x,icon_y-12,x,icon_y+12,ec,2);rect(l,x-3,icon_y-9+i*6,7,4,ec,1);}}
   else { // Short room reflections, symmetric about the sound source.
    rect(l,cx-1,icon_y-6,3,12,ec,1);
    for(int r:{8,14}){circle(l,cx,icon_y,r,ec,2,305,55);circle(l,cx,icon_y,r,ec,2,125,235);}
   }
   lv_draw_label_dsc_t caption;lv_draw_label_dsc_init(&caption);caption.font=&aurora_fx_skin::label_font;caption.color=color(ec);caption.align=LV_TEXT_ALIGN_CENTER;caption.text=c->asset==14?"REVERB":"EQ";
   lv_area_t caption_area={a.x1,a.y1+46+(pressed?1:0),a.x2,a.y1+59+(pressed?1:0)};lv_draw_label(l,&caption,&caption_area);
  }if(c->action==PLAYER_UI_REPEAT&&ui.state.repeat==1) { line(l,cx,cy-4,cx,cy+5,LIME,2); }return; }uint32_t fg=c->action==PLAYER_UI_SETTINGS?0x303330:c->active?LIME:WHITE;
 if(c->skin==PILL) { rect(l,a.x1,a.y1,w,h,c->active?0xc4d166:0x68725c,9);rect(l,a.x1+1,a.y1+1,w-2,h-2,c->active?0x3b4426:pressed?0x343638:0x20271f,8); }
 else if(c->skin==DISC) { rect(l,a.x1,a.y1,w,h,0xd4d6d1,LV_RADIUS_CIRCLE);rect(l,a.x1+2,a.y1+2,w-4,h-4,0x090b0b,LV_RADIUS_CIRCLE);rect(l,a.x1+5,a.y1+5,w-10,h-10,pressed?0x383d2d:0x202324,LV_RADIUS_CIRCLE);circle(l,cx,cy,w/2-5,0x6a6d6a);circle(l,cx,cy,w/2-8,0x0d1010,2);int r=w/2-9;for(int y=-r+2;y<r-2;y+=4) { int dx=int(sqrtf(float(r*r-y*y)));line(l,cx-dx+2,cy+y,cx+dx-2,cy+y,0x303334,1,80); }if(c->icon==PLAY) { fg=LIME; } }
 else if(c->skin==HARDWARE) { rect(l,a.x1,a.y1,w,h,0x050606,11);rect(l,a.x1+1,a.y1+1,w-2,h-2,c->active?LIME:0x686b66,10);rect(l,a.x1+3,a.y1+3,w-6,h-6,c->active&&c->action==PLAYER_UI_FORMAT_FILTER?LIME:pressed?0x383e2c:0x111415,8);line(l,a.x1+10,a.y1+5,a.x2-10,a.y1+5,c->active?0x9da64c:0x35393a); }
 else if(c->skin==SILVER_BUTTON) { rect(l,a.x1,a.y1,w,h,SILVER,8);fg=ui.state.muted?0x747774:0x303330; }
 icon(l,c->icon,cx,c->skin==HARDWARE&&lv_obj_get_child_count(o)?cy-9:cy,fg,c->icon==PLAY?42:22);
}
void battery_touch(lv_event_t *e){
 static uint32_t down=0;static bool held=false,sent=false;const auto code=lv_event_get_code(e);
 if(code==LV_EVENT_PRESSED){keyboard_clear();down=lv_tick_get();held=true;sent=false;}
 else if(code==LV_EVENT_PRESSING&&held&&!sent&&lv_tick_elaps(down)>=900){sent=true;emit(PLAYER_UI_SCREENSHOT);}
 else if(code==LV_EVENT_RELEASED||code==LV_EVENT_PRESS_LOST)held=false;
}
void press_feedback(lv_event_t *e){lv_obj_invalidate(lv_event_get_target_obj(e));}
void delete_control(lv_event_t *e) { auto *c=static_cast<Control *>(lv_event_get_user_data(e));if(c->pixels) { lv_image_cache_drop(&c->variant);pixel_free(c->pixels); }if(c->source_pixels) { lv_image_cache_drop(&c->sprite);pixel_free(c->source_pixels); }delete c; }
void settings_visible(bool on);
void settings_page(int page);
void control_click(lv_event_t *e) { auto *c=static_cast<Control *>(lv_obj_get_user_data(lv_event_get_target_obj(e)));if(ui.lock_panel&&c->action!=PLAYER_UI_LOCK_INPUT&&c->action!=PLAYER_UI_LOCK_BACKGROUND_SELECT)return;if(c->action==PLAYER_UI_SETTINGS) { if(c->value<0){if(!ui.settings_open)settings_page(0);settings_visible(!ui.settings_open);}else {settings_page(c->value);settings_visible(true);}return; }if(c->action==PLAYER_UI_LIBRARY_TAB) { settings_visible(false); }if(c->action==PLAYER_UI_VIEW) { show_menu(SIZE_MAX,false);return; }int v=c->value;if(c->action==PLAYER_UI_SHUFFLE) { v=!ui.state.shuffle; }else if(c->action==PLAYER_UI_REPEAT) { v=ui.state.repeat==0?2:ui.state.repeat==2?1:0; }else if(c->action==PLAYER_UI_MUTE) { v=!ui.state.muted; }else if(c->action==PLAYER_UI_FORMAT_FILTER&&ui.state.format_filter==v) { v=0; }emit(c->action,v); }
lv_obj_t *button(lv_obj_t *p,int x,int y,int w,int h,player_ui_action_t act,Icon symbol=NONE,Skin skin=CLEAR,const char *t=nullptr,int v=0) { auto *o=plain(p,x,y,w,h);lv_obj_add_flag(o,LV_OBJ_FLAG_CLICKABLE);keyboard_bind(o);auto *c=new Control{act,v,symbol,skin};lv_obj_set_user_data(o,c);lv_obj_add_event_cb(o,control_draw,LV_EVENT_DRAW_MAIN,nullptr);lv_obj_add_event_cb(o,press_feedback,LV_EVENT_PRESSED,nullptr);lv_obj_add_event_cb(o,press_feedback,LV_EVENT_RELEASED,nullptr);lv_obj_add_event_cb(o,press_feedback,LV_EVENT_PRESS_LOST,nullptr);lv_obj_add_event_cb(o,control_click,LV_EVENT_CLICKED,nullptr);lv_obj_add_event_cb(o,delete_control,LV_EVENT_DELETE,c);if(t) { auto *s=label(o,t,0,0,w,&lv_font_montserrat_14);lv_obj_set_style_text_align(s,LV_TEXT_ALIGN_CENTER,0);if(symbol!=NONE) { lv_obj_set_pos(s,2,h-22);lv_obj_set_width(s,w-4); }else { lv_obj_center(s); }if(skin==HARDWARE&&symbol!=NONE) { lv_obj_set_style_text_letter_space(s,-1,0); } }return o; }
lv_obj_t *asset_button(int id,player_ui_action_t action,int value=0) {
 const auto &r=aurora_controls[id];auto *o=button(ui.root,r.x,r.y,r.w,r.h,action,NONE,CLEAR,nullptr,value);
 static_cast<Control *>(lv_obj_get_user_data(o))->asset=id;return o;
}
void settings_visible(bool on);
void settings_page(int page);
void active(lv_obj_t *o,bool on) { auto *c=static_cast<Control *>(lv_obj_get_user_data(o));if(c->active==on) { return; }c->active=on;for(uint32_t i=0;i<lv_obj_get_child_count(o);++i) { ink(lv_obj_get_child(o,i),on?LIME:WHITE); }lv_obj_invalidate(o); }
void menu_dismiss(lv_event_t *) { close_menu(); }
void menu_item(lv_event_t *e) { auto *c=static_cast<Control *>(lv_event_get_user_data(e));auto a=c->action;int v=c->value;close_menu();emit(a,v); }
void show_menu(size_t index,bool row) {
 close_menu();ui.menu=plain(ui.root,0,0,1280,720);lv_obj_add_flag(ui.menu,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_event_cb(ui.menu,menu_dismiss,LV_EVENT_CLICKED,nullptr);auto *p=plain(ui.menu,935,160,282,row?314:266);solid(p,0x151818,12);lv_obj_add_flag(p,LV_OBJ_FLAG_CLICKABLE);lv_obj_set_style_border_color(p,color(0x72776d),0);lv_obj_set_style_border_width(p,1,0);
 const char *names[]={row&&index<ui.tracks.size()&&ui.tracks[index].favorite?"Remove favorite":"Add favorite","File information","Choose folder","Search music","Sort playlist","Settings"};const player_ui_action_t acts[]={PLAYER_UI_FAVORITE,PLAYER_UI_FILE_INFO,PLAYER_UI_FOLDER,PLAYER_UI_SEARCH,PLAYER_UI_SORT,PLAYER_UI_ADVANCED};
 for(int i=row?0:1,at=0;i<6;++i,++at) { auto *o=button(p,9,9+at*49,264,44,acts[i],NONE,PILL,names[i],i<2&&row?int(index):-1);lv_obj_remove_event_cb(o,control_click);lv_obj_add_event_cb(o,menu_item,LV_EVENT_CLICKED,lv_obj_get_user_data(o)); }
}
void row_click(lv_event_t *e) { auto *r=static_cast<Row *>(lv_event_get_user_data(e));if(r->index<ui.tracks.size()) { emit(PLAYER_UI_SELECT_TRACK,int(r->index)); } }
void row_menu(lv_event_t *e) { auto *r=static_cast<Row *>(lv_event_get_user_data(e));if(r->index<ui.tracks.size()) { show_menu(r->index,true); } }
const lv_image_dsc_t *thumbnail(uint64_t k) { if(ui.state.covers&&k) { for(auto &t:ui.thumbnails) { if(t.key==k&&!t.missing) { t.used=++ui.thumb_age;return &t.image; } } }return &dragon_thumb; }
std::string subtitle(const player_ui_track_t &t) {
 std::string value=t.artist&&*t.artist?t.artist:"Unknown artist";
 if(t.path){std::string path=t.path;auto dot=path.find_last_of('.');if(dot!=std::string::npos){std::string ext=path.substr(dot+1);if(ext.size()<6){for(auto &c:ext)c=char(toupper(static_cast<unsigned char>(c)));value+=" · "+ext;}}}return value;
}
// A queue contains only eight recycled rows. Rasterize their static content
// once, rather than blending every glyph and cover again on each scroll frame.
// The selected row's playing indicator and keyboard focus remain live children.
void row_raster_draw(lv_event_t *e) {
 auto *r=static_cast<Row*>(lv_event_get_user_data(e));lv_area_t a;lv_obj_get_coords(r->box,&a);auto *layer=lv_event_get_layer(e);
 if(r->capturing){
  // Snapshot RGB565 starts at zero. The original rounded row is composited on
  // the playlist's BLACK, so preserve that exact backdrop at its corner pixels.
  rect(layer,a.x1,a.y1,lv_area_get_width(&a),lv_area_get_height(&a),BLACK);
 }else if(r->cached){
  lv_draw_image_dsc_t d;lv_draw_image_dsc_init(&d);d.src=r->cache;lv_draw_image(layer,&d,&a);lv_event_stop_processing(e);
 }
}
void cache_row(Row &r) {
 r.cached=false;
 for(auto *o:{r.image,r.title,r.artist,r.duration})lv_obj_remove_flag(o,LV_OBJ_FLAG_HIDDEN);
 if(r.selected)lv_obj_remove_flag(r.marker,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(r.marker,LV_OBJ_FLAG_HIDDEN);
 // 358 RGB565 pixels have an aligned 716-byte stride. Each row is 51,552 B;
 // all eight caches together are bounded at 412,416 B, independent of library.
 if(!r.cache)r.cache=lv_draw_buf_create(358,CELL,LV_COLOR_FORMAT_RGB565,716);
 if(!r.cache){lv_obj_invalidate(r.box);return;}
 lv_image_cache_drop(r.cache);lv_obj_add_flag(r.menu,LV_OBJ_FLAG_HIDDEN);r.capturing=true;
 const auto result=lv_snapshot_take_to_draw_buf(r.box,LV_COLOR_FORMAT_RGB565,r.cache);
 r.capturing=false;lv_obj_remove_flag(r.menu,LV_OBJ_FLAG_HIDDEN);
 if(result==LV_RESULT_OK){
  lv_draw_buf_flush_cache(r.cache,nullptr);r.cached=true;
  for(auto *o:{r.image,r.title,r.artist,r.duration,r.marker})lv_obj_add_flag(o,LV_OBJ_FLAG_HIDDEN);
 }
 // On allocation/snapshot failure the original live row remains fully usable.
 lv_obj_invalidate(r.box);
}
void refresh_rows(bool force=false) {
 if(!ui.playlist) { return; }int first=std::max(0,int(lv_obj_get_scroll_y(ui.playlist))/CELL);if(first==ui.first&&!force) { return; }ui.first=first;
 for(int i=0;i<ROWS;++i) { size_t index=size_t(first+i);auto &r=ui.rows[index%ROWS];if(index>=ui.tracks.size()) { if(!lv_obj_has_flag(r.box,LV_OBJ_FLAG_HIDDEN)) { lv_obj_add_flag(r.box,LV_OBJ_FLAG_HIDDEN); }r.initialized=false;continue; }
 const auto &t=ui.tracks[index];bool selected=index==ui.state.track_index;bool geometry=!r.initialized||r.index!=index||r.selected!=selected;if(!force&&r.initialized&&r.index==index&&r.selected==selected)continue;r.index=index;
 if(lv_obj_has_flag(r.box,LV_OBJ_FLAG_HIDDEN)) { lv_obj_remove_flag(r.box,LV_OBJ_FLAG_HIDDEN); }if(geometry) { lv_obj_set_y(r.box,int(index)*CELL); }
 if(!r.initialized||r.selected!=selected) { lv_obj_set_style_bg_color(r.box,color(selected?0x202423:BLACK),0);if(selected) { lv_obj_remove_flag(r.marker,LV_OBJ_FLAG_HIDDEN); }else { lv_obj_add_flag(r.marker,LV_OBJ_FLAG_HIDDEN); } }
 auto *image=thumbnail(t.key);bool changed=geometry||lv_image_get_src(r.image)!=image;if(lv_image_get_src(r.image)!=image) { lv_image_set_src(r.image,image); }
 char d[20];time_text(d,sizeof d,t.duration);const char *artist=t.artist.empty()?"Unknown artist":t.artist.c_str(),*duration=t.duration?d:"--:--";
 // LONG_DOT can temporarily replace the label's text bytes with an ellipsis.
 // Compare our immutable source copy, not LVGL's rendered text buffer.
 auto same=[](lv_obj_t *o,const char *value){auto *stored=static_cast<std::string*>(lv_obj_get_user_data(o));return stored&&*stored==value;};
 changed|=!same(r.title,t.title.c_str())||!same(r.artist,artist)||!same(r.duration,duration);
 text(r.title,t.title.c_str());text(r.artist,artist);text(r.duration,duration);
 if(geometry) { lv_obj_set_width(r.title,selected?196:215);lv_obj_set_width(r.artist,selected?196:215);lv_obj_set_x(r.duration,selected?267:290); }
 r.selected=selected;r.initialized=true;if(changed||!r.cached)cache_row(r);
 }
}
// A wide transparent touch target drives the template's thin visual scrollbar.
// It floats above list content, so it remains at the edge during scrolling.
struct ScrollHandle {lv_obj_t *list{};int y{},start{},range{},travel{};bool dragging=false;};
void scroll_handle_event(lv_event_t *e){auto *h=static_cast<ScrollHandle*>(lv_event_get_user_data(e));auto code=lv_event_get_code(e);if(code==LV_EVENT_DELETE){delete h;return;}if(code!=LV_EVENT_PRESSED&&code!=LV_EVENT_PRESSING&&code!=LV_EVENT_RELEASED&&code!=LV_EVENT_PRESS_LOST)return;auto *in=lv_indev_active();if(!in)return;lv_point_t p;lv_indev_get_point(in,&p);
 if(code==LV_EVENT_PRESSED){keyboard_clear();h->start=lv_obj_get_scroll_y(h->list);h->range=h->start+lv_obj_get_scroll_bottom(h->list);if(h->range<=0)return;lv_area_t hor,ver,area;lv_obj_get_scrollbar_area(h->list,&hor,&ver);lv_obj_get_coords(h->list,&area);h->travel=std::max<int>(1,lv_obj_get_height(h->list)-lv_area_get_height(&ver));h->y=p.y;h->dragging=true;lv_obj_set_scroll_dir(h->list,LV_DIR_NONE);if(p.y<ver.y1||p.y>ver.y2){h->start=std::clamp(int(int64_t(p.y-area.y1-lv_area_get_height(&ver)/2)*h->range/h->travel),0,h->range);lv_obj_scroll_to_y(h->list,h->start,LV_ANIM_OFF);}}
 else if(code==LV_EVENT_PRESSING&&h->dragging){int target=std::clamp(h->start+int(int64_t(p.y-h->y)*h->range/h->travel),0,h->range);lv_obj_scroll_to_y(h->list,target,LV_ANIM_OFF);}
 else if(code==LV_EVENT_RELEASED||code==LV_EVENT_PRESS_LOST){h->dragging=false;lv_obj_set_scroll_dir(h->list,LV_DIR_VER);}
}
void add_scroll_handle(lv_obj_t *list,int width,int height){auto *o=plain(list,width-24,0,24,height);lv_obj_add_flag(o,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_FLOATING));lv_obj_set_ext_click_area(o,8);auto *h=new ScrollHandle;h->list=list;lv_obj_add_event_cb(o,scroll_handle_event,LV_EVENT_ALL,h);}
void list_scroll(lv_event_t *) { ui.interaction_tick=lv_tick_get();refresh_rows(); }
void list_geometry(bool reveal) { lv_obj_set_height(ui.spacer,std::max(1,int(ui.tracks.size())*CELL));ui.first=-1;if(reveal) { int row=ui.state.track_index<ui.tracks.size()?int(ui.state.track_index):0;lv_obj_scroll_to_y(ui.playlist,row>=6?(row-2)*CELL:0,LV_ANIM_OFF); }refresh_rows(true); }
void seek_event(lv_event_t *e) { auto c=lv_event_get_code(e);if(c==LV_EVENT_VALUE_CHANGED)lv_obj_invalidate(ui.seek);if(c==LV_EVENT_PRESSED) { ui.dragging=true; }else if(c==LV_EVENT_RELEASED||c==LV_EVENT_PRESS_LOST) { ui.dragging=false;emit(PLAYER_UI_SEEK_MS,int(uint64_t(lv_slider_get_value(ui.seek))*ui.state.duration_ms/1000)); }if(c==LV_EVENT_VALUE_CHANGED&&ui.dragging) { char t[24];time_text(t,sizeof t,uint32_t(uint64_t(lv_slider_get_value(ui.seek))*ui.state.duration_ms/1000));text(ui.elapsed,t); } }

void seek_rainbow(lv_event_t *e) {
 auto *o=lv_event_get_target_obj(e);lv_area_t a;lv_obj_get_coords(o,&a);auto *l=lv_event_get_layer(e);
 int width=lv_area_get_width(&a)-24,fill=width*lv_slider_get_value(o)/1000,x0=a.x1+12,y=a.y1+22;
 rect(l,x0,y,width,9,0x717774,5);rect(l,x0+1,y+1,width-2,7,0x25292a,4);
 for(int x=1;x<fill;x+=3){rect(l,x0+x,y+1,std::min(3,fill-x),7,rainbow(x*BANDS/std::max(1,fill)));}
 rect(l,x0+fill-11,y-7,23,23,WHITE,LV_RADIUS_CIRCLE);
}
void volume_event(lv_event_t *e) {
 auto *o=lv_event_get_target_obj(e);
 // LVGL invalidates only the native arc ring. Our pointer crosses its center,
 // so a touch change must redraw the complete custom-painted knob immediately.
 if(o==ui.volume) { lv_obj_invalidate(ui.volume); }
 int value=o==ui.volume?ui.knob_value:lv_slider_get_value(o);
 if(ui.knob_value!=value){ui.knob_value=value;lv_obj_invalidate(ui.volume);}ui.interaction_tick=lv_tick_get();
 char b[8];snprintf(b,sizeof b,"%d",value);text(ui.volume_text,b);text(ui.volume_percent,"%");
 emit(PLAYER_UI_VOLUME,value);
}
// Same interaction as player.js: vertical drag, or tap to set the angle.
void knob_touch(lv_event_t *e) {
 const auto code=lv_event_get_code(e);if(code!=LV_EVENT_PRESSED&&code!=LV_EVENT_PRESSING&&code!=LV_EVENT_RELEASED&&code!=LV_EVENT_PRESS_LOST)return;
 auto *in=lv_indev_active();if(!in)return;lv_point_t p;lv_indev_get_point(in,&p);
 if(code==LV_EVENT_PRESSED){keyboard_clear();ui.knob_drag=true;ui.knob_moved=false;ui.knob_origin=ui.knob_value;ui.knob_y=p.y;ui.knob_x=p.x;return;}
 if(code==LV_EVENT_PRESS_LOST){ui.knob_drag=false;return;}
 if(!ui.knob_drag)return;
 const float dy=(ui.knob_y-p.y)/1.2f;if(fabsf(dy)>3||abs(p.x-ui.knob_x)>3)ui.knob_moved=true;
 int value=ui.knob_value;
 if(ui.knob_moved)value=std::clamp(int(lroundf(ui.knob_origin+dy)),0,100);
 else if(code==LV_EVENT_RELEASED){lv_area_t a;lv_obj_get_coords(ui.volume,&a);float angle=atan2f(p.x-(a.x1+a.x2)/2.f,-(p.y-(a.y1+a.y2)/2.f))*57.2957795f;value=std::clamp(int(lroundf((angle+135)/270*100)),0,100);}
 if(value!=ui.knob_value){ui.knob_value=value;lv_obj_invalidate(ui.volume);lv_slider_set_value(ui.volume_slider,value,LV_ANIM_OFF);ui.interaction_tick=lv_tick_get();char b[8];snprintf(b,sizeof b,"%d",value);text(ui.volume_text,b);emit(PLAYER_UI_VOLUME,value);}
 if(code==LV_EVENT_RELEASED)ui.knob_drag=false;
}
// Build one bounded RGB565 disc per changed value. No rotated LVGL layer,
// rounded clipping mask or transformed drawing can leak outside the knob.
void knob_draw(lv_event_t *e) {
 auto *o=lv_event_get_target_obj(e);lv_area_t a;lv_obj_get_coords(o,&a);auto *l=lv_event_get_layer(e);
 const auto &src=control_images[7][0];int w=src.header.w,h=src.header.h;
 if(painted_knob!=ui.knob_value){
  const float t=(ui.knob_value*2.7f-175.f)*.0174532925f,co=cosf(t),si=sinf(t);
  for(int y=0;y<h;y++)for(int x=0;x<w;x++){
   const auto *row=reinterpret_cast<const uint16_t*>(src.data+y*src.header.stride);uint16_t p=row[x];
   float dx=x-56.f,dy=y-56.f;
   if(dx*dx+dy*dy<38.f*38.f){
    float sx=56+co*dx+si*dy,sy=56-si*dx+co*dy;int ix=int(sx),iy=int(sy),fx=int((sx-ix)*256),fy=int((sy-iy)*256);
    const auto *r0=reinterpret_cast<const uint16_t*>(src.data+iy*src.header.stride),*r1=reinterpret_cast<const uint16_t*>(src.data+(iy+1)*src.header.stride);
    int rr=0,gg=0,bb=0;for(int j=0;j<4;j++){uint16_t c=(j<2?r0:r1)[ix+(j&1)];int weight=((j&1)?fx:256-fx)*((j&2)?fy:256-fy);rr+=(c>>11)*weight;gg+=((c>>5)&63)*weight;bb+=(c&31)*weight;}p=uint16_t((rr>>16)<<11|(gg>>16)<<5|(bb>>16));
   }knob_pixels[y*w+x]=p;
  }
  raw(knob_image,reinterpret_cast<const uint8_t*>(knob_pixels),w,h,w*2);lv_image_cache_drop(&knob_image);painted_knob=ui.knob_value;
 }
 draw_sprite(l,a,&knob_image);
 float angle=(135+ui.knob_value*2.7f)*.0174532925f;int cx=a.x1+56,cy=a.y1+56;
 line(l,cx+int(cosf(angle)*9),cy+int(sinf(angle)*9),cx+int(cosf(angle)*31),cy+int(sinf(angle)*31),LIME,8);
}

// Direct bounded RGB565 raster: no per-bar/particle LVGL draw allocations and
// no repeated spectrum drawing for each display strip. Exact shell restored.
struct VisualRaster {
 uint16_t *row(int y){return reinterpret_cast<uint16_t*>(visual_buffer->data+y*visual_buffer->header.stride);}
 static uint16_t pack(uint32_t c){return uint16_t(((c>>19)&31)<<11|((c>>10)&63)<<5|((c>>3)&31));}
 void pixel(int x,int y,uint32_t c,int alpha=255){
  if(unsigned(x)>=453||unsigned(y)>=110)return;
  auto &p=row(y)[x];uint16_t q=pack(c);
  if(alpha==255){p=q;return;}int inv=255-alpha;
  p=uint16_t((((p>>11)*inv+(q>>11)*alpha)/255)<<11|((((p>>5)&63)*inv+((q>>5)&63)*alpha)/255)<<5|((p&31)*inv+(q&31)*alpha)/255);
 }
 void box(int x,int y,int w,int h,uint32_t c,int alpha=255){for(int j=std::max(y,0);j<std::min(y+h,110);j++)for(int i=std::max(x,0);i<std::min(x+w,453);i++)pixel(i,j,c,alpha);}
 void segment(int x1,int y1,int x2,int y2,uint32_t c,int alpha=255){
  int dx=abs(x2-x1),dy=-abs(y2-y1),sx=x1<x2?1:-1,sy=y1<y2?1:-1,err=dx+dy;
  for(;;){pixel(x1,y1,c,alpha);if(x1==x2&&y1==y2)break;int e=err*2;if(e>=dy){err+=dy;x1+=sx;}if(e<=dx){err+=dx;y1+=sy;}}
 }
 void soft_dot(float x,float y,float radius,uint32_t c,float alpha){
  int extent=int(ceilf(radius+1));
  for(int j=int(floorf(y))-extent;j<=int(ceilf(y))+extent;j++)for(int i=int(floorf(x))-extent;i<=int(ceilf(x))+extent;i++){
   float dx=i-x,dy=j-y,d2=dx*dx+dy*dy;if(d2>(radius+1)*(radius+1))continue;
   float coverage=std::clamp(radius+.5f-sqrtf(d2),0.f,1.f);if(coverage>0)pixel(i,j,c,int(alpha*coverage));
  }
 }
 void dot(int x,int y,int radius,uint32_t c,int alpha){for(int j=-radius;j<=radius;j++)for(int i=-radius;i<=radius;i++){int d=i*i+j*j;if(d<=radius*radius)pixel(x+i,y+j,c,d<radius*radius?alpha:alpha/2);}}
};
void spectrum_paint() {
 if(!visual_buffer||ui.sleeping||ui.overlay)return;
 for(int y=0;y<110;y++)memcpy(visual_buffer->data+y*visual_buffer->header.stride,ui.shell_cache+(251+y)*2560+386*2,453*2);
 VisualRaster r;static uint32_t colors[BANDS]{};if(!colors[0])for(int i=0;i<BANDS;i++)colors[i]=rainbow(i);
 if(ui.state.visual_mode!=2)for(int x=7;x<453;x+=17)r.box(x,4,1,95,0xffffff,15);
 if(ui.state.visual_mode==1){
  for(int i=1;i<WAVE;i++){
   int x1=(i-1)*452/(WAVE-1),x2=i*452/(WAVE-1),y1=55-ui.wave[i-1]*47/32768,y2=55-ui.wave[i]*47/32768;auto c=colors[i*BANDS/WAVE];
   r.segment(x1,y1+2,x2,y2+2,c,45);r.segment(x1,y1-1,x2,y2-1,c,100);r.segment(x1,y1,x2,y2,c);r.segment(x1,y1+1,x2,y2+1,c,170);
  }
 }else if(ui.state.visual_mode==2){
  // Three continuous ribbons with fixed frequency positions, driven by audio.
  for(int lane=0;lane<3;lane++)for(int i=0;i<44;i++){
   const auto point=ui.particles.point(lane,i);
   const auto c=colors[(i*(BANDS-1)/43+lane*9)%BANDS];
   r.soft_dot(point.x,point.y,point.radius+1.7f,c,point.glow_alpha);
   r.soft_dot(point.x,point.y,point.radius,c,point.alpha);
   if(point.core_alpha>0)r.soft_dot(point.x,point.y,.65f,0xffffff,point.core_alpha);
  }
 }else{
  for(int i=0;i<BANDS;i++){
   int h=std::max(1,int(ui.bands[i]*90)),x=i*453/BANDS;auto c=colors[i];
   r.box(x,95-h,4,h,c);r.box(x,98,4,std::min(11,int(h*.18f)),c,33);
   int peak=int(ui.peaks[i]*90);if(peak>h+2)r.box(x,94-peak,4,1,c,190);
  }
  r.box(0,96,453,1,0xd5c6e3,90);
 }
 lv_draw_buf_flush_cache(visual_buffer,nullptr);lv_obj_invalidate(ui.spectrum);
}

void hide_notice(lv_timer_t *t) { lv_obj_add_flag(ui.notice,LV_OBJ_FLAG_HIDDEN);lv_timer_pause(t); }
void refresh_profile(lv_event_t *e) {
 switch(lv_event_get_code(e)) {
 case LV_EVENT_REFR_START:ui.refresh_started=lv_tick_get();ui.refresh_drew=false;break;
 case LV_EVENT_RENDER_START:ui.refresh_drew=true;break;
 case LV_EVENT_REFR_READY:if(ui.refresh_drew) {
  const uint32_t elapsed=lv_tick_elaps(ui.refresh_started);ui.stats.last_ms=elapsed;
  ui.stats.max_ms=std::max(ui.stats.max_ms,elapsed);ui.stats.interval_max_ms=std::max(ui.stats.interval_max_ms,elapsed);
  ui.stats.total_ms+=elapsed;++ui.stats.frames;
  if(elapsed>33){++ui.stats.slow_frames;}
  if(elapsed>50){++ui.stats.stall_frames;}
  // This is classification for measurement only. Touch never stops animation.
  const bool scrolling=(ui.playlist&&lv_obj_is_visible(ui.playlist)&&lv_obj_is_scrolling(ui.playlist))||
    (ui.folders&&lv_obj_is_visible(ui.folders)&&lv_obj_is_scrolling(ui.folders))||
    (ui.interaction_tick&&lv_tick_elaps(ui.interaction_tick)<120);
  if(scrolling){++ui.stats.scroll_frames;ui.stats.scroll_total_ms+=elapsed;
   if(elapsed>33){++ui.stats.scroll_slow_frames;}
   if(elapsed>50){++ui.stats.scroll_stall_frames;}}
 }break;
 default:break;
 }
}
} // namespace

namespace {
void visible(lv_obj_t *object,bool on) {
 if(!object||lv_obj_has_flag(object,LV_OBJ_FLAG_HIDDEN)==!on)return;
 if(on)lv_obj_remove_flag(object,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(object,LV_OBJ_FLAG_HIDDEN);
}
void refresh_library_view() {
 const bool settings=ui.settings_open,folders=ui.state.library_tab==1;
 visible(ui.settings,settings);visible(ui.playlist,!settings&&!folders);visible(ui.folders,!settings&&folders);
 active(ui.settings_tab,settings);for(int i=0;i<3;++i)active(ui.tabs[i],!settings&&ui.state.library_tab==i);
 text(ui.empty,folders?"No folders on SD card":"No tracks in this view");
 visible(ui.empty,!settings&&(folders?ui.folder_count==0:ui.tracks.empty()));
 text(ui.list_label,settings?"SETTINGS":folders?"FOLDERS ON SD CARD":ui.state.library_tab==2?"YOUR FAVORITES":ui.folder_label.empty()?"SD COLLECTION":ui.folder_label.c_str());
 char count[40],duration[32];snprintf(count,sizeof count,"%u %s",unsigned(folders?ui.folder_count:ui.tracks.size()),folders?"FOLDERS":"TRACKS");
 text(ui.list_count,settings?"":count);text(ui.queue_status,settings?"SELECT A CATEGORY":folders?"SELECT A FOLDER":"SD CARD");
 time_text(duration,sizeof duration,uint32_t(std::min<uint64_t>(ui.playlist_ms,UINT32_MAX)));
 text(ui.queue_duration,settings||folders?"":duration);
}
void settings_visible(bool on) {
 keyboard_clear();ui.settings_open=on;refresh_library_view();
}
void cover_pins(lv_event_t *e) {
 auto *l=lv_event_get_layer(e);for(int x:{49,329})for(int y:{115,394}){rect(l,x-5,y-5,11,11,0x171b1a,6);circle(l,x,y,4,0xa3aba3,1);}
}
lv_obj_t *setting_button(lv_obj_t *p,const char *name,int x,int y,int w,player_ui_action_t a,int v=0) {
 auto *b=button(p,x,y,w,53,a,NONE,PILL,name,v);return b;
}
void setting_event(lv_event_t *e) {
 auto *o=lv_event_get_target_obj(e);emit(o==ui.dragon_speed?PLAYER_UI_DRAGON_SPEED:o==ui.track_speed?PLAYER_UI_TRACK_SPEED:PLAYER_UI_BRIGHTNESS,lv_slider_get_value(o));
}
lv_obj_t *setting_slider(lv_obj_t *p,int y,int low,int high) {
 auto *v=lv_slider_create(p);lv_obj_set_pos(v,12,y);lv_obj_set_size(v,314,9);lv_slider_set_range(v,low,high);lv_obj_set_ext_click_area(v,21);
 lv_obj_set_ext_click_area(v,22);lv_obj_set_style_bg_color(v,color(0x303635),LV_PART_MAIN);lv_obj_set_style_bg_color(v,color(LIME),LV_PART_INDICATOR);lv_obj_set_style_bg_color(v,color(WHITE),LV_PART_KNOB);lv_obj_add_event_cb(v,setting_event,LV_EVENT_VALUE_CHANGED,nullptr);keyboard_bind(v);return v;
}
#include "aurora_settings.inc"
void dragon_paint(bool force=false) {
 if(!ui.dragon_pixels){return;}
 const bool flight=ui.state.dragon_type!=0;const int count=flight?60:100;const double period=flight?1.65:4.;
 int frame=int(fmod(ui.dragon_phase,period)*count/period)+(flight?0:60);
 int hue=ui.state.dragon_colors?int(fmod(ui.dragon_phase*45.,360.)):0;
 if((ui.overlay||ui.sleeping)&&!force)return;
 if(!force&&frame==ui.dragon_frame&&hue==ui.dragon_hue){return;}
 ui.dragon_frame=frame;ui.dragon_hue=hue;const uint8_t *src=(ui.dragon_cache)+frame*132*125*3;
 const float co=cosf(hue*.0174532925f),si=sinf(hue*.0174532925f);
 const float mat[9]={.213f+.787f*co-.213f*si,.715f-.715f*co-.715f*si,.072f-.072f*co+.928f*si,.213f-.213f*co+.143f*si,.715f+.285f*co+.140f*si,.072f-.072f*co-.283f*si,.213f-.213f*co-.787f*si,.715f-.715f*co+.715f*si,.072f+.928f*co+.072f*si};
 // Component lookup tables replace nine floating-point multiplies per texel.
 int cr[32][3],cg[64][3],cb[32][3];
 for(int v=0;v<64;v++)for(int c=0;c<3;c++){cg[v][c]=int(mat[c*3+1]*(v*255/63)*256);if(v<32){cr[v][c]=int(mat[c*3]*(v*255/31)*256);cb[v][c]=int(mat[c*3+2]*(v*255/31)*256);}}
 for(int y=0;y<125;++y) {const auto *bg=reinterpret_cast<const uint16_t *>((ui.shell_cache)+(y+415)*2560+123*2);
  for(int x=0;x<132;++x,src+=3) {int alpha=src[2];uint16_t back=bg[x];if(!alpha){ui.dragon_pixels[y*132+x]=back;continue;}
   uint16_t p=src[0]|(uint16_t(src[1])<<8);int r=((p>>11)&31)*255/31,g=((p>>5)&63)*255/63,b=(p&31)*255/31;
   if(hue) {int ri=p>>11,gi=(p>>5)&63,bi=p&31;int rr=std::clamp((cr[ri][0]+cg[gi][0]+cb[bi][0]+128)>>8,0,255),gg=std::clamp((cr[ri][1]+cg[gi][1]+cb[bi][1]+128)>>8,0,255),bb=std::clamp((cr[ri][2]+cg[gi][2]+cb[bi][2]+128)>>8,0,255);r=rr;g=gg;b=bb;}
   r=(r*alpha+((back>>11)&31)*255/31*(255-alpha))/255;g=(g*alpha+((back>>5)&63)*255/63*(255-alpha))/255;b=(b*alpha+(back&31)*255/31*(255-alpha))/255;
   ui.dragon_pixels[y*132+x]=uint16_t((r>>3)<<11|(g>>2)<<5|(b>>3));
  }
 }
 lv_image_cache_drop(&ui.dragon_image);lv_obj_invalidate(ui.dragon);
}
void dragon_tick(lv_timer_t *) {
 const uint32_t now=lv_tick_get(),dt=now-ui.dragon_tick;ui.dragon_tick=now;
 // Scrolling must not freeze the independently visible audio visualization.
 // Timers already bound the work; sleep and full-screen dialogs still suspend it.
 if(!ui.state.animations||ui.overlay||ui.sleeping){return;}
 if(ui.visual_source){float bands[32];int16_t wave[128];if(ui.visual_source(bands,wave,ui.visual_context))player_ui_set_spectrum(bands,32,wave,128);}
 if(ui.state.playing){ui.visual_phase+=std::min<uint32_t>(dt,200)/1000.;if(!ui.settings_open)for(auto &r:ui.rows)if(r.selected&&!lv_obj_has_flag(r.box,LV_OBJ_FLAG_HIDDEN))lv_obj_invalidate(r.menu);}
 ui.dragon_phase+=std::min<uint32_t>(dt,200)/1000.*std::clamp<int>(ui.state.dragon_speed,4,16)/10.;dragon_paint();
}
void ui_cleanup(lv_event_t *) {
 for(auto &r:ui.rows)if(r.cache){lv_image_cache_drop(r.cache);lv_draw_buf_destroy(r.cache);r.cache=nullptr;r.cached=false;}
 if(visual_buffer){lv_draw_buf_destroy(visual_buffer);visual_buffer=nullptr;}
 if(ui.dragon_timer){lv_timer_delete(ui.dragon_timer);}
 if(ui.notice_timer){lv_timer_delete(ui.notice_timer);}
 if(ui.saver){lv_obj_delete(ui.saver);}
 if(ui.lock_panel){lv_obj_delete(ui.lock_panel);ui.lock_panel=nullptr;}
 lv_image_cache_drop(&ui.dragon_image);pixel_free(ui.dragon_pixels);ui.dragon_pixels=nullptr;
 pixel_free(ui.shell_cache);pixel_free(ui.control_cache);pixel_free(ui.dragon_cache);
 for(auto &t:ui.thumbnails){lv_image_cache_drop(&t.image);pixel_free(t.pixels);t.pixels=nullptr;}
}
void battery_draw(lv_event_t *e) {
 lv_area_t a;lv_obj_get_coords(lv_event_get_target_obj(e),&a);auto *l=lv_event_get_layer(e);int x=a.x1,y=a.y1+7;
 rect(l,x,y,43,26,WHITE,4);rect(l,x+3,y+3,37,20,0x080a0c,2);rect(l,x+44,y+8,4,10,WHITE,1);
 if(ui.state.battery_percent>=0)rect(l,x+6,y+6,31*std::clamp<int>(ui.state.battery_percent,0,100)/100,14,ui.state.battery_percent<=15?0xff895f:WHITE,1);
 else {line(l,x+15,y+13,x+28,y+13,MUTED,2);}
 if(ui.state.battery_charging){int c=x+68;triangle(l,c+3,y-1,c-10,y+14,c+1,y+13,LIME);triangle(l,c-1,y+9,c+10,y+8,c-4,y+27,LIME);}
}
float css_ease(float x){float t=x;for(int n=0;n<5;n++){float u=1-t;float bx=3*u*u*t*.42f+3*u*t*t*.58f+t*t*t,dx=3*u*u*.42f+6*u*t*.16f+3*t*t*.42f;t=std::clamp(t-(bx-x)/std::max(.1f,dx),0.f,1.f);}return t*t*(3-2*t);}
void row_eq(lv_event_t *e) {auto *r=static_cast<Row*>(lv_event_get_user_data(e));if(!r->selected)return;lv_area_t a;lv_obj_get_coords(lv_event_get_target_obj(e),&a);auto *l=lv_event_get_layer(e);const float heights[]={.55f,1.f,.75f},delays[]={0,.4f,.2f};for(int i=0;i<3;i++){float h=heights[i];if(ui.state.playing){float t=fmod(float(ui.visual_phase)+delays[i],1.6f)/.8f;t=t>1?2-t:t;h+=(.2f-h)*css_ease(t);}int bh=std::max(3,int(18*h));rect(l,a.x1+5+i*5,a.y1+21-bh,3,bh,LIME,1);}}

lv_obj_t *meter(lv_obj_t *p,int x,int y,int w,bool seek) {
 auto *o=lv_slider_create(p);lv_obj_add_event_cb(o,keyboard_touch,LV_EVENT_PRESSED,nullptr);lv_obj_remove_style_all(o);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,seek?9:15);lv_slider_set_range(o,0,seek?1000:100);
 solid(o,0x25292a,8);lv_obj_set_style_border_width(o,1,0);lv_obj_set_style_border_color(o,color(0x717774),0);lv_obj_set_ext_click_area(o,21);
 if(seek){lv_obj_set_pos(o,x-12,y-22);lv_obj_set_size(o,w+24,53);lv_obj_set_style_bg_opa(o,0,LV_PART_MAIN);lv_obj_set_style_border_width(o,0,0);lv_obj_set_style_bg_opa(o,0,LV_PART_INDICATOR);lv_obj_set_style_bg_opa(o,0,LV_PART_KNOB);lv_obj_set_ext_click_area(o,0);lv_obj_add_event_cb(o,seek_event,LV_EVENT_ALL,nullptr);lv_obj_add_event_cb(o,seek_rainbow,LV_EVENT_DRAW_POST,nullptr);}
 else {lv_obj_set_style_bg_color(o,color(LIME),LV_PART_INDICATOR);lv_obj_set_style_bg_opa(o,255,LV_PART_INDICATOR);lv_obj_set_style_radius(o,8,LV_PART_INDICATOR);lv_obj_set_style_bg_color(o,color(0xe4ebd7),LV_PART_KNOB);lv_obj_set_style_bg_opa(o,255,LV_PART_KNOB);lv_obj_set_style_radius(o,3,LV_PART_KNOB);lv_obj_set_style_pad_hor(o,6,LV_PART_KNOB);lv_obj_set_style_pad_ver(o,4,LV_PART_KNOB);lv_obj_add_event_cb(o,volume_event,LV_EVENT_VALUE_CHANGED,nullptr);}
 return o;
}
}
extern "C" void player_ui_create(lv_obj_t *parent,player_ui_action_cb cb,void *ctx) {
 ui.callback=cb;ui.context=ctx;ui.state.animations=true;ui.state.dragon_type=1;ui.state.dragon_speed=10;ui.state.dragon_colors=true;
 lv_image_cache_resize(0,true);
 // Resident RGB565 assets avoid DIO flash reads competing with code/audio on every repaint.
 auto cache=[](const uint8_t *source,const uint8_t *end,size_t bytes){auto *p=static_cast<uint8_t*>(pixel_alloc(bytes));if(p&&!aurora_unpack(source,size_t(end-source),p,bytes)){pixel_free(p);p=nullptr;}return p;};
 ui.shell_cache=cache(shell_start,shell_end,1280*720*2);ui.control_cache=cache(control_start,control_end,AURORA_CONTROLS_BYTES);ui.dragon_cache=cache(dragon_start,dragon_end,160*132*125*3);
 if(!ui.shell_cache||!ui.control_cache||!ui.dragon_cache){label(parent,"Insufficient graphics memory. Restart the player.",50,50,1100,&aurora_sans20);return;}
 aurora_fx_skin::prepare(ui.control_cache);
 raw(shell,ui.shell_cache,1280,720,2560);raw(dragon_thumb,thumb_start,56,56,112);
 for(int i=0;i<24;i++)for(int on=0;on<4;on++){const auto &r=aurora_controls[i];raw(control_images[i][on],(ui.control_cache)+r.offset+on*r.stride*r.h,r.w,r.h,r.stride);}
 ui.root=plain(parent,0,0,1280,720);solid(ui.root,BLACK);auto *bg=lv_image_create(ui.root);lv_image_set_src(bg,&shell);lv_obj_remove_flag(bg,LV_OBJ_FLAG_CLICKABLE);
 auto *display=lv_obj_get_display(ui.root);lv_display_add_event_cb(display,refresh_profile,LV_EVENT_REFR_START,nullptr);lv_display_add_event_cb(display,refresh_profile,LV_EVENT_RENDER_START,nullptr);lv_display_add_event_cb(display,refresh_profile,LV_EVENT_REFR_READY,nullptr);
 lv_obj_add_event_cb(ui.root,ui_cleanup,LV_EVENT_DELETE,nullptr);
 // Equal vertical centers and balanced spacing within the original 350 px status capsule.
 ui.clock=label(ui.root,"--:--",904,36,94,&aurora_mono29);lv_obj_set_style_text_align(ui.clock,LV_TEXT_ALIGN_CENTER,0);
 ui.bluetooth=button(ui.root,1010,22,122,53,PLAYER_UI_BLUETOOTH);
 auto *bt_icon=plain(ui.bluetooth,34,5,54,44);lv_obj_add_event_cb(bt_icon,[](lv_event_t *e){lv_area_t a;lv_obj_get_coords(lv_event_get_target_obj(e),&a);icon(lv_event_get_layer(e),BLUETOOTH,a.x1+27,a.y1+22,ui.state.bluetooth_available&&ui.state.bluetooth_enabled?0xa8ed76:0xb5bbb7,32);},LV_EVENT_DRAW_MAIN,nullptr);
 ui.battery_icon=plain(ui.root,1146,29,85,44);lv_obj_add_flag(ui.battery_icon,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_event_cb(ui.battery_icon,battery_touch,LV_EVENT_ALL,nullptr);lv_obj_add_event_cb(ui.battery_icon,battery_draw,LV_EVENT_DRAW_MAIN,nullptr);
 ui.cover_box=plain(ui.root,ART_X,ART_Y,ART_W,ART_H);solid(ui.cover_box,BLACK);lv_obj_set_style_radius(ui.cover_box,10,0);lv_obj_set_style_clip_corner(ui.cover_box,true,0);lv_obj_add_flag(ui.cover_box,LV_OBJ_FLAG_HIDDEN);ui.cover=lv_image_create(ui.cover_box);lv_image_set_pivot(ui.cover,0,0);

 auto *pins=plain(ui.root,34,100,310,310);lv_obj_add_event_cb(pins,cover_pins,LV_EVENT_DRAW_MAIN,nullptr);
 auto *cover_touch=plain(ui.root,36,146,307,262);lv_obj_add_flag(cover_touch,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_event_cb(cover_touch,[](lv_event_t*){emit(PLAYER_UI_FILE_INFO,-1);},LV_EVENT_LONG_PRESSED,nullptr);
 auto *badge=plain(ui.root,40,451,82,58);solid(badge,0xcdd1c7,5);lv_obj_set_style_bg_grad_color(badge,color(0xb0b4ad),0);lv_obj_set_style_bg_grad_dir(badge,LV_GRAD_DIR_VER,0);lv_obj_set_style_border_color(badge,color(0xeeeeee),0);lv_obj_set_style_border_width(badge,1,0);
 ui.format=label(badge,"--",0,0,82,&aurora_mono29,0x131615);lv_obj_set_style_text_align(ui.format,LV_TEXT_ALIGN_CENTER,0);lv_obj_center(ui.format);
 ui.sample=label(ui.root,"--",253,450,55,&aurora_mono19,WHITE);lv_obj_set_style_text_align(ui.sample,LV_TEXT_ALIGN_RIGHT,0);label(ui.root,"kHz",313,453,34,&aurora_mono13,WHITE);
 ui.bitrate=label(ui.root,"--",253,480,55,&aurora_mono19,WHITE);lv_obj_set_style_text_align(ui.bitrate,LV_TEXT_ALIGN_RIGHT,0);label(ui.root,"kbps",311,483,38,&aurora_mono13,WHITE);ui.depth=label(ui.root,"",253,507,93,&aurora_mono12,MUTED);lv_obj_set_style_text_align(ui.depth,LV_TEXT_ALIGN_RIGHT,0);
 ui.dragon_pixels=static_cast<uint16_t*>(pixel_alloc(132*125*2));raw(ui.dragon_image,reinterpret_cast<const uint8_t*>(ui.dragon_pixels),132,125,264);ui.dragon=lv_image_create(ui.root);lv_obj_set_pos(ui.dragon,123,415);if(ui.dragon_pixels)lv_image_set_src(ui.dragon,&ui.dragon_image);ui.dragon_tick=lv_tick_get();dragon_paint(true);ui.dragon_timer=lv_timer_create(dragon_tick,33,nullptr);
 ui.led=plain(ui.root,386,113,8,8);solid(ui.led,0x757a67,4);ui.play_state=label(ui.root,"READY",404,108,173,&aurora_mono12,MUTED);lv_obj_set_style_text_letter_space(ui.play_state,1,0);ui.codec=label(ui.root,"--",693,108,146,&aurora_mono12,MUTED);lv_obj_set_style_text_align(ui.codec,LV_TEXT_ALIGN_RIGHT,0);
 ui.title=label(ui.root,"SELECT A TRACK",386,139,453,&aurora_title31);ui.artist=label(ui.root,"Local SD audio",386,187,393,&aurora_sans18,0xb6baba);ui.album=label(ui.root,"/Music",386,212,393,&aurora_sans14,0xb7bfbc);
 ui.favorite=asset_button(0,PLAYER_UI_FAVORITE,-1);ui.spectrum=lv_canvas_create(ui.root);lv_obj_set_pos(ui.spectrum,386,251);visual_buffer=lv_draw_buf_create(453,110,LV_COLOR_FORMAT_RGB565,LV_STRIDE_AUTO);if(visual_buffer){lv_canvas_set_draw_buf(ui.spectrum,visual_buffer);spectrum_paint();}ui.caption=label(ui.root,"SPECTRUM / PAUSED",386,366,449,&aurora_mono10,0xabb5af);lv_obj_set_style_text_letter_space(ui.caption,1,0);
 ui.elapsed=label(ui.root,"00:00",386,396,48,&aurora_mono13);ui.duration=label(ui.root,"--:--",798,396,48,&aurora_mono13);ui.seek=meter(ui.root,448,401,330,true);
 ui.shuffle=asset_button(1,PLAYER_UI_SHUFFLE);asset_button(2,PLAYER_UI_PREVIOUS);ui.play=asset_button(3,PLAYER_UI_PLAY_PAUSE);asset_button(4,PLAYER_UI_NEXT);ui.repeat=asset_button(5,PLAYER_UI_REPEAT);
 for(int i=0;i<3;i++){ui.tabs[i]=asset_button(8+i,PLAYER_UI_LIBRARY_TAB,i);}
 ui.settings_tab=asset_button(11,PLAYER_UI_SETTINGS,-1);
 ui.list_label=label(ui.root,"SD COLLECTION",880,166,220,&aurora_mono10,0xb9c2bb);ui.list_count=label(ui.root,"0 TRACKS",1101,166,146,&aurora_mono10,0xb9c2bb);lv_obj_set_style_text_align(ui.list_count,LV_TEXT_ALIGN_RIGHT,0);
 ui.queue_status=label(ui.root,"SD CARD",880,524,254,&aurora_mono10,0xb9c2bb);ui.queue_duration=label(ui.root,"",1136,524,111,&aurora_mono10,0xb9c2bb);lv_obj_set_style_text_align(ui.queue_duration,LV_TEXT_ALIGN_RIGHT,0);
 ui.playlist=plain(ui.root,880,186,367,328);solid(ui.playlist,BLACK);lv_obj_add_flag(ui.playlist,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_scroll_dir(ui.playlist,LV_DIR_VER);lv_obj_set_scrollbar_mode(ui.playlist,LV_SCROLLBAR_MODE_ON);lv_obj_set_style_bg_color(ui.playlist,color(0xb7c78d),LV_PART_SCROLLBAR);lv_obj_set_style_width(ui.playlist,6,LV_PART_SCROLLBAR);lv_obj_set_style_bg_opa(ui.playlist,255,LV_PART_SCROLLBAR);ui.spacer=plain(ui.playlist,0,0,1,1);
 for(auto &r:ui.rows) {r.box=plain(ui.playlist,0,0,358,72);solid(r.box,BLACK,5);lv_obj_add_event_cb(r.box,row_raster_draw,static_cast<lv_event_code_t>(LV_EVENT_DRAW_MAIN|LV_EVENT_PREPROCESS),&r);lv_obj_add_event_cb(r.box,keyboard_touch,LV_EVENT_PRESSED,nullptr);lv_obj_add_event_cb(r.box,keyboard_row_draw,LV_EVENT_DRAW_POST,&r);lv_obj_add_flag(r.box,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_event_cb(r.box,row_click,LV_EVENT_SHORT_CLICKED,&r);lv_obj_add_event_cb(r.box,row_menu,LV_EVENT_LONG_PRESSED,&r);lv_obj_set_style_border_color(r.box,color(0x232826),0);lv_obj_set_style_border_width(r.box,1,0);lv_obj_set_style_border_side(r.box,LV_BORDER_SIDE_BOTTOM,0);r.marker=plain(r.box,0,14,4,42);solid(r.marker,LIME,2);r.image=lv_image_create(r.box);lv_obj_set_pos(r.image,10,8);lv_image_set_src(r.image,&dragon_thumb);r.title=label(r.box,"",73,15,212,&aurora_sans17);r.artist=label(r.box,"",73,40,200,&aurora_sans14,0xa5ada9);r.duration=label(r.box,"",287,28,48,&aurora_mono13,0xb8c0ba);r.menu=plain(r.box,334,24,24,25);lv_obj_add_event_cb(r.menu,row_eq,LV_EVENT_DRAW_MAIN,&r);lv_obj_add_flag(r.box,LV_OBJ_FLAG_HIDDEN);}
 lv_obj_add_event_cb(ui.playlist,keyboard_touch,LV_EVENT_PRESSED,nullptr);add_scroll_handle(ui.playlist,367,328);lv_obj_add_event_cb(ui.playlist,list_scroll,LV_EVENT_SCROLL,nullptr);ui.folders=create_embedded_folders();ui.empty=label(ui.root,"No tracks in this view",908,297,307,&aurora_sans17,MUTED);lv_obj_set_style_text_align(ui.empty,LV_TEXT_ALIGN_CENTER,0);
 create_settings();for(auto *page:ui.settings_pages)lv_obj_add_event_cb(page,keyboard_touch,LV_EVENT_PRESSED,nullptr);
 const auto &kr=aurora_controls[7];ui.volume=plain(ui.root,kr.x,kr.y,kr.w,kr.h);lv_obj_add_flag(ui.volume,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_event_cb(ui.volume,knob_draw,LV_EVENT_DRAW_MAIN,nullptr);lv_obj_add_event_cb(ui.volume,knob_touch,LV_EVENT_ALL,nullptr);
 ui.mute=asset_button(6,PLAYER_UI_MUTE);ui.volume_slider=meter(ui.root,201,627,129,false);ui.volume_text=label(ui.root,"65",278,587,48,&aurora_mono16,0x252d22);ui.volume_percent=label(ui.root,"%",328,589,12,&aurora_mono13,0x252d22);lv_obj_set_style_text_align(ui.volume_text,LV_TEXT_ALIGN_RIGHT,0);
 for(int i=0;i<4;++i){ui.effects[i]=asset_button(12+i,PLAYER_UI_EFFECT,i);}
 for(int i=0;i<3;++i){ui.formats[i]=asset_button(16+i,PLAYER_UI_FORMAT_FILTER,i);ui.visuals[i]=asset_button(19+i,PLAYER_UI_VISUAL_MODE,i);}
 for(int i=3;i<5;++i)ui.formats[i]=asset_button(19+i,PLAYER_UI_FORMAT_FILTER,i);
 ui.firmware=label(ui.root,"v1.3.2",1146,685,89,&aurora_mono12,0x353f2e);lv_obj_set_style_text_align(ui.firmware,LV_TEXT_ALIGN_RIGHT,0);
 ui.notice=plain(ui.root,383,545,568,44);solid(ui.notice,0x20261d,8);lv_obj_set_style_border_color(ui.notice,color(LIME),0);lv_obj_set_style_border_width(ui.notice,1,0);ui.notice_label=label(ui.notice,"",14,13,540,&aurora_sans14,LIME);lv_obj_add_flag(ui.notice,LV_OBJ_FLAG_HIDDEN);ui.notice_timer=lv_timer_create(hide_notice,4000,nullptr);lv_timer_pause(ui.notice_timer);
 lv_obj_update_layout(ui.root);
}
extern "C" void player_ui_set_state(const player_ui_state_t *s) {
 const bool previous_bt_available=ui.state.bluetooth_available,previous_bt_enabled=ui.state.bluetooth_enabled,previous_covers=ui.state.covers;
 if(!s||!ui.root){return;}if(s->library_tab!=ui.state.library_tab)keyboard_clear();bool selection=s->track_index!=ui.state.track_index,playing=s->playing!=ui.state.playing,mute=s->muted!=ui.state.muted,repeat=s->repeat!=ui.state.repeat;
 bool battery=s->battery_percent!=ui.state.battery_percent||s->battery_charging!=ui.state.battery_charging,mode=s->visual_mode!=ui.state.visual_mode;
 ui.updating=true;ui.state=*s;ui.folder_label=s->folder_label?s->folder_label:"";ui.artwork_key=key(s->artwork_key);text(ui.title,s->title&&*s->title?s->title:"SELECT A TRACK");text(ui.artist,s->artist);text(ui.album,s->album);text(ui.clock,s->clock_text&&*s->clock_text?s->clock_text:"--:--");const lv_font_t *clock_font=s->clock24?&aurora_mono29:&aurora_mono19;int clock_top=100,clock_bottom=-100;const char *ct=s->clock_text&&*s->clock_text?s->clock_text:"--:--";for(const char *c=ct;*c;c++){lv_font_glyph_dsc_t glyph{};if(lv_font_get_glyph_dsc(clock_font,&glyph,static_cast<unsigned char>(*c),0)&&glyph.box_h){int baseline=clock_font->line_height-clock_font->base_line;clock_top=std::min(clock_top,baseline-int(glyph.ofs_y)-int(glyph.box_h));clock_bottom=std::max(clock_bottom,baseline-int(glyph.ofs_y)-1);}}if(clock_bottom>=clock_top)lv_obj_set_y(ui.clock,48-(clock_top+clock_bottom)/2);if(lv_obj_get_style_text_font(ui.clock,0)!=clock_font){lv_obj_set_style_text_font(ui.clock,clock_font,0);lv_obj_set_height(ui.clock,lv_font_get_line_height(clock_font));}
 char b[160],t[32];snprintf(b,sizeof b,"v%s",s->firmware?s->firmware:"1.3.6");text(ui.firmware,b);const char *format=s->format&&*s->format?s->format:"--";text(ui.format,format);
 if(s->bitrate_kbps)snprintf(b,sizeof b,"%s · %lu kbps",format,static_cast<unsigned long>(s->bitrate_kbps));else snprintf(b,sizeof b,"%s",format);text(ui.codec,b);
 if(s->sample_rate)snprintf(b,sizeof b,"%g",s->sample_rate/1000.);else snprintf(b,sizeof b,"--");text(ui.sample,b);if(s->bitrate_kbps)snprintf(b,sizeof b,"%lu",static_cast<unsigned long>(s->bitrate_kbps));else snprintf(b,sizeof b,"--");text(ui.bitrate,b);if(s->bit_depth)snprintf(b,sizeof b,"%u bit",s->bit_depth);else b[0]=0;text(ui.depth,b);
 text(ui.play_state,s->playing?"NOW PLAYING":s->track_count?"PAUSED":"READY");if(playing)solid(ui.led,s->playing?LIME:0x757a67,4);
 snprintf(b,sizeof b,"%s / %s",s->visual_mode==1?"WAVE":s->visual_mode==2?"PARTICLES":"SPECTRUM",s->playing?"AUDIO":"PAUSED");text(ui.caption,b);
 if(!ui.dragging){time_text(t,sizeof t,s->position_ms);text(ui.elapsed,t);int value=s->duration_ms?int(uint64_t(s->position_ms)*1000/s->duration_ms):0;if(lv_slider_get_value(ui.seek)!=value){lv_slider_set_value(ui.seek,value,LV_ANIM_OFF);lv_obj_invalidate(ui.seek);}}time_text(t,sizeof t,s->duration_ms);text(ui.duration,s->duration_ms?t:"--:--");
 active(ui.shuffle,s->shuffle);active(ui.repeat,s->repeat!=0);active(ui.favorite,s->favorite);active(ui.bluetooth,s->bluetooth_connected);if(s->bluetooth_available!=previous_bt_available||s->bluetooth_enabled!=previous_bt_enabled)lv_obj_invalidate(ui.bluetooth);
 if(ui.knob_value!=s->volume&&!ui.knob_drag){ui.knob_value=s->volume;lv_obj_invalidate(ui.volume);}lv_slider_set_value(ui.volume_slider,s->muted?0:s->volume,LV_ANIM_OFF);snprintf(b,sizeof b,"%u",s->volume);text(ui.volume_text,s->muted?"MUTE":b);text(ui.volume_percent,s->muted?"":"%");
 for(int i=0;i<3;++i){active(ui.tabs[i],!ui.settings_open&&s->library_tab==i);active(ui.visuals[i],s->visual_mode==i);}for(int i=0;i<5;++i)active(ui.formats[i],s->format_filter==i);for(int i=0;i<4;++i)active(ui.effects[i],s->effect==i);
 if(s->battery_percent<0)snprintf(b,sizeof b,"--");else snprintf(b,sizeof b,"%d",std::clamp<int>(s->battery_percent,0,100));if(battery){lv_obj_invalidate(ui.battery_icon);}
 refresh_library_view();
 active(ui.dragon_types[0],s->dragon_type==0);active(ui.dragon_types[1],s->dragon_type==1);lv_slider_set_value(ui.dragon_speed,s->dragon_speed,LV_ANIM_OFF);snprintf(b,sizeof b,"%.1fx",s->dragon_speed/10.);text(ui.speed_value,b);lv_slider_set_value(ui.brightness,s->brightness,LV_ANIM_OFF);snprintf(b,sizeof b,"%u%%",s->brightness);text(ui.brightness_value,b);
 for(auto pair:{std::pair<lv_obj_t*,bool>{ui.dragon_colors,s->dragon_colors},{ui.animations,s->animations},{ui.clock_format,s->clock24}}){active(pair.first,pair.second);text(lv_obj_get_child(pair.first,0),pair.first==ui.clock_format?(pair.second?"24 h":"12 h"):(pair.second?"ON":"OFF"));}
 update_settings_state();dragon_paint();if(selection){if(ui.keyboard_tab<0&&s->track_index<ui.tracks.size()&&(ui.first<0||s->track_index<size_t(ui.first)||s->track_index>=size_t(ui.first+4)))list_geometry(true);else refresh_rows(true);}else if(previous_covers!=s->covers)refresh_rows(true);if(mode)spectrum_paint();if(playing){lv_obj_invalidate(ui.play);for(auto &r:ui.rows)if(r.selected)lv_obj_invalidate(r.menu);}if(mute)lv_obj_invalidate(ui.mute);if(repeat)lv_obj_invalidate(ui.repeat);ui.updating=false;
}

extern "C" void player_ui_set_playlist(const player_ui_track_t *tracks,size_t count,size_t current) {
 if(!ui.root) { return; }if(!tracks) { count=0; }bool changed=count!=ui.tracks.size();
 if(!changed) { for(size_t i=0;i<count;++i) { const auto &a=ui.tracks[i];const auto &b=tracks[i];if(a.title!=(b.title?b.title:"")||a.artist!=subtitle(b)||a.duration!=b.duration_ms||a.favorite!=b.favorite||a.key!=key(b.path)) { changed=true;break; } } }
 bool selected=current!=ui.state.track_index;ui.state.track_index=current;ui.state.track_count=count;if(!changed) { if(selected) { refresh_rows(true); }refresh_library_view();return; }
 if(ui.keyboard_tab>=0&&ui.keyboard_tab!=1){keyboard_clear();}ui.playlist_ms=0;ui.tracks.resize(count);for(size_t i=0;i<count;++i) { auto &t=tracks[i];ui.playlist_ms+=t.duration_ms;ui.tracks[i]={t.title?t.title:"",subtitle(t),t.duration_ms,key(t.path),t.favorite}; }list_geometry(true);refresh_library_view();
}
extern "C" void player_ui_update_track(size_t index,const player_ui_track_t *track) {
 if(!ui.root||!track||index>=ui.tracks.size()) { return; }auto &t=ui.tracks[index];const char *title=track->title?track->title:"";std::string artist=subtitle(*track);uint64_t k=key(track->path);
 if(t.title==title&&t.artist==artist&&t.duration==track->duration_ms&&t.favorite==track->favorite&&t.key==k) { return; }ui.playlist_ms=ui.playlist_ms-t.duration+track->duration_ms;t={title,artist,track->duration_ms,k,track->favorite};if(ui.first>=0&&index>=size_t(ui.first)&&index<size_t(ui.first+ROWS)) { refresh_rows(true); }
}
struct player_ui_playlist {std::vector<Track> tracks;uint64_t duration=0;};
extern "C" player_ui_playlist_t *player_ui_prepare_playlist(const player_ui_track_t *tracks,size_t count) {
 auto *prepared=new(std::nothrow) player_ui_playlist;if(!prepared)return nullptr;
 if(!tracks)count=0;
 prepared->tracks.reserve(count);
 for(size_t i=0;i<count;i++){const auto &t=tracks[i];prepared->duration+=t.duration_ms;prepared->tracks.push_back({t.title?t.title:"",subtitle(t),t.duration_ms,key(t.path),t.favorite});}
 return prepared;
}
extern "C" void player_ui_apply_playlist(player_ui_playlist_t *prepared,size_t current) {
 if(!prepared||!ui.root)return;
 if(ui.keyboard_tab>=0&&ui.keyboard_tab!=1){keyboard_clear();}ui.tracks.swap(prepared->tracks);ui.playlist_ms=prepared->duration;
 ui.state.track_index=current;ui.state.track_count=ui.tracks.size();
 refresh_library_view();
 list_geometry(true);
}
extern "C" void player_ui_free_playlist(player_ui_playlist_t *prepared){delete prepared;}
extern "C" void player_ui_set_visual_source(player_ui_visual_source_cb callback,void *context){ui.visual_source=callback;ui.visual_context=context;}
extern "C" void player_ui_set_spectrum(const float *bands,size_t count,const int16_t *wave,size_t wave_count) {
 if(!ui.spectrum||!ui.state.animations||ui.overlay||ui.sleeping) { return; }++ui.stats.spectrum_updates;bool bars=false,dots=false,waves=false;
 const uint32_t stamp=lv_tick_get();float dt=ui.particle_tick?std::clamp((stamp-ui.particle_tick)/1000.f,.001f,.050f):.033f;ui.particle_tick=stamp;
 ui.particles.advance(bands,count,dt,ui.state.playing);

 for(int i=0;i<BANDS;++i) { float before=ui.bands[i],v=bands&&count?bands[size_t(i)*count/BANDS]:0;v=std::clamp(v,0.f,1.f);ui.bands[i]=v>before?v:before*.35f+v*.65f;if(v==0&&ui.bands[i]<.001f) { ui.bands[i]=0; }bars|=std::max(1,int(before*91))!=std::max(1,int(ui.bands[i]*91));dots|=int(before*9)!=int(ui.bands[i]*9); }
 for(int i=0;i<WAVE;++i) { int16_t next=wave&&wave_count?wave[size_t(i)*wave_count/WAVE]:0;waves|=ui.wave[i]*58/32768!=next*58/32768;ui.wave[i]=next; }
 for(int i=0;i<BANDS;i++){float old=ui.peaks[i];ui.peaks[i]=std::max(ui.bands[i],ui.peaks[i]-.022f);if(int(old*90)!=int(ui.peaks[i]*90))bars=true;}
 bool changed=ui.state.visual_mode==0?bars:ui.state.visual_mode==1?waves:dots;if((changed||(ui.state.visual_mode==2&&ui.state.playing))&&ui.state.animations&&!ui.overlay) { spectrum_paint(); }else { ++ui.stats.skipped_spectrum_updates; }
}
extern "C" void player_ui_set_notice(const char *message) { if(!ui.notice) { return; }if(!message||!*message) { lv_obj_add_flag(ui.notice,LV_OBJ_FLAG_HIDDEN);lv_timer_pause(ui.notice_timer);return; }text(ui.notice_label,message);lv_obj_remove_flag(ui.notice,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(ui.notice);lv_timer_reset(ui.notice_timer);lv_timer_resume(ui.notice_timer); }
extern "C" bool player_ui_accepts_artwork(const char *path) {
 if(!ui.root)return false;
 if(!path||!*path)return true;
 return ui.state.covers&&ui.artwork_key==key(path);
}
extern "C" void player_ui_set_artwork(const lv_image_dsc_t *image) {
 if(!ui.cover||image==ui.artwork_source) { return; }if(image&&(image->header.cf!=LV_COLOR_FORMAT_RGB565||!image->header.w||!image->header.h||image->header.w>312||image->header.h>312)) { return; }
 if(ui.artwork_source) { lv_image_cache_drop(ui.artwork_source); }ui.artwork_source=image;if(!image) { lv_obj_add_flag(ui.cover_box,LV_OBJ_FLAG_HIDDEN);lv_image_set_src(ui.cover,nullptr);return; }
 int scale=std::max((ART_W*256+int(image->header.w)-1)/int(image->header.w),(ART_H*256+int(image->header.h)-1)/int(image->header.h));lv_image_set_src(ui.cover,image);lv_image_set_scale(ui.cover,scale);lv_obj_set_pos(ui.cover,(ART_W-int(image->header.w)*scale/256)/2,(ART_H-int(image->header.h)*scale/256)/2);lv_obj_remove_flag(ui.cover_box,LV_OBJ_FLAG_HIDDEN);
 if(ui.artwork_key)cache_thumbnail(ui.artwork_key,image);
}
namespace {
bool valid_thumbnail(const lv_image_dsc_t *image) {
 if(!image)return true;
 const auto &h=image->header;size_t stride=h.stride?h.stride:size_t(h.w)*2;
 return h.cf==LV_COLOR_FORMAT_RGB565&&h.w&&h.h&&h.w<=312&&h.h<=312&&image->data&&stride>=size_t(h.w)*2&&!(stride&1)&&image->data_size>=stride*h.h;
}
bool visible_thumbnail_key(uint64_t k) {
 if(!k||!ui.playlist||!lv_obj_is_visible(ui.playlist))return false;
 int top=std::max(0,int(lv_obj_get_scroll_y(ui.playlist))),bottom=top+lv_obj_get_height(ui.playlist);
 for(size_t i=size_t(top/CELL);i<ui.tracks.size()&&int(i*CELL)<bottom;i++)if(ui.tracks[i].key==k)return true;
 return false;
}
}
extern "C" size_t player_ui_get_missing_thumbnails(size_t *indices,size_t capacity) {
 if(!indices||!capacity||!ui.root||!ui.playlist||!ui.state.covers||ui.settings_open||ui.overlay||ui.menu||ui.sleeping||!lv_obj_is_visible(ui.playlist))return 0;
 capacity=std::min<size_t>(capacity,ROWS);size_t count=0;
 int top=std::max(0,int(lv_obj_get_scroll_y(ui.playlist))),bottom=top+lv_obj_get_height(ui.playlist);
 for(size_t i=size_t(top/CELL);i<ui.tracks.size()&&int(i*CELL)<bottom&&count<capacity;i++){
  const uint64_t k=ui.tracks[i].key;if(!k)continue;bool known=false;
  for(auto &t:ui.thumbnails)if(t.key==k&&(!t.missing||lv_tick_elaps(t.checked)<60000)){known=true;t.used=++ui.thumb_age;break;}
  for(size_t n=0;n<count&&!known;n++)known=ui.tracks[indices[n]].key==k;
  if(!known)indices[count++]=i;
 }
 return count;
}
extern "C" void player_ui_set_thumbnail(const char *path,const lv_image_dsc_t *image) {
 cache_thumbnail(key(path),image);
}
namespace {
void cache_thumbnail(uint64_t k,const lv_image_dsc_t *image) {
 if(!ui.root||!k||!valid_thumbnail(image))return;
 Thumbnail *entry=nullptr;
 for(auto &t:ui.thumbnails)if(t.key==k){entry=&t;break;}
 if(!entry)for(auto &t:ui.thumbnails)if(!t.key){entry=&t;break;}
 // A late worker result must never evict a thumbnail still on screen.
 if(!entry)for(auto &t:ui.thumbnails)if(!visible_thumbnail_key(t.key)&&(!entry||t.used<entry->used))entry=&t;
 if(!entry)return;
 if(image&&!entry->pixels){entry->pixels=static_cast<uint16_t*>(pixel_alloc(56*56*2));if(!entry->pixels)return;}
 lv_image_cache_drop(&entry->image);entry->key=k;entry->used=++ui.thumb_age;entry->checked=lv_tick_get();entry->missing=!image;
 if(image){
  size_t stride=image->header.stride?image->header.stride:size_t(image->header.w)*2;
  for(int y=0;y<56;y++){const auto *row=reinterpret_cast<const uint16_t*>(image->data+(y*image->header.h/56)*stride);for(int x=0;x<56;x++)entry->pixels[y*56+x]=row[x*image->header.w/56];}
  raw(entry->image,reinterpret_cast<const uint8_t*>(entry->pixels),56,56,112);
 }
 // Rebind aliases when an LRU entry changes owner. A cache contains the image
 // pixels, so refresh affected row snapshots even if their descriptor is reused.
 for(auto &r:ui.rows)if(r.initialized&&r.index<ui.tracks.size()){
  const auto *before=lv_image_get_src(r.image);const auto *after=thumbnail(ui.tracks[r.index].key);
  if(before!=after)lv_image_set_src(r.image,after);
  if(before!=after||after==&entry->image)cache_row(r);
 }
}
}
extern "C" void player_ui_clear_thumbnails(void) {
 if(!ui.root)return;
 for(auto &r:ui.rows)if(r.image){lv_image_set_src(r.image,&dragon_thumb);if(r.initialized)cache_row(r);}
 for(auto &t:ui.thumbnails){lv_image_cache_drop(&t.image);pixel_free(t.pixels);t=Thumbnail{};}
 ui.thumb_age=0;
}

extern "C" void player_ui_get_render_stats(player_ui_render_stats_t *stats) { if(stats) { *stats=ui.stats;ui.stats.interval_max_ms=0; } }
extern "C" lv_obj_t *player_ui_root(void) { return ui.root; }

extern "C" void player_ui_set_overlay(bool visible) {if(visible)keyboard_clear();ui.overlay=visible;ui.dragon_tick=lv_tick_get();}
extern "C" void player_ui_open_settings(void) {settings_page(0);settings_visible(true);}
extern "C" void player_ui_open_files(void) {if(!ui.root)return;close_menu();ui.state.library_tab=1;settings_visible(false);}
#include "aurora_folders.inc"
#include "aurora_lock.inc"
#include "aurora_keyboard.inc"

extern "C" void player_ui_set_sleeping(bool sleeping) {
 if(sleeping){keyboard_clear();}ui.sleeping=sleeping;ui.dragon_tick=lv_tick_get();ui.particle_tick=0;
 if(!ui.dragon_timer)return;
 if(sleeping){
  lv_timer_pause(ui.dragon_timer);lv_timer_pause(ui.notice_timer);
  if(!ui.saver){
   ui.saver=plain(lv_layer_top(),0,0,1280,720);solid(ui.saver,0);lv_obj_add_flag(ui.saver,LV_OBJ_FLAG_CLICKABLE);
  }
  lv_obj_remove_flag(ui.saver,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(ui.saver);
 }else{
  if(ui.saver){lv_obj_add_flag(ui.saver,LV_OBJ_FLAG_HIDDEN);}
  lv_timer_resume(ui.dragon_timer);if(lv_obj_is_visible(ui.notice))lv_timer_resume(ui.notice_timer);lv_obj_invalidate(ui.root);
 }
}
