#undef NDEBUG /* zig -O1/-O2 predefines NDEBUG; keep assert() checks active */
// Real LVGL widgets/rendering plus extracted production search dialog and
// physical-key event handler. Only hardware and the controller command queue
// are substituted; text insertion and dialog geometry are not mocked.
#define main existing_preview_main
#include "aurora_ui_preview.cpp"
#undef main
#include "aurora_fonts.h"
#include "keyboard_shortcuts.hpp"
#include <deque>
#include <climits>
struct Queued {int action,value;std::string text;};
static std::deque<Queued> queued;
static lv_obj_t *dialog=nullptr,*search_input=nullptr,*dialog_key_focus=nullptr;
static lv_obj_t *bt_status=nullptr,*bt_list=nullptr,*power_message=nullptr;
static int dialog_kind=0;static bool dialog_closable=true;
static void enqueue(int a,int v=0){queued.push_back({a,v,{}});}
static void enqueue_text(int a,const char *text){queued.push_back({a,0,text?text:""});}
static int tab5_display_sleep(bool,unsigned){return ESP_OK;}
static void audio_engine_set_visuals_enabled(bool){}
// close_dialog() clears the EQ/reverb input ring under its spinlock; the
// fixture is single-threaded, so the critical section only checks pairing.
static int eq_input_mux,eq_critical_depth=0;
static unsigned eq_input_read=0,eq_input_count=0;
static void portENTER_CRITICAL(int *mux){assert(mux==&eq_input_mux&&!eq_critical_depth);++eq_critical_depth;}
static void portEXIT_CRITICAL(int *mux){assert(mux==&eq_input_mux&&eq_critical_depth==1);--eq_critical_depth;}
// The fixture never sleeps the display (dimmed stays false), so the wake path
// may only disarm the motion tap; the screen lock is disabled and never active.
namespace player {static void sleep_tap_arm(bool enabled){assert(!enabled);}}
#include "search_ui_globals.h"
struct SearchPlayer {
 struct {bool locked() const {return false;}} screen_lock;
 bool lock_active() const {return false;}
 void lock_show_pin_ui(){assert(false);}
 bool shutdown_requested=false,dimmed=false,keyboard_wake_hold=false;
 unsigned last_state=123;
 unsigned keyboard_trace_remaining=0;
 struct {uint8_t brightness=70,volume=60,bt_volume=80,library_tab=0;bool animations=false,bt_enabled=false,local_output=true;} settings;
 struct {uint32_t position_ms=10000;} audio;
 std::string search_query;
#include "search_ui_methods.h"
} search_player;
static void fixture_action(player_ui_action_t a,int32_t v,void*){enqueue(a,v);}
static void drain(){
 while(!queued.empty()){
  auto c=queued.front();queued.pop_front();
  switch(c.action){
   case PLAYER_UI_SEARCH:search_player.search_dialog();break;
   case CLOSE:close_dialog();break;
   case SEARCH_APPLY:search_player.search_query=c.text;close_dialog();break;
   case SEARCH_CLEAR:search_player.search_query.clear();close_dialog();break;
   default:printf("Unexpected command %d\n",c.action);assert(false);
  }
 }
 step();
}
static void physical(uint32_t key,uint8_t modifiers=0,bool repeat=false){
 tab5_keyboard_event_t e{key,modifiers,repeat};search_player.keyboard_event(e);step();
}
static void typed(const char *s){for(;*s;++s)physical(static_cast<unsigned char>(*s));}
static std::vector<uint32_t> field_pixels(){
 assert(search_input);lv_area_t a;lv_obj_get_coords(search_input,&a);std::vector<uint32_t> out;
 for(int y=a.y1;y<=a.y2;y++)for(int x=a.x1;x<=a.x2;x++)out.push_back(pixels[y*1280+x]);return out;
}
static void check_text(const char *expected){
 assert(dialog_kind==DIALOG_SEARCH&&search_input&&dialog);
 assert(!strcmp(lv_textarea_get_text(search_input),expected));
 assert(!strcmp(lv_label_get_text(lv_textarea_get_label(search_input)),expected));
 assert(lv_obj_is_visible(search_input)&&lv_obj_is_visible(lv_textarea_get_label(search_input)));
 lv_area_t area;lv_obj_get_coords(search_input,&area);assert(area.x1>=0&&area.x2<1280&&area.y1>=0&&area.y2<720);
 // Draining and full redraw must retain exactly the same entered text.
 lv_refr_now(lv_display_get_default());auto before=std::vector<uint32_t>(pixels,pixels+1280*720);
 lv_obj_invalidate(lv_screen_active());lv_refr_now(lv_display_get_default());assert(std::equal(before.begin(),before.end(),pixels));
}
int main(int argc,char **argv){
 setvbuf(stdout,nullptr,_IONBF,0);target_rgb565=true;std::string dir=argc>1?argv[1]:".";
 lv_init();auto*d=lv_display_create(1280,720);lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);lv_display_set_buffers(d,buffer,nullptr,sizeof(buffer)/2,LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(d,flush);
 auto *pointer_input=lv_indev_create();lv_indev_set_type(pointer_input,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(pointer_input,read_pointer);
 player_ui_create(lv_screen_active(),fixture_action,nullptr);state.title="Aurora Protocol";state.artist="Hikari Collective";state.animations=false;state.brightness=70;state.clock24=true;state.clock_text="14:28";state.firmware="1.3.10";player_ui_set_state(&state);step();
 // Open the real large dialog via Ctrl+F, as on the physical keyboard.
 assert(player_ui_keyboard_text_allowed());physical('f',TAB5_KEY_MOD_CTRL);assert(dialog&&search_input);auto empty=field_pixels();
 typed("Aurora 123");check_text("Aurora 123");assert(field_pixels()!=empty);assert(queued.empty());save(dir+"/search-main-physical.ppm");
 physical(TAB5_KEY_LEFT);physical(TAB5_KEY_BACKSPACE);check_text("Aurora 13");physical(TAB5_KEY_HOME);physical(TAB5_KEY_DELETE);check_text("urora 13");physical(TAB5_KEY_END);typed(" !");check_text("urora 13 !");
 physical('u',TAB5_KEY_MOD_CTRL);check_text("");typed("Midnight Circuit");physical(TAB5_KEY_ENTER);assert(queued.size()==1&&queued.front().action==SEARCH_APPLY&&queued.front().text=="Midnight Circuit");drain();assert(!dialog&&!search_input);
 // Settings stays open underneath its own Search modal. Input must take the
 // SEARCH branch before the settings/overlay shortcut guards.
 player_ui_open_settings();step();tap_text("Library");step();tap_text("Search");assert(queued.size()==1&&queued.front().action==PLAYER_UI_SEARCH);drain();assert(!player_ui_keyboard_text_allowed());
 check_text("Midnight Circuit");physical('u',TAB5_KEY_MOD_CTRL);empty=field_pixels();typed("A Little Bit - LET IT DIE");check_text("A Little Bit - LET IT DIE");assert(field_pixels()!=empty);save(dir+"/search-settings-physical.ppm");
 // Touching the on-screen keyboard and then physical typing uses one textarea.
 auto *keyboard=static_cast<lv_obj_t*>(nullptr);for(uint32_t i=0;i<lv_obj_get_child_count(dialog);i++){auto *o=lv_obj_get_child(dialog,i);if(lv_obj_has_class(o,&lv_keyboard_class))keyboard=o;}assert(keyboard);
 auto *matrix=(lv_buttonmatrix_t*)keyboard;lv_area_t ka;lv_obj_get_coords(keyboard,&ka);bool hit=false;
 for(unsigned i=0;i<matrix->btn_cnt;i++){const char *t=lv_buttonmatrix_get_button_text(keyboard,i);if(t&&!strcmp(t,"a")){auto a=matrix->button_areas[i];tap(ka.x1+(a.x1+a.x2)/2,ka.y1+(a.y1+a.y2)/2);hit=true;break;}}assert(hit);
 typed("bc");check_text("A Little Bit - LET IT DIEabc");assert(queued.empty());physical(TAB5_KEY_ESC);drain();assert(!dialog&&!search_input&&!player_ui_keyboard_text_allowed());
 assert(player_ui_keyboard(LV_KEY_ESC));assert(player_ui_keyboard(LV_KEY_ESC));assert(player_ui_keyboard_text_allowed());
 // Direct type-to-search starts a new real textarea and subsequent keys append.
 physical('x');typed("yz");check_text("xyz");physical('u',TAB5_KEY_MOD_CTRL);for(int i=0;i<110;i++)physical('a');assert(strlen(lv_textarea_get_text(search_input))==96);physical(TAB5_KEY_ESC);drain();
 puts("PASS: production SEARCH MUSIC/new_dialog and keyboard_event on real LVGL: Ctrl+F, settings action, visible glyph rendering, edits, submit, Escape, touch+physical typing, type-to-search and 96-character limit.");
 lv_obj_delete(player_ui_root());return 0;
}
