#undef NDEBUG
#include "aurora_airpods.inc"
#include <cassert>
#include <string>
#include <vector>
static uint32_t pixels[1280*720];static uint16_t buffer[1280*80];static uint64_t flushed=0;
static lv_point_t pointer{};static bool pressed=false;static lv_obj_t *panel{};
static std::vector<int> actions,values;
static void flush(lv_display_t *display,const lv_area_t *area,uint8_t *data){
 auto *p=(uint16_t*)data;for(int y=area->y1;y<=area->y2;y++)for(int x=area->x1;x<=area->x2;x++){const auto v=*p++;pixels[y*1280+x]=((uint32_t(v>>11)*255/31)<<16)|((uint32_t((v>>5)&63)*255/63)<<8)|(uint32_t(v&31)*255/31);++flushed;}lv_display_flush_ready(display);
}
static void read_pointer(lv_indev_t*,lv_indev_data_t *data){data->point=pointer;data->state=pressed?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;}
static void step(int count=3){for(int i=0;i<count;i++){lv_tick_inc(25);lv_timer_handler();}}
static void tap(lv_obj_t *object){lv_area_t area;lv_obj_get_coords(object,&area);assert(lv_area_get_width(&area)>=44&&lv_area_get_height(&area)>=44);pointer={(area.x1+area.x2)/2,(area.y1+area.y2)/2};pressed=true;step(4);pressed=false;step(3);}
static void action(int kind,int value){actions.push_back(kind);values.push_back(value);}
static void save(const std::string &path){FILE *file=fopen(path.c_str(),"wb");assert(file);fprintf(file,"P6\n1280 720\n255\n");for(auto pixel:pixels){const uint8_t bytes[]={uint8_t(pixel>>16),uint8_t(pixel>>8),uint8_t(pixel)};assert(fwrite(bytes,1,3,file)==3);}fclose(file);}
static bool selected(lv_obj_t *object){lv_style_value_t value{};assert(lv_obj_get_local_style_prop(object,LV_STYLE_BG_COLOR,&value,0)==LV_STYLE_RES_FOUND);return lv_color_eq(value.color,lv_color_hex(aurora_airpods_detail::lime));}
static void all_modes_disabled(aurora_airpods_detail::State *s){for(auto *mode:s->modes)assert(lv_obj_has_state(mode,LV_STATE_DISABLED));}
int main(int argc,char **argv){
 assert(argc==2);lv_init();auto *display=lv_display_create(1280,720);lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);lv_display_set_buffers(display,buffer,nullptr,sizeof buffer,LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(display,flush);
 auto *input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(input,read_pointer);
 lv_obj_set_style_bg_color(lv_screen_active(),lv_color_hex(0x101310),0);panel=lv_obj_create(lv_screen_active());lv_obj_set_style_bg_color(panel,lv_color_hex(0x141615),0);
 lv_obj_set_style_border_width(panel,1,0);lv_obj_set_style_border_color(panel,lv_color_hex(0x788177),0);auto *title=lv_label_create(panel);lv_label_set_text(title,"AIRPODS");lv_obj_set_style_text_color(title,lv_color_hex(0xeeefe8),0);
 AirPodsDialogState snapshot;airpods_dialog_build(panel,snapshot,action);step();auto *s=aurora_airpods_detail::state(panel);assert(s&&s->bindings_used==7);
 lv_area_t box;lv_obj_get_coords(panel,&box);assert(box.x1==170&&box.y1==80&&box.x2==1109&&box.y2==639);
 all_modes_disabled(s);assert(lv_obj_has_state(s->connect,LV_STATE_DISABLED));assert(!strcmp(lv_label_get_text(s->autopause_label),"Auto pause: --"));
 tap(s->modes[1]);lv_obj_send_event(s->modes[1],LV_EVENT_CLICKED,nullptr);assert(actions.empty());save(std::string(argv[1])+"/airpods-unavailable.ppm");
 snapshot.capable=true;airpods_dialog_refresh(panel,snapshot);step();all_modes_disabled(s);assert(lv_obj_has_state(s->connect,LV_STATE_DISABLED));
 snapshot.headphones_connected=true;airpods_dialog_refresh(panel,snapshot);step();assert(!lv_obj_has_state(s->connect,LV_STATE_DISABLED));tap(s->connect);assert(actions.back()==AIRPODS_CONNECT&&values.back()==0);
 assert(!strcmp(lv_label_get_text(s->channel),"Controls disconnected")); // request is not a successful connection
 snapshot.connecting=true;airpods_dialog_refresh(panel,snapshot);step();assert(lv_obj_has_state(s->connect,LV_STATE_DISABLED));all_modes_disabled(s);
 snapshot.connecting=false;snapshot.channel_connected=true;snapshot.auto_pause_known=true;snapshot.battery_left=0;snapshot.battery_right=87;snapshot.battery_case=-1;snapshot.ears=2;
 airpods_dialog_refresh(panel,snapshot);step();assert(!strcmp(lv_label_get_text(s->batteries[0]),"0%"));assert(!strcmp(lv_label_get_text(s->batteries[1]),"87%"));assert(!strcmp(lv_label_get_text(s->batteries[2]),"--"));
 for(unsigned i=0;i<4;++i){assert(!selected(s->modes[i]));assert(!lv_obj_has_state(s->modes[i],LV_STATE_DISABLED));tap(s->modes[i]);assert(actions.back()==AIRPODS_SET_MODE&&values.back()==int(i)+1);for(auto *mode:s->modes)assert(!selected(mode));}
 snapshot.mode=AIRPODS_MODE_ANC;airpods_dialog_refresh(panel,snapshot);step();assert(selected(s->modes[1]));
 tap(s->modes[3]);assert(selected(s->modes[1])&&!selected(s->modes[3]));
 snapshot.pending=true;airpods_dialog_refresh(panel,snapshot);step();all_modes_disabled(s);assert(selected(s->modes[1]));const size_t pending_calls=actions.size();tap(s->modes[3]);lv_obj_send_event(s->autopause,LV_EVENT_CLICKED,nullptr);assert(actions.size()==pending_calls);
 save(std::string(argv[1])+"/airpods-awaiting-confirmation.ppm");
 snapshot.pending=false;snapshot.status="The headphones did not confirm that change. Try again.";airpods_dialog_refresh(panel,snapshot);step();assert(selected(s->modes[1])&&!selected(s->modes[3]));
 snapshot.mode=AIRPODS_MODE_ADAPTIVE;snapshot.status=nullptr;snapshot.battery_left=64;snapshot.battery_case=99;airpods_dialog_refresh(panel,snapshot);step();assert(selected(s->modes[3]));
 tap(s->autopause);assert(actions.back()==AIRPODS_SET_AUTOPAUSE&&values.back()==1);assert(!strcmp(lv_label_get_text(s->autopause_label),"Auto pause: OFF"));
 snapshot.auto_pause=true;airpods_dialog_refresh(panel,snapshot);step();assert(!strcmp(lv_label_get_text(s->autopause_label),"Auto pause: ON"));save(std::string(argv[1])+"/airpods-connected.ppm");
 // All buttons remain inside the panel and mode columns have equal gaps.
 int last_right=0;for(unsigned i=0;i<4;++i){lv_area_t area;lv_obj_get_coords(s->modes[i],&area);assert(lv_area_get_width(&area)==207&&lv_area_get_height(&area)==64);if(i)assert(area.x1-last_right-1==16);last_right=area.x2;assert(area.x1>=box.x1+32&&area.x2<=box.x2-31);}
 lv_refr_now(display);auto baseline=flushed;for(unsigned i=0;i<20;i++)airpods_dialog_refresh(panel,snapshot);lv_refr_now(display);assert(flushed==baseline);
 const std::vector<uint32_t> before(pixels,pixels+1280*720);lv_obj_invalidate(lv_screen_active());lv_refr_now(display);assert(std::equal(before.begin(),before.end(),pixels));
 snapshot.battery_left=101;snapshot.battery_right=-2;snapshot.mode=99;airpods_dialog_refresh(panel,snapshot);step();assert(!strcmp(lv_label_get_text(s->batteries[0]),"--")&&!strcmp(lv_label_get_text(s->batteries[1]),"--"));for(auto *mode:s->modes)assert(!selected(mode));
 snapshot.channel_connected=false;airpods_dialog_refresh(panel,snapshot);step();for(auto *battery:s->batteries)assert(!strcmp(lv_label_get_text(battery),"--"));all_modes_disabled(s);
 snapshot.pending=true;airpods_dialog_refresh(panel,snapshot);step();auto *close=lv_obj_get_child(panel,lv_obj_get_child_count(panel)-2);tap(close);assert(actions.back()==AIRPODS_CLOSE);
 lv_obj_delete(panel);assert(lv_obj_get_child_count(lv_screen_active())==0);
 puts("PASS real LVGL AirPods modal: 940x560 layout; actual capability/channel gating; confirmed-only modes and auto-pause; pending/error states; 0%, unknown/invalid batteries; touch events/forged-event guards; symmetric large targets; zero unchanged redraw; clean teardown. Hardware protocol tested separately.");
}
