#ifdef NDEBUG
#undef NDEBUG
#endif
#include "keyboard_shortcuts.hpp"
#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

// Small host UI model: production chooses routing; these mocks only record
// LVGL edits/actions and enforce display-lock/lifetime rules.
enum { LV_KEY_UP=17,LV_KEY_DOWN,LV_KEY_LEFT,LV_KEY_RIGHT,LV_KEY_HOME,LV_KEY_END,
       LV_KEY_ENTER,LV_KEY_ESC,LV_KEY_NEXT,LV_KEY_PREV,LV_KEY_BACKSPACE,
       PLAYER_UI_KEY_PAGE_UP=0x10001,PLAYER_UI_KEY_PAGE_DOWN,
       LV_OBJ_FLAG_HIDDEN=1,LV_STATE_DISABLED=2,LV_STATE_FOCUS_KEY=4,
       LV_ANIM_OFF=0,LV_EVENT_CLICKED=1,LV_EVENT_KEY,LV_EVENT_VALUE_CHANGED };
constexpr uint32_t LV_TEXTAREA_CURSOR_LAST=UINT32_MAX;
static int lv_button_class,lv_spinbox_class,lv_slider_class,lv_dropdown_class;
struct lv_obj_t {
    std::vector<lv_obj_t*> children;
    std::string text;
    size_t cursor=0;
    unsigned flags=0,states=0,clicked=0;
    int *kind=nullptr,value=0;
    bool alive=true,open=false;
};
struct Test {
    bool locked=false,lock_fail=false,connected=true,text_allowed=true;
    bool sleeping=false,wake_fail=false,visuals=false,ui_consumes=false,overlay=false;
    unsigned activity=0,wakes=0,read_calls=0,searches=0,ui_calls=0,tap_disarms=0,pin_shows=0;
    unsigned lock_calls=0,last_lock_wait=0,lock_delay=0,trace_lines=0;
    uint64_t now=100;
    bool disconnect_on_lock=false;
    uint32_t last_ui_key=0;
    std::deque<tab5_keyboard_event_t> input;
    struct Queued {int action,value;std::string text;};
    std::vector<Queued> queued;
} t;
namespace player {void sleep_tap_arm(bool enabled){assert(!enabled);++t.tap_disarms;}}
static lv_obj_t panel,input,control1,control2,container;
static lv_obj_t *dialog=nullptr,*search_input=nullptr,*dialog_key_focus=nullptr;
static lv_obj_t *bt_status=nullptr,*bt_list=nullptr,*power_message=nullptr;
static int dialog_kind=0;
static bool dialog_closable=true;
static int eq_input_mux;
static unsigned eq_input_read=0,eq_input_count=0;
static void portENTER_CRITICAL(int *mux){assert(mux==&eq_input_mux&&t.locked);}
static void portEXIT_CRITICAL(int *mux){assert(mux==&eq_input_mux&&t.locked);}
static const char *TAG="keyboard-test";
static uint64_t now_ms(){return t.now;}
template<class... Args> static void log_context(Args...){assert(!t.locked);++t.trace_lines;}
#define ESP_LOGI(...) log_context(__VA_ARGS__)
static void check(lv_obj_t *o){assert(t.locked&&o&&o->alive);}
static bool tab5_display_lock(unsigned wait){assert(!t.locked);++t.lock_calls;t.last_lock_wait=wait;if(t.lock_fail){t.now+=wait;return false;}if(t.lock_delay>wait){t.lock_delay-=wait;t.now+=wait;return false;}t.now+=t.lock_delay;t.lock_delay=0;t.locked=true;if(t.disconnect_on_lock)t.connected=false;return true;}
static void tab5_display_unlock(){assert(t.locked);t.locked=false;}
static int lv_display_get_default(){assert(t.locked);return 1;}
static void lv_display_trigger_activity(int){assert(t.locked);++t.activity;}
static void player_ui_set_sleeping(bool v){assert(t.locked);t.sleeping=v;}
static int tab5_display_sleep(bool sleep,unsigned){assert(t.locked&&!sleep);++t.wakes;if(t.wake_fail)return ESP_FAIL;t.sleeping=false;return ESP_OK;}
static void audio_engine_set_visuals_enabled(bool v){t.visuals=v;}
extern "C" bool tab5_keyboard_read_event(tab5_keyboard_event_t *e){assert(!t.locked);++t.read_calls;if(!t.connected||t.input.empty())return false;*e=t.input.front();t.input.pop_front();return true;}
extern "C" bool tab5_keyboard_connected(){return t.connected;}
static bool player_ui_keyboard(uint32_t key){assert(t.locked);++t.ui_calls;t.last_ui_key=key;return t.ui_consumes;}
static bool player_ui_keyboard_text_allowed(){assert(t.locked);return t.text_allowed;}
static void enqueue(int a,int v=0){assert(t.locked);t.queued.push_back({a,v,{}});}
static void enqueue_text(int a,const char *s){assert(t.locked);t.queued.push_back({a,0,s?s:""});}
static void lv_textarea_set_text(lv_obj_t *o,const char *s){check(o);o->text=s;o->cursor=o->text.size();}
static const char *lv_textarea_get_text(lv_obj_t *o){check(o);return o->text.c_str();}
static void lv_textarea_add_text(lv_obj_t *o,const char *s){check(o);if(o->text.size()+std::char_traits<char>::length(s)<=96){o->text.insert(o->cursor,s);o->cursor+=std::char_traits<char>::length(s);}}
static void lv_textarea_delete_char(lv_obj_t *o){check(o);if(o->cursor)o->text.erase(--o->cursor,1);}
static void lv_textarea_delete_char_forward(lv_obj_t *o){check(o);if(o->cursor<o->text.size())o->text.erase(o->cursor,1);}
static void lv_textarea_cursor_left(lv_obj_t *o){check(o);if(o->cursor)--o->cursor;}
static void lv_textarea_cursor_right(lv_obj_t *o){check(o);if(o->cursor<o->text.size())++o->cursor;}
static void lv_textarea_set_cursor_pos(lv_obj_t *o,uint32_t at){check(o);o->cursor=std::min<size_t>(at,o->text.size());}
static bool lv_obj_has_flag(lv_obj_t *o,unsigned v){check(o);return (o->flags&v)!=0;}
static bool lv_obj_has_state(lv_obj_t *o,unsigned v){check(o);return (o->states&v)!=0;}
static bool lv_obj_has_class(lv_obj_t *o,int *c){check(o);return o->kind==c;}
static uint32_t lv_obj_get_child_count(lv_obj_t *o){check(o);return uint32_t(o->children.size());}
static lv_obj_t *lv_obj_get_child(lv_obj_t *o,uint32_t n){check(o);return o->children.at(n);}
static void lv_obj_remove_state(lv_obj_t *o,unsigned v){check(o);o->states&=~v;}
static void lv_obj_add_state(lv_obj_t *o,unsigned v){check(o);o->states|=v;}
static unsigned lv_color_hex(unsigned v){return v;}
static void lv_obj_set_style_outline_color(lv_obj_t *o,unsigned,unsigned){check(o);}
static void lv_obj_set_style_outline_width(lv_obj_t *o,unsigned,unsigned){check(o);}
static void lv_obj_scroll_to_view(lv_obj_t *o,int){check(o);}
static void lv_obj_send_event(lv_obj_t *o,int e,void *arg){
    check(o);
    if(e==LV_EVENT_CLICKED){++o->clicked;return;}
    if(e==LV_EVENT_VALUE_CHANGED)return;
    assert(e==LV_EVENT_KEY&&arg&&o->kind==&lv_dropdown_class);
    const auto key=*static_cast<uint32_t*>(arg);
    if(key==LV_KEY_ESC)o->open=false;
    else if(key==LV_KEY_ENTER)o->open=!o->open;
    else if(key==LV_KEY_DOWN||key==LV_KEY_RIGHT)++o->value;
    else if((key==LV_KEY_UP||key==LV_KEY_LEFT)&&o->value>0)--o->value;
}
static bool lv_dropdown_is_open(lv_obj_t *o){check(o);return o->open;}
static uint32_t lv_dropdown_get_selected(lv_obj_t *o){check(o);return uint32_t(o->value);}
static void lv_dropdown_set_selected(lv_obj_t *o,uint32_t value){check(o);o->value=int(value);}
static int lv_slider_get_value(lv_obj_t *o){check(o);return o->value;}
static void lv_slider_set_value(lv_obj_t *o,int value,int){check(o);o->value=value;}
static void lv_spinbox_increment(lv_obj_t *o){check(o);++o->value;}
static void lv_spinbox_decrement(lv_obj_t *o){check(o);--o->value;}
static void lv_obj_delete(lv_obj_t *o){check(o);o->alive=false;for(auto *child:o->children)child->alive=false;}
static void player_ui_set_overlay(bool v){assert(t.locked);t.overlay=v;}
#include "keyboard_globals.h"
struct Player {
    bool shutdown_requested=false,dimmed=false,keyboard_wake_hold=false;
    bool keyboard_event_pending=false;
    tab5_keyboard_event_t keyboard_pending{};
    uint64_t keyboard_pending_since=0;
    unsigned keyboard_trace_remaining=0;
    unsigned last_state=123;
    struct {bool active=false;bool locked()const{return active;}} screen_lock;
    bool lock_active()const{return screen_lock.locked();}
    void lock_show_pin_ui(){assert(t.locked&&screen_lock.locked());++t.pin_shows;}
    struct {uint8_t brightness=70,volume=60,bt_volume=80,library_tab=0;bool animations=true,bt_enabled=false,local_output=true;} settings;
    struct {uint32_t position_ms=10000;} audio;
    std::string search_query="old filter";
    void search_dialog(){assert(t.locked);++t.searches;panel={};input={};panel.children={&input};dialog=&panel;search_input=&input;dialog_kind=DIALOG_SEARCH;keyboard_trace_remaining=8;t.overlay=true;lv_textarea_set_text(&input,search_query.c_str());}
#include "keyboard_production.h"
};
static Player reset(){
    t={};panel={};input={};control1={};control2={};container={};
    dialog=search_input=dialog_key_focus=bt_status=bt_list=power_message=nullptr;
    dialog_kind=DIALOG_NONE;dialog_closable=true;
    return Player{};
}
static void key(Player &p,uint32_t k,uint8_t modifiers=0,bool repeat=false){
    assert(tab5_display_lock(2));tab5_keyboard_event_t e{k,modifiers,repeat};p.keyboard_event(e);tab5_display_unlock();
}
static void closed(){assert(tab5_display_lock(2));close_dialog();tab5_display_unlock();}
static void command(int action,int value=0){assert(t.queued.size()==1&&t.queued.back().action==action&&t.queued.back().value==value);t.queued.clear();}
static void modal(int kind=DIALOG_OTHER){panel={};control1={};control2={};control1.kind=&lv_button_class;control2.kind=&lv_spinbox_class;panel.children={&control1,&control2};dialog=&panel;dialog_kind=kind;dialog_key_focus=nullptr;}
int main(){
    auto p=reset();key(p,'a');assert(t.searches==1&&input.text=="a"&&t.queued.empty());
    key(p,'b');key(p,' ');key(p,'C',TAB5_KEY_MOD_SHIFT);assert(input.text=="ab C");
    key(p,'d',0,true);key(p,TAB5_KEY_LEFT);key(p,TAB5_KEY_BACKSPACE);assert(input.text=="ab d");
    key(p,TAB5_KEY_HOME);key(p,TAB5_KEY_DELETE);key(p,TAB5_KEY_END);assert(input.text=="b d");
    for(auto k:{uint32_t(' '),uint32_t('f'),uint32_t(','),uint32_t(TAB5_KEY_LEFT),uint32_t(TAB5_KEY_RIGHT)})key(p,k,TAB5_KEY_MOD_CTRL);
    key(p,'X',TAB5_KEY_MOD_ALT);assert(input.text=="b d"&&t.queued.empty());
    key(p,TAB5_KEY_ENTER,0,true);assert(t.queued.empty());
    key(p,TAB5_KEY_ENTER);assert(t.queued.size()==1&&t.queued[0].action==SEARCH_APPLY&&t.queued[0].text=="b d");
    t.queued.clear();closed();assert(!dialog&&!search_input&&!dialog_key_focus&&!t.overlay);
    key(p,'f',TAB5_KEY_MOD_CTRL);assert(input.text=="old filter");key(p,'u',TAB5_KEY_MOD_CTRL);assert(input.text.empty());
    key(p,'A',TAB5_KEY_MOD_SHIFT);key(p,TAB5_KEY_ESC);command(CLOSE);closed();
    puts("PASS extracted search routing: typing/spaces/case/repeat/cursor/delete, modifiers blocked, Ctrl+F and Ctrl+U, Enter captures query, Escape and pointer cleanup.");

    p=reset();key(p,' ');command(PLAYER_UI_PLAY_PAUSE);key(p,' ',0,true);assert(t.queued.empty());
    key(p,TAB5_KEY_LEFT,TAB5_KEY_MOD_CTRL);command(PLAYER_UI_PREVIOUS);
    key(p,TAB5_KEY_RIGHT,TAB5_KEY_MOD_CTRL);command(PLAYER_UI_NEXT);
    key(p,TAB5_KEY_RIGHT,TAB5_KEY_MOD_CTRL,true);assert(t.queued.empty());
    p.settings.volume=98;key(p,TAB5_KEY_UP,TAB5_KEY_MOD_CTRL);command(PLAYER_UI_VOLUME,100);
    p.settings.volume=2;key(p,TAB5_KEY_DOWN,TAB5_KEY_MOD_CTRL);command(PLAYER_UI_VOLUME,0);
    p.settings.bt_enabled=true;p.settings.local_output=false;key(p,TAB5_KEY_DOWN,TAB5_KEY_MOD_CTRL);command(PLAYER_UI_VOLUME,75);
    p.audio.position_ms=1000;key(p,TAB5_KEY_LEFT);command(PLAYER_UI_SEEK_MS,0);
    p.audio.position_ms=UINT32_MAX;key(p,TAB5_KEY_RIGHT);command(PLAYER_UI_SEEK_MS,INT32_MAX);
    p.settings.library_tab=2;key(p,TAB5_KEY_TAB);command(PLAYER_UI_LIBRARY_TAB,0);
    p.settings.library_tab=0;key(p,TAB5_KEY_TAB,TAB5_KEY_MOD_SHIFT);command(PLAYER_UI_LIBRARY_TAB,2);
    key(p,',',TAB5_KEY_MOD_CTRL);command(PLAYER_UI_SETTINGS);key(p,'b',TAB5_KEY_MOD_CTRL);command(PLAYER_UI_BLUETOOTH);key(p,'m',TAB5_KEY_MOD_CTRL);command(PLAYER_UI_MUTE);
    key(p,' ',TAB5_KEY_MOD_ALT);assert(t.queued.empty());
    t.ui_consumes=true;key(p,TAB5_KEY_RIGHT);key(p,TAB5_KEY_ENTER);assert(t.queued.empty());
    t.text_allowed=false;t.ui_consumes=false;key(p,' ');key(p,'f',TAB5_KEY_MOD_CTRL);key(p,TAB5_KEY_UP,TAB5_KEY_MOD_CTRL);key(p,'x');assert(t.queued.empty()&&!dialog);
    puts("PASS extracted main routing: bounded seek/volume, BT/local volume, nonrepeating playback, tab wrap, focused UI/settings isolate global shortcuts.");

    p=reset();modal(DIALOG_BT);key(p,' ');key(p,'x');key(p,'f',TAB5_KEY_MOD_CTRL);assert(t.queued.empty()&&!search_input);
    key(p,TAB5_KEY_ENTER);assert(dialog_key_focus==&control1&&control1.clicked==0);
    key(p,TAB5_KEY_ENTER,0,true);assert(control1.clicked==0);key(p,TAB5_KEY_ENTER);assert(control1.clicked==1);
    key(p,TAB5_KEY_TAB);assert(dialog_key_focus==&control2);key(p,TAB5_KEY_RIGHT);assert(control2.value==1);key(p,TAB5_KEY_LEFT);assert(control2.value==0);
    dialog_closable=false;key(p,TAB5_KEY_ESC);assert(t.queued.empty());dialog_closable=true;key(p,TAB5_KEY_ESC);command(CLOSE);closed();
    // Disabled/hidden controls never receive focus, including nested BT rows.
    modal();control1.flags=LV_OBJ_FLAG_HIDDEN;key(p,TAB5_KEY_TAB);assert(dialog_key_focus==&control2);
    control2.states=LV_STATE_DISABLED;dialog_key_focus=nullptr;key(p,TAB5_KEY_ENTER);assert(!dialog_key_focus);
    puts("PASS extracted modal routing: no typing/media fall-through, first Enter reveals focus, guarded repeated activation, spinbox adjustment, closable Escape, disabled/hidden controls skipped.");

    p=reset();p.dimmed=true;t.sleeping=true;key(p,' ');assert(!p.dimmed&&p.keyboard_wake_hold&&t.wakes==1&&t.queued.empty()&&t.tap_disarms==1);
    key(p,' ',0,true);key(p,' ',0,true);assert(t.queued.empty());key(p,' ');command(PLAYER_UI_PLAY_PAUSE);
    p=reset();p.dimmed=true;t.wake_fail=true;key(p,'a');key(p,'a',0,true);assert(p.dimmed&&t.sleeping&&t.searches==0);t.wake_fail=false;key(p,'a',0,true);assert(!p.dimmed&&t.searches==0);key(p,'a',0,true);assert(t.searches==0);key(p,'b');assert(input.text=="b");
    p=reset();p.shutdown_requested=true;p.dimmed=true;key(p,' ');key(p,TAB5_KEY_ESC);assert(t.activity==0&&t.wakes==0&&t.queued.empty());
    p=reset();p.dimmed=true;p.screen_lock.active=true;t.sleeping=true;key(p,' ');
    assert(!p.dimmed&&p.screen_lock.locked()&&p.keyboard_wake_hold&&t.pin_shows==1&&t.tap_disarms==1&&t.queued.empty()&&!t.visuals);
    key(p,'1',0,true);assert(t.ui_calls==0);key(p,'1');assert(t.ui_calls==1&&t.last_ui_key=='1'&&t.queued.empty());
    key(p,' ',0,false);assert(t.queued.empty()&&p.screen_lock.locked());
    puts("PASS extracted power guard: wake disarms taps, press and held repeats consumed, PIN remains locked and receives digits, failed wake retries safely, shutdown ignores all keyboard input.");

    p=reset();t.input={{'a',0,false},{'b',0,false},{TAB5_KEY_ENTER,0,false},{' ',0,false}};
    p.poll_keyboard();assert(t.input.size()==3&&input.text=="a");p.poll_keyboard();assert(t.input.size()==2&&input.text=="ab");p.poll_keyboard();assert(t.input.size()==1&&t.queued.size()==1&&t.queued[0].action==SEARCH_APPLY);
    t.queued.clear();closed();p.poll_keyboard();command(PLAYER_UI_PLAY_PAUSE);assert(t.input.empty()&&!t.locked);
    const auto idle_locks=t.lock_calls;p.poll_keyboard();assert(t.lock_calls==idle_locks&&!p.keyboard_event_pending);
    t.connected=false;p.poll_keyboard();assert(t.lock_calls==idle_locks);
    puts("PASS extracted poll ordering: one event per loop, Enter command processed before next key, no display lock for idle or disconnected keyboard.");

    // A big modal refresh can own LVGL for much more than the old 2ms poll.
    // Fetch once outside the lock, wait100ms only with work, retain until ready.
    p=reset();t.input={{'a',0,false},{'b',0,false}};t.lock_delay=90;p.poll_keyboard();assert(input.text=="a"&&t.last_lock_wait==100&&!p.keyboard_event_pending&&t.input.size()==1);
    t.lock_delay=250;const auto reads=t.read_calls;p.poll_keyboard();assert(p.keyboard_event_pending&&input.text=="a"&&t.input.empty()&&t.read_calls==reads+1);
    p.poll_keyboard();assert(p.keyboard_event_pending&&input.text=="a"&&t.read_calls==reads+1);
    p.poll_keyboard();assert(!p.keyboard_event_pending&&input.text=="ab"&&t.read_calls==reads+1&&!t.locked);
    // Pending keys are not replayed after unplug, nor after a long UI stall.
    p=reset();t.input={{'x',0,false}};t.lock_fail=true;p.poll_keyboard();assert(p.keyboard_event_pending&&t.input.empty());t.connected=false;t.lock_fail=false;p.poll_keyboard();assert(!p.keyboard_event_pending&&!dialog);t.connected=true;p.poll_keyboard();assert(!dialog);
    p=reset();t.input={{'x',0,false}};t.disconnect_on_lock=true;p.poll_keyboard();assert(!p.keyboard_event_pending&&!dialog&&!t.locked);
    p=reset();t.input={{'x',0,false}};t.lock_fail=true;p.poll_keyboard();assert(p.keyboard_event_pending);t.now=p.keyboard_pending_since+2000;t.lock_fail=false;p.poll_keyboard();assert(!p.keyboard_event_pending&&!dialog&&!t.locked);t.input={{'b',0,false}};p.poll_keyboard();assert(input.text=="b");
    puts("PASS extracted busy-display regression:90ms render accepted,250ms retains a single event across retries, no double read, pending input discarded after disconnect/race or2s deadline.");
    // Diagnostics are bounded per search opening and occur after unlock.
    p=reset();for(unsigned i=0;i<12;i++){t.input.push_back({'a',0,false});p.poll_keyboard();}assert(t.trace_lines==8&&p.keyboard_trace_remaining==0&&input.text==std::string(12,'a'));
    puts("PASS extracted search diagnostics: at most8 fresh events per opening, outside UI lock. Hardware/I2C timing not simulated here.");
}
