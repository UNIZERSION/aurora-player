#include "player_ui.h"
#include "search_keyboard.h"
#include "keyboard_shortcuts.hpp"
#include "screenshot_bmp.h"
#include "screen_lock_storage.h"
#include "sleep_tap_input.h"
#include "lock_wallpaper.h"
#include "aurora_startup.hpp"
#include "esp_heap_caps.h"
#include <unistd.h>
#include "ui_fonts.h"
#include "aurora_fonts.h"
#include "audio_engine.h"
#include "audio_level_policy.h"
#include "transport_policy.h"
#include "music_library.h"
#include "storage_layout.h"
#include "player_store.h"
#include "eq_store.h"
#include "reverb_store.h"
#include "aurora_reverb.inc"
#include "aurora_airpods.inc"
#include "airpods_playback.hpp"
#include "eq_presets.h"
#include "aurora_eq_dialog.hpp"
#include "favorites_store.h"
#include "tab5_hal.h"
#include "bluetooth_bridge.h"
#include "player_artwork.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <algorithm>
#include <atomic>
#include <new>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>
#include <sys/time.h>

#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3
static_assert(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ <= 360,
              "ESP-IDF 5.5.2 pre-v3 ESP32-P4 clock configuration must not exceed 360 MHz");
#endif

namespace {
constexpr const char *MUSIC = "/sdcard/Music";
constexpr const char *DATA = "/sdcard/Aurora/Settings";
constexpr const char *TAG = "orbital";
static const char *format_name(audio_format_t format) {
    switch (format) {
    case AUDIO_FORMAT_MP3: return "MP3";
    case AUDIO_FORMAT_WAV: return "WAV";
    case AUDIO_FORMAT_FLAC: return "FLAC";
    case AUDIO_FORMAT_AAC: return "AAC";
    default: return "--";
    }
}
enum ExtraAction { CLOSE = 100, FOLDER_SELECT, BRIGHTNESS, DIM_TIME, RESCAN, SAVE, CLOCK_SET,
    SEARCH_APPLY, SEARCH_CLEAR, BT_ENABLE, BT_VOLUME, BT_SCAN, BT_RECONNECT, BT_SELECT, BT_TEST, COVERS,
    POWER_OFF, POWER_RETRY, RESTART, EQ_ACTION, AIRPODS_OPEN, AIRPODS_ACTION };
enum DialogKind { DIALOG_NONE, DIALOG_OTHER, DIALOG_BT, DIALOG_SEARCH, DIALOG_EQ, DIALOG_REVERB, DIALOG_AIRPODS };
struct Command { int action, value, fields[5]; char text[112]; };
QueueHandle_t commands;
lv_obj_t *dialog = nullptr;
lv_obj_t *search_input = nullptr, *dialog_key_focus = nullptr;
bool dialog_closable = true;
lv_obj_t *date_fields[5]{};
DialogKind dialog_kind = DIALOG_NONE;
lv_obj_t *bt_status = nullptr, *bt_list = nullptr, *bt_airpods=nullptr;
lv_obj_t *power_message = nullptr;
std::atomic_bool artwork_io_allowed{false};
std::atomic_bool lock_input_gate{false};
static uint64_t now_ms() { return static_cast<uint64_t>(esp_timer_get_time()) / 1000; }
static void enqueue(int action, int value = 0) {
    Command c{}; c.action = action; c.value = value;
    if (xQueueSend(commands, &c, 0) != pdTRUE) player_ui_set_notice("Busy - please try again");
}
static void enqueue_text(int action, const char *text) {
    Command c{}; c.action = action; snprintf(c.text, sizeof c.text, "%s", text ? text : "");
    if (xQueueSend(commands, &c, 0) != pdTRUE) player_ui_set_notice("Busy - please try again");
}
static std::atomic_int latest_volume{-1};
// Consecutive drag updates coalesce, but buttons are ordering barriers.
// The tiny critical section only copies POD; never LVGL, storage or audio work.
struct EqInput {int kind,index,value;};
static EqInput eq_inputs[32]{};
static unsigned eq_input_read=0,eq_input_count=0;
static portMUX_TYPE eq_input_mux=portMUX_INITIALIZER_UNLOCKED;
static void eq_ui_action(int kind,int index,int value) {
    if(lock_input_gate.load(std::memory_order_acquire))return;
    bool accepted=false;
    portENTER_CRITICAL(&eq_input_mux);
    if(eq_input_count && (kind==EQ_BAND || kind==EQ_PREAMP || (kind>=100&&kind<=100+REVERB_LOW_CUT))){
        auto &last=eq_inputs[(eq_input_read+eq_input_count-1)%32];
        if(last.kind==kind && last.index==index){last.value=value;accepted=true;}
    }
    if(!accepted && eq_input_count<32){
        eq_inputs[(eq_input_read+eq_input_count)%32]={kind,index,value};
        ++eq_input_count;accepted=true;
    }
    portEXIT_CRITICAL(&eq_input_mux);
    if(!accepted)player_ui_set_notice("EQ busy - please try again");
}
static void reverb_ui_action(int kind,int value){eq_ui_action(100+kind,0,value);}
static void airpods_ui_action(int kind,int value){
    if(lock_input_gate.load(std::memory_order_acquire))return;
    Command c{};c.action=AIRPODS_ACTION;c.value=kind;c.fields[0]=value;
    if(xQueueSend(commands,&c,0)!=pdTRUE)player_ui_set_notice("Busy - please try again");
}
static bool eq_take_input(EqInput &input) {
    bool found=false;portENTER_CRITICAL(&eq_input_mux);
    if(eq_input_count){input=eq_inputs[eq_input_read];eq_input_read=(eq_input_read+1)%32;--eq_input_count;found=true;}
    portEXIT_CRITICAL(&eq_input_mux);return found;
}
static void ui_action(player_ui_action_t action, int32_t value, void *) {
    if(lock_input_gate.load(std::memory_order_acquire) && (action<PLAYER_UI_LOCK_ENABLE||action>PLAYER_UI_LOCK_INPUT))return;
    if(action==PLAYER_UI_VOLUME)latest_volume.store(value);else enqueue(action,value);
}
static void button_event(lv_event_t *event) {
    enqueue(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event))));
}
static void close_dialog() { if(dialog_kind==DIALOG_EQ||dialog_kind==DIALOG_REVERB){portENTER_CRITICAL(&eq_input_mux);eq_input_read=eq_input_count=0;portEXIT_CRITICAL(&eq_input_mux);} search_input = dialog_key_focus = nullptr; if (dialog) { lv_obj_delete(dialog); dialog = nullptr; } dialog_kind = DIALOG_NONE; bt_status = bt_list = power_message = nullptr; player_ui_set_overlay(false); }
static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int width = 0) {
    auto *obj = lv_label_create(parent); lv_label_set_text(obj, text); lv_obj_set_pos(obj, x, y);
    if (width) lv_obj_set_width(obj, width);
    return obj;
}
static lv_obj_t *button(lv_obj_t *parent, const char *text, int x, int y, int width, int action) {
    auto *obj = lv_button_create(parent); lv_obj_set_pos(obj, x, y); lv_obj_set_size(obj, width, 46);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0xDFEB73), 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(0x111312), 0);
    lv_obj_set_style_text_font(obj,&aurora_sans18,0);
    auto *caption = lv_label_create(obj); lv_label_set_text(caption, text); lv_obj_center(caption);
    if (action >= 0) lv_obj_add_event_cb(obj, button_event, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(action)));
    return obj;
}
static lv_obj_t *new_dialog(const char *title, bool closable = true) {
    close_dialog();
    dialog_closable = closable;
    // Keep opaque dialogs in the active screen's object tree. LVGL 9.3 can
    // then cull the player behind them; layer_top redraws the whole player
    // underneath every folder-scroll frame before painting over it.
    dialog = lv_obj_create(lv_screen_active()); lv_obj_set_size(dialog, 870, 630); lv_obj_center(dialog);
    dialog_kind = DIALOG_OTHER;player_ui_set_overlay(true);
    lv_obj_set_style_bg_color(dialog, lv_color_hex(0x141615), 0);
    lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(dialog, 0, 0);
    lv_obj_set_style_text_color(dialog, lv_color_hex(0xEDEEE8), 0);
    lv_obj_set_style_text_font(dialog, &aurora_sans18, 0);
    lv_obj_set_style_border_color(dialog, lv_color_hex(0xADAEA6), 0);
    lv_obj_set_style_border_width(dialog, 2, 0); lv_obj_set_style_radius(dialog, 6, 0);
    lv_obj_remove_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);
    label(dialog, title, 12, 6);
    if (closable) button(dialog, "CLOSE", 708, 540, 112, CLOSE);
    return dialog;
}
static void information(const char *title, const char *message) {
    auto *panel = new_dialog(title); label(panel, message, 12, 68, 790);
}
static uint32_t keyboard_ui_key(const tab5_keyboard_event_t &e) {
    switch(e.key){
    case TAB5_KEY_UP:return LV_KEY_UP;case TAB5_KEY_DOWN:return LV_KEY_DOWN;
    case TAB5_KEY_LEFT:return LV_KEY_LEFT;case TAB5_KEY_RIGHT:return LV_KEY_RIGHT;
    case TAB5_KEY_HOME:return LV_KEY_HOME;case TAB5_KEY_END:return LV_KEY_END;
    case TAB5_KEY_ENTER:return LV_KEY_ENTER;case TAB5_KEY_ESC:return LV_KEY_ESC;
    case TAB5_KEY_PAGE_UP:return PLAYER_UI_KEY_PAGE_UP;case TAB5_KEY_PAGE_DOWN:return PLAYER_UI_KEY_PAGE_DOWN;
    case TAB5_KEY_TAB:return (e.modifiers&TAB5_KEY_MOD_SHIFT)?LV_KEY_PREV:LV_KEY_NEXT;
    default:return 0;
    }
}
static void dialog_keyboard_controls(lv_obj_t *parent,lv_obj_t **items,size_t &count,unsigned depth=0) {
    if(depth>10||count>=64||lv_obj_has_flag(parent,LV_OBJ_FLAG_HIDDEN))return;
    for(uint32_t i=0;i<lv_obj_get_child_count(parent)&&count<64;++i){
        auto *obj=lv_obj_get_child(parent,i);
        if(lv_obj_has_flag(obj,LV_OBJ_FLAG_HIDDEN)||lv_obj_has_state(obj,LV_STATE_DISABLED))continue;
        if(lv_obj_has_class(obj,&lv_button_class)||lv_obj_has_class(obj,&lv_spinbox_class)||lv_obj_has_class(obj,&lv_slider_class)||lv_obj_has_class(obj,&lv_dropdown_class))items[count++]=obj;
        else dialog_keyboard_controls(obj,items,count,depth+1);
    }
}
static void dialog_keyboard(const tab5_keyboard_event_t &e) {
    if(!dialog)return;
    lv_obj_t *items[64]{};size_t count=0;
    dialog_keyboard_controls(dialog,items,count);
    lv_obj_t *open_dropdown=nullptr;
    for(size_t i=0;i<count;++i)if(lv_obj_has_class(items[i],&lv_dropdown_class)&&lv_dropdown_is_open(items[i])){open_dropdown=items[i];break;}
    if(e.key==TAB5_KEY_ESC){
        if(!e.repeat){
            if(open_dropdown){uint32_t key=LV_KEY_ESC;lv_obj_send_event(open_dropdown,LV_EVENT_KEY,&key);}
            else if(dialog_closable)enqueue(CLOSE);
        }
        return;
    }
    if(e.modifiers&(TAB5_KEY_MOD_CTRL|TAB5_KEY_MOD_ALT))return;
    uint32_t key=keyboard_ui_key(e);
    if(!key)return;
    if(open_dropdown){
        if(key==LV_KEY_ENTER||key==LV_KEY_UP||key==LV_KEY_DOWN||key==LV_KEY_LEFT||key==LV_KEY_RIGHT){
            if(key==LV_KEY_ENTER&&!e.repeat){
                // Match a touch selection: confirming the current factory
                // curve must reapply it too (including EQ ON and headroom).
                uint32_t selected=lv_dropdown_get_selected(open_dropdown);
                // Restore the committed index first: LVGL's setter is a no-op
                // when the previewed index already equals the requested one.
                uint32_t cancel=LV_KEY_ESC;lv_obj_send_event(open_dropdown,LV_EVENT_KEY,&cancel);
                lv_dropdown_set_selected(open_dropdown,selected);
                lv_obj_send_event(open_dropdown,LV_EVENT_VALUE_CHANGED,&selected);
            }else if(key!=LV_KEY_ENTER)lv_obj_send_event(open_dropdown,LV_EVENT_KEY,&key);
            return;
        }
        // Moving focus cancels an unconfirmed list choice before leaving it.
        uint32_t cancel=LV_KEY_ESC;lv_obj_send_event(open_dropdown,LV_EVENT_KEY,&cancel);
        for(size_t i=0;i<count;++i)if(items[i]==dialog_key_focus)lv_obj_remove_state(items[i],LV_STATE_FOCUS_KEY);
        dialog_key_focus=open_dropdown;
    }
    if(!count)return;
    size_t index=0;bool found=false;
    for(size_t i=0;i<count;++i)if(items[i]==dialog_key_focus){index=i;found=true;break;}
    if(found&&(key==LV_KEY_LEFT||key==LV_KEY_RIGHT)&&lv_obj_has_class(items[index],&lv_slider_class)){
        auto *slider=items[index];int value=lv_slider_get_value(slider)+(key==LV_KEY_RIGHT?1:-1);
        lv_slider_set_value(slider,value,LV_ANIM_OFF);lv_obj_send_event(slider,LV_EVENT_VALUE_CHANGED,nullptr);return;
    }
    if(found&&(key==LV_KEY_LEFT||key==LV_KEY_RIGHT)&&lv_obj_has_class(items[index],&lv_spinbox_class)){
        if(key==LV_KEY_RIGHT)lv_spinbox_increment(items[index]);else lv_spinbox_decrement(items[index]);return;
    }
    if(found){
        lv_obj_remove_state(items[index],LV_STATE_FOCUS_KEY);
        if(key==LV_KEY_NEXT||key==LV_KEY_DOWN||key==LV_KEY_RIGHT)index=(index+1)%count;
        if(key==LV_KEY_PREV||key==LV_KEY_UP||key==LV_KEY_LEFT)index=(index+count-1)%count;
    }
    if(key==LV_KEY_HOME)index=0;
    if(key==LV_KEY_END)index=count-1;
    dialog_key_focus=items[index];
    lv_obj_set_style_outline_color(dialog_key_focus,lv_color_hex(0xDFEB73),LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(dialog_key_focus,2,LV_STATE_FOCUS_KEY);
    lv_obj_add_state(dialog_key_focus,LV_STATE_FOCUS_KEY);
    lv_obj_scroll_to_view(dialog_key_focus,LV_ANIM_OFF);
    // First Enter only reveals focus, avoiding an unseen CLOSE/POWER action.
    if(found&&key==LV_KEY_ENTER&&!e.repeat){
        if(lv_obj_has_class(dialog_key_focus,&lv_dropdown_class))lv_obj_send_event(dialog_key_focus,LV_EVENT_KEY,&key);
        else if(!lv_obj_has_class(dialog_key_focus,&lv_spinbox_class)&&!lv_obj_has_class(dialog_key_focus,&lv_slider_class))lv_obj_send_event(dialog_key_focus,LV_EVENT_CLICKED,nullptr);
    }
}

struct Player {
    struct ScanJob {
        player::MusicLibrary library;
        player::ScanResult result;
        tab5_storage_info_t storage{};
        player::StorageLayout layout;
        bool layout_ready = false;
        std::atomic_uint visited{0}, found{0}, cached{0};
        std::atomic_bool finished{false}, scanning{false};
        bool mounted = false, force = false, from_index = false;
        bool startup_rescan_failed = false, rescan_scheduled = false;
        uint64_t elapsed_ms = 0;
    };
    ScanJob *scan_job = nullptr;
    player::StorageLayout storage_layout;
    bool storage_layout_ready = false;
    uint64_t last_scan_progress = 0;
    player::Settings settings;
    player::SettingsStore store{DATA};
    player::EqBank eq_bank;
    player::EqStore eq_store{DATA};
    audio_reverb_config_t reverb_config=audio_reverb_default_config();
    player::ReverbStore reverb_store{DATA};
    bool reverb_dirty=false,reverb_refresh_pending=false;
    const char *reverb_status="Changes are saved automatically.";
    bool eq_dirty=false,eq_refresh_pending=false;
    uint32_t eq_display_rate=0;
    uint64_t last_eq_meter=0;
    const char *eq_status="Choose a slot, then Save to keep your preset";
    player::FavoritesStore favorites{DATA, MUSIC};
    player::MusicLibrary library;
    player::ShuffleBag shuffle;
    std::vector<int> history;
    std::vector<size_t> playback_queue;
    bool playback_favorites = false;
    audio_snapshot_t audio{};
    tab5_storage_info_t storage{};
    tab5_battery_status_t battery_status{};
    bt_bridge_snapshot_t bt{};
    bt_bridge_device_t bt_display_devices[BT_BRIDGE_MAX_DEVICES]{};
    uint8_t bt_display_count = 255;
    bool bt_ready = false, bt_was_connected = false, bt_target_restored = false;
    tab5bt::AirPodsPlayback airpods_playback;
    uint64_t route_retry_after = 0, bt_dialog_updated = 0;
    int current = -1;
    uint32_t last_event = 0, pending_resume = 0;
    uint64_t dirty_since = 0, last_save = 0, save_retry_after = 0, last_state = 0, last_visual = 0, last_sensor = 0;
    uint64_t last_profile = 0, profile_render_ms = 0, last_metadata_save = 0, last_artwork = 0;
    uint64_t favorites_retry_after = 0;
    uint32_t profile_frames = 0, visual_period_ms = 33;
    player_ui_render_stats_t profile_previous{};
    bool dirty = false, mounted = false, audio_ready = false, dimmed = false, clock_valid = false, play_pending = false;
    bool persistence_loaded = false;
    bool resume_pending = false;
    bool pending_paused = false; // Intent of the outstanding play_pending load.
    bool keyboard_wake_hold = false;
    bool keyboard_event_pending = false;
    tab5_keyboard_event_t keyboard_pending{};
    uint64_t keyboard_pending_since = 0;
    unsigned keyboard_trace_remaining = 0;
    bool shutdown_requested = false, shutdown_attempted = false, shutdown_failed = false;
    bool shutdown_error_pending = false, shutdown_error_dialog_ready = false;
    const char *shutdown_failure_message = nullptr;
    uint64_t shutdown_error_retry_at = 0;
    audio_snapshot_t shutdown_audio{};
    int battery = -1;
    std::string message, search_query, requested_artwork;

#include "aurora_lock_controller.inc"

    void notice(const std::string &text) {
        message = text;
        if (tab5_display_lock(1000)) { player_ui_set_notice(message.c_str()); tab5_display_unlock(); }
    }
    struct ScreenshotJob {uint16_t *pixels{};char path[160]{};std::string directory;bool ok=false;std::atomic_bool done{false};};
    ScreenshotJob *screenshot_job=nullptr;
    void take_screenshot() {
        if(screenshot_job || !mounted || !storage_layout_ready || scan_job || dimmed){notice("Screenshot unavailable while SD or screen is busy");return;}
        auto *job=new(std::nothrow) ScreenshotJob;
        if(!job){notice("Not enough memory for screenshot");return;}
        job->directory=storage_layout.screenshots_dir;
        job->pixels=static_cast<uint16_t*>(heap_caps_malloc(1280*720*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        bool captured=false;
        if(job->pixels && tab5_display_lock(1000)){captured=tab5_display_snapshot_rgb565(job->pixels,1280*720)==ESP_OK;tab5_display_unlock();}
        if(!captured){heap_caps_free(job->pixels);delete job;notice("Could not capture screen");return;}
        screenshot_job=job;
        if(xTaskCreatePinnedToCore([](void *arg){
            auto *j=static_cast<ScreenshotJob*>(arg);mkdir(j->directory.c_str(),0755);
            // Exclusive creation prevents overwriting an existing screenshot after reboot.
            FILE *file=nullptr;for(unsigned i=1;i<=99999&&!file;i++){
                snprintf(j->path,sizeof j->path,"%s/Aurora-%05u.bmp",j->directory.c_str(),i);
                file=fopen(j->path,"wbx");if(!file&&errno!=EEXIST)break;
            }
            if(file){j->ok=screenshot_bmp(file,j->pixels,1280,720);if(j->ok)j->ok=fsync(fileno(file))==0;if(fclose(file)!=0)j->ok=false;if(!j->ok)remove(j->path);}
            heap_caps_free(j->pixels);j->pixels=nullptr;j->done.store(true,std::memory_order_release);vTaskDelete(nullptr);
        },"screen_capture",6144,job,1,nullptr,0)!=pdPASS){screenshot_job=nullptr;heap_caps_free(job->pixels);delete job;notice("Could not start screenshot save");}
    }
    void poll_screenshot(){if(screenshot_job&&screenshot_job->done.load(std::memory_order_acquire)){auto *j=screenshot_job;screenshot_job=nullptr;notice(j->ok?std::string("Saved ")+j->path:"Screenshot save failed. Check SD space.");delete j;}}
    void rebuild_queue(bool from_selection=false) {
        // The playback origin survives tab, search and format changes. Only
        // starting a selected row changes Favorites into ordinary folder play.
        if(from_selection)playback_favorites=settings.library_tab==2;
        playback_queue=library.folder_queue(playback_favorites?MUSIC:(settings.folder[0]?settings.folder:MUSIC),
                                            settings.sort,playback_favorites?&favorites:nullptr);
        history.clear();shuffle.reset(playback_queue.size(),queue_index(),esp_random());
    }
    void screen_power() {
        if(!tab5_display_lock(20))return;
        const bool wake=tab5_display_wake_requested();
        const uint32_t inactive=lv_display_get_inactive_time(lv_display_get_default());
        const bool off=!scan_job && settings.dim_seconds && inactive>=settings.dim_seconds*1000U;
        // The PIN must not depend on the screen blanking. A library scan keeps
        // the screen lit for its progress notice, and "Screen off after: Never"
        // never blanks it; both still lock after the dim time or 5 min idle.
        constexpr uint32_t lock_idle_never_ms=5*60*1000;
        const bool lit_lock=!dimmed && (scan_job || !settings.dim_seconds) &&
            inactive>=(settings.dim_seconds?settings.dim_seconds*1000U:lock_idle_never_ms) &&
            screen_lock.enabled() && !screen_lock.locked();
        if(dimmed && wake){
            player::sleep_tap_arm(false);
            if(screen_lock.locked())lock_show_pin_ui();
            player_ui_set_sleeping(false);
            if(tab5_display_sleep(false,settings.brightness)==ESP_OK){dimmed=false;audio_engine_set_visuals_enabled(settings.animations&&!lock_active());last_state=0;}
            else player_ui_set_sleeping(true);
        }else if(!dimmed && off){
            tab5_display_unlock();
            if(screen_lock.enabled() && !screen_lock.locked())lock_begin(LockFlow::Unlock);
            else if(!screen_lock.enabled()&&lock_active())lock_finish(false);
            if(!tab5_display_lock(1000))return;
            if(screen_lock.locked()){
                lock_clear_secrets();lock_hint.clear();lock_flow=LockFlow::Unlock;lock_show_pin_ui();
            }
            player_ui_set_sleeping(true);
            if(tab5_display_sleep(true,settings.brightness)==ESP_OK){dimmed=true;audio_engine_set_visuals_enabled(false);}
            else player_ui_set_sleeping(false);
        }else if(lit_lock){
            tab5_display_unlock();
            // No screen capture during a scan: its 1.8 MB would compete with
            // the scan's reserve; the cached frosted background is used.
            lock_begin(LockFlow::Unlock,!scan_job);
            return;
        }
        tab5_display_unlock();
    }
    void poll_sleep_taps() {
        bool eligible=dimmed && screen_lock.enabled() && screen_lock.locked() &&
            !screen_lock.storage_error() && audio_ready && !shutdown_requested;
        // Touch wake takes priority over a completed enclosure-tap sequence.
        if(eligible){
            if(tab5_display_lock(2)){
                eligible=tab5_display_is_sleeping()&&!tab5_display_wake_requested();
                tab5_display_unlock();
            }else eligible=false;
        }
        player::sleep_tap_set_playing(audio.state==AUDIO_PLAYING);
        player::sleep_tap_arm(eligible);
        const auto gesture=player::sleep_tap_take(uint32_t(now_ms()));
        if(!eligible || gesture==player::TapGesture::None)return;
        audio_engine_get_snapshot(&audio);
        const bool pause=gesture==player::TapGesture::Triple;
        // Explicit operations, never a toggle or starting a different track.
        if(play_pending || resume_pending || (pause?audio.state!=AUDIO_PLAYING:audio.state!=AUDIO_PAUSED))return;
        if(!pause && bt_ready && !update_audio_route())return;
        audio_engine_get_snapshot(&audio);
        if(play_pending || resume_pending || (pause?audio.state!=AUDIO_PLAYING:audio.state!=AUDIO_PAUSED))return;
        // A route update can wait; revalidate the asleep state before transport.
        if(!tab5_display_lock(2))return;
        const bool still_asleep=dimmed&&tab5_display_is_sleeping()&&!tab5_display_wake_requested();
        tab5_display_unlock();
        if(!still_asleep){player::sleep_tap_arm(false);return;}
        if(!pause && bt_ready)bluetooth_bridge_set_activity(true,false);
        airpods_playback.manual();
        const auto result=audio_engine_pause(pause);
        if(result==ESP_OK){
            if(!pause)resume_pending=true;
            mark_dirty();
            ESP_LOGI(TAG,"TAP %s accepted: %u taps; screen remains off and PIN locked",pause?"PAUSE":"RESUME",pause?3U:2U);
        }else ESP_LOGW(TAG,"TAP transport busy: %s; no retry queued",esp_err_to_name(result));
    }
    void mark_dirty() { dirty = true; dirty_since = now_ms(); }
    void adopt_storage_layout(player::StorageLayout layout) {
        // Never replace a live store: it owns the sequence of its next save.
        if(storage_layout_ready || persistence_loaded)return;
        storage_layout=std::move(layout);
        store=player::SettingsStore(storage_layout.settings_dir);
        eq_store=player::EqStore(storage_layout.eq_dir);
        reverb_store=player::ReverbStore(storage_layout.reverb_dir);
        favorites=player::FavoritesStore(storage_layout.favorites_dir,MUSIC);
        storage_layout_ready=true;
        if(storage_layout.persistent_read_error)ESP_LOGW(TAG,"STORAGE settings read failed; persistent writes blocked until restart");
        ESP_LOGI(TAG,"STORAGE layout moved=%lu archived=%lu errors=%lu legacy=%u cache=%s",
            static_cast<unsigned long>(storage_layout.moved),static_cast<unsigned long>(storage_layout.archived),
            static_cast<unsigned long>(storage_layout.errors),storage_layout.using_legacy,storage_layout.cache_file.c_str());
    }
    void load_persistence() {
        if (!mounted || persistence_loaded) return;
        if(!storage_layout_ready)adopt_storage_layout(player::prepare_storage_layout());
        store.load(settings);eq_store.load(eq_bank);reverb_store.load(reverb_config); favorites.load(); persistence_loaded = true;
        if (!player::valid_music_path(settings.last_path, MUSIC)) { settings.last_path[0] = 0; settings.resume_ms = 0; }
        settings.local_output = !(settings.bt_enabled && bt.connected);
        tab5_brightness_set(settings.brightness);audio_engine_set_visuals_enabled(settings.animations);
        // Also handle a card that first becomes available in the scan worker,
        // after the initial mount failed. Never overwrite its unseen favorites.
        if (bt_ready) {
            bluetooth_bridge_set_local_output(settings.local_output);
            bluetooth_bridge_set_enabled(settings.bt_enabled); bt_target_restored = false;
            bt_bridge_device_t remembered[8]{};
            for (unsigned i = 0; i < settings.receiver_count; ++i) {
                snprintf(remembered[i].mac, sizeof remembered[i].mac, "%s", settings.receivers[i].mac);
                snprintf(remembered[i].name, sizeof remembered[i].name, "%s", settings.receivers[i].name);
                remembered[i].saved = true;
            }
            bluetooth_bridge_restore_devices(remembered, settings.receiver_count);
        }
        if (audio_ready) { apply_volume(); audio_engine_set_effect(settings.effect);audio_engine_set_eq(&eq_bank.current);audio_engine_set_reverb(&reverb_config);audio_engine_set_speed(settings.speed_percent); }
    }
    void power_status(const char *text) {
        ESP_LOGI(TAG, "POWER: %s", text);
        if (tab5_display_lock(1000)) {
            if (power_message) lv_label_set_text(power_message, text);
            tab5_display_unlock();
        }
    }
    void restore_shutdown_error() {
        if (!shutdown_error_pending || now_ms() < shutdown_error_retry_at) return;
        shutdown_error_retry_at = now_ms() + 500;
        if (!tab5_display_lock(20)) return;
        if (!shutdown_error_dialog_ready) {
            auto *panel = new_dialog("POWER OFF / ACTION REQUIRED", false);
            power_message = label(panel, shutdown_failure_message, 12, 68, 790);
            button(panel, "TRY POWER OFF AGAIN", 12, 470, 300, POWER_RETRY);
            button(panel, "RESTART PLAYER", 500, 470, 300, RESTART);
            shutdown_error_dialog_ready = true;
        }
        player_ui_set_sleeping(false);
        const esp_err_t wake_result = tab5_display_sleep(false, settings.brightness);
        if (wake_result == ESP_OK) { dimmed = false; shutdown_error_pending = false; }
        else {
            player_ui_set_sleeping(true);
            ESP_LOGE(TAG, "Restore display after failed shutdown: %s; will retry", esp_err_to_name(wake_result));
        }
        tab5_display_unlock();
    }
    void shutdown_error(const char *text) {
        shutdown_failed = true;
        shutdown_failure_message = text;
        shutdown_error_pending = true;
        shutdown_error_dialog_ready = false;
        shutdown_error_retry_at = 0;
        ESP_LOGE(TAG, "POWER failed: %s", text);
        restore_shutdown_error();
    }
    void request_shutdown() {
        if(screenshot_job){notice("Finishing screenshot. Try power off again in a moment.");return;}
        if(lock_job)lock_job->cancel.store(true,std::memory_order_release);
        if (!shutdown_requested) {
            if (audio_ready) audio_engine_get_snapshot(&audio);
            shutdown_audio = audio;
        }
        shutdown_requested = true; shutdown_attempted = shutdown_failed = false;
        player::sleep_tap_arm(false);
        shutdown_error_pending = false;
        artwork_io_allowed.store(false, std::memory_order_release);
        if (tab5_display_lock(1000)) {
            auto *panel = new_dialog("POWER OFF", false);
            power_message = label(panel, scan_job ? "Finishing the library write before turning off ..." : "Stopping playback ...", 12, 68, 790);
            tab5_display_unlock();
        }
    }
    void perform_shutdown() {
        if(!player::sleep_tap_stop_and_wait(1800)){shutdown_error("Motion reader is still closing. Please retry power off.");return;}
        if(!lock_stop_worker(3000)){shutdown_error("Background reader is still closing. Please retry power off.");return;}
        power_status("Stopping playback and closing audio files ...");
        // Keep this snapshot until save(): STOP clears the engine's live path
        // and position, but the SD record must retain the last audible position.
        audio = shutdown_audio;
        if (audio_ready) {
            if (audio_engine_stop_and_wait(2500) != ESP_OK) {
                shutdown_error("Playback could not stop. Power remains on. Please try again."); return;
            }
        }
        play_pending = resume_pending = false; pending_resume = 0;
        if (!player_artwork_stop_and_wait(3000)) {
            shutdown_error("The cover file is still in use. Power remains on. Please try again."); return;
        }
        if (mounted) {
            power_status("Saving position and settings to SD ...");
            save();
            if (dirty) { shutdown_error("Settings could not be saved to SD. Please try again."); return; }
            if (favorites.dirty() && (storage_layout.persistent_read_error || !favorites.save())) {
                shutdown_error("Favorites could not be saved to SD. Please try again."); return;
            }
            // This cache is optional; records are CRC protected and every call
            // closes its file even on failure. Settings have already been synced.
            library.flush_metadata_cache(1);
        }
        power_status("Putting Pico to sleep ...");
        if (bt_ready) {
            const esp_err_t result = bluetooth_bridge_shutdown(2500);
            if (result != ESP_OK) ESP_LOGW(TAG, "Pico sleep acknowledgement: %s; continuing hardware power off", esp_err_to_name(result));
        }
        if (mounted) {
            power_status("Closing SD card ...");
            const esp_err_t result = tab5_storage_unmount();
            // IDF consumes the card handle even if its final VFS unregister
            // fails. A retry must not attempt another save on that handle.
            mounted = false;
            if (result != ESP_OK) {
                shutdown_error("SD card could not close safely. Please retry power off."); return;
            }
        }
        power_status("Turning off Tab5 ...");
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!tab5_display_lock(1000)) {
            shutdown_error("Display is busy. Power remains on. Please retry."); return;
        }
        player_ui_set_sleeping(true);
        const esp_err_t display_result = tab5_display_sleep(true, settings.brightness);
        if (display_result == ESP_OK) dimmed = true;
        else player_ui_set_sleeping(false);
        tab5_display_unlock();
        if (display_result != ESP_OK) {
            shutdown_error("Display could not blank. Power remains on. Please retry."); return;
        }
        const esp_err_t result = tab5_power_off();
        ESP_LOGE(TAG, "Hardware power-off returned: %s", esp_err_to_name(result));
        shutdown_error("Power-off was not completed. Retry, or double-press the physical power button.\n\nHolding RESET enters download mode; it does not turn off Tab5.");
    }
    const player::Track *track() const {
        if (current >= 0 && static_cast<size_t>(current) < library.visible.size()) return &library.tracks[library.visible[current]];
        // A folder switch may hide the requested track before the decoder
        // publishes its new path. Keep that request selected until it settles.
        if (play_pending && settings.last_path[0])
            for (const auto &t : library.tracks) if (t.path == settings.last_path) return &t;
        if (audio.path[0]) for (const auto &t : library.tracks) if (t.path == audio.path) return &t;
        return nullptr;
    }
    void request_main_artwork(const char *path) {
        const std::string desired = settings.covers && path ? path : "";
        if (desired == requested_artwork && !player_artwork_needs_retry()) return;
        if (player_artwork_request(desired.empty() ? nullptr : desired.c_str())) requested_artwork = desired;
    }
    void service_artwork(uint64_t now) {
        if (now - last_artwork < 200) return;
        last_artwork = now;
        if (!mounted || !settings.covers || dimmed || dialog || lock_active() || lock_job || screenshot_job || scan_job || play_pending || resume_pending) {
            player_artwork_request_thumbnails(nullptr, 0);
            return;
        }
        // The selected cover is also loaded before PLAY, after a saved-library
        // restore or a filter change. It always has priority over list images.
        const auto *selected = track();
        request_main_artwork(selected ? selected->path.c_str() : nullptr);
        size_t indices[8]{};
        if (!tab5_display_lock(2)) return;
        const size_t count = player_ui_get_missing_thumbnails(indices, 8);
        tab5_display_unlock();
        const char *paths[8]{};
        size_t used = 0;
        for (size_t i = 0; i < count && i < 8; ++i) {
            if (indices[i] >= library.visible.size()) continue;
            const size_t index = library.visible[indices[i]];
            if (index < library.tracks.size()) paths[used++] = library.tracks[index].path.c_str();
        }
        // This copies at most eight paths. No stat/read/decode occurs under
        // the display lock; the worker debounces and deduplicates view changes.
        player_artwork_request_thumbnails(paths, used);
    }
    int queue_index() const {
        const auto *t=track();if(!t)return -1;
        for(size_t i=0;i<playback_queue.size();i++)if(library.tracks[playback_queue[i]].path==t->path)return int(i);
        return -1;
    }
    bool play_queue(int index,bool remember=true,bool paused=false) {
        if(index<0||size_t(index)>=playback_queue.size())return false;
        if(playback_favorites && !favorites.contains(library.tracks[playback_queue[index]].path))return false;
        return play_path(library.tracks[playback_queue[index]].path,remember,0,paused);
    }
    void update_playlist() {
        std::vector<player_ui_track_t> rows;
        rows.reserve(library.visible.size());
        for (size_t n : library.visible) {
            auto &t = library.tracks[n]; rows.push_back({t.info.title, t.info.artist, t.info.duration_ms, favorites.contains(t.path), t.path.c_str()});
        }
        auto *prepared=player_ui_prepare_playlist(rows.data(),rows.size());
        if (prepared && tab5_display_lock(1000)) {
            player_ui_apply_playlist(prepared, current >= 0 ? current : SIZE_MAX);
            tab5_display_unlock();
        }
        player_ui_free_playlist(prepared);
        last_state = 0;
    }
    void filter() {
        std::string previous = track() ? track()->path : settings.last_path;
        library.filter_sort(settings.library_tab == 2 || !settings.folder[0] ? MUSIC : settings.folder,
                            settings.sort, search_query, settings.format_filter, &favorites, settings.library_tab == 2);
        current = library.find_visible(previous);
        if (current < 0 && !library.visible.empty() && !play_pending && !resume_pending &&
            (audio.state == AUDIO_STOPPED || !audio.path[0])) current = 0;
        update_playlist();
    }
    void update_folders() {
        std::vector<const char*> names;names.reserve(library.folders.size());size_t selected=SIZE_MAX;
        for(size_t i=0;i<library.folders.size();i++){
            const auto &path=library.folders[i];
            names.push_back(path==MUSIC?"All music":path.c_str()+strlen(MUSIC)+1);
            if(path==settings.folder)selected=i;
        }
        // Copy names outside the display lock; apply only swaps data and
        // refreshes the small pool of visible folder rows. No SD access here.
        auto *prepared=player_ui_prepare_folders(names.data(),names.size());
        if(prepared && tab5_display_lock(1000)){
            player_ui_apply_folders(prepared,selected);tab5_display_unlock();
        }
        player_ui_free_folders(prepared);
    }
    void scan(bool force = false) {
        if(!lock_stop_worker(3000)){notice("Background reader is closing. Please retry the library update.");return;}
        if(screenshot_job){notice("Finishing screenshot. Try update again in a moment.");return;}
        if (scan_job) { notice("Scan already running; progress is shown below."); return; }
        if (audio_ready) {
            if (audio_engine_stop_and_wait(2000) != ESP_OK) { notice("Audio has not stopped; scan cancelled"); return; }
            audio_engine_get_snapshot(&audio);
        }
        pending_resume = 0;
        play_pending = false;
        resume_pending = false;
        artwork_io_allowed.store(false);
        // The barrier invalidates the worker's request even if its ACK times
        // out. Allow the selected cover to be requested again after recovery.
        requested_artwork.clear(); last_artwork = 0;
        if (!player_artwork_stop_and_wait(3000)) {
            notice("Cover reader is still busy. Please retry the library update."); return;
        }
        if (tab5_display_lock(1000)) { player_ui_clear_thumbnails(); tab5_display_unlock(); }
        auto *job = new (std::nothrow) ScanJob;
        if (!job) { notice("Not enough memory to scan SD"); return; }
        job->mounted = mounted;
        job->force = force;
        job->layout=storage_layout;job->layout_ready=storage_layout_ready;
        scan_job = job;
        last_scan_progress = 0;
        notice(force ? "Updating music library ..." : "Loading saved library ...");
        // The controller and LVGL keep processing input while the SD worker runs.
        if (xTaskCreatePinnedToCore([](void *p) {
            auto *job = static_cast<ScanJob *>(p);
            const uint64_t started = now_ms();
            ESP_LOGI(TAG, "SCAN start (limit=%u)", static_cast<unsigned>(player::MusicLibrary::MAX_TRACKS));
            if (!job->mounted) job->mounted = tab5_storage_mount() == ESP_OK;
            if (job->mounted) {
                // A card may first become available here, after the boot mount
                // failed. Read its index only after migration has selected paths.
                if(!job->layout_ready){job->layout=player::prepare_storage_layout();job->layout_ready=true;}
                mkdir(MUSIC, 0755);
                tab5_storage_info(&job->storage);
                const std::string &cache = job->layout.cache_file;
                // A forced update needs a second full library next to the live
                // one. When that runs out of memory, the next startup (without a
                // live library) scans instead of loading the old index, once.
                const std::string rescan_marker = cache + ".rescan";
                FILE *marker = job->force ? nullptr : fopen(rescan_marker.c_str(), "rb");
                const bool startup_rescan = marker != nullptr;
                if (marker) fclose(marker);
                job->from_index = !job->force && !startup_rescan && job->library.load_index(MUSIC, cache, job->storage.identity);
                if (job->from_index) {
                    job->result.readable = true; job->result.cached = job->library.tracks.size();
                } else {job->scanning.store(true);job->result = job->library.scan(MUSIC, cache,
                    [](size_t visited, size_t found, size_t cached, void *context) {
                        auto *job = static_cast<ScanJob *>(context);
                        job->visited.store(visited, std::memory_order_relaxed);
                        job->found.store(found, std::memory_order_relaxed);
                        job->cached.store(cached, std::memory_order_relaxed);
                    }, job, false, job->storage.identity);
                    const bool out_of_memory = job->result.out_of_memory;
                    if (out_of_memory && job->force) {
                        if ((marker = fopen(rescan_marker.c_str(), "wb"))) job->rescan_scheduled = fclose(marker) == 0;
                    } else if (out_of_memory) {
                        // Startup: no live library competes, so the device cannot
                        // do better. Keep the previous library when there is one
                        // instead of starting empty; free the partial scan first.
                        job->library = player::MusicLibrary{};
                        job->result = player::ScanResult{}; job->result.readable = true;
                        if (startup_rescan && job->library.load_index(MUSIC, cache, job->storage.identity)) {
                            job->from_index = true; job->result.cached = job->library.tracks.size();
                        }
                        job->startup_rescan_failed = true;
                    }
                    // One startup attempt only; a completed scan also clears a
                    // marker left by an earlier refused update.
                    if (startup_rescan || !out_of_memory) remove(rescan_marker.c_str());
                }
            }
            job->elapsed_ms = now_ms() - started;
            ESP_LOGI(TAG, "%s complete: %u tracks, %u cached, %u probes, %llu ms", job->from_index ? "INDEX" : "SCAN",
                     static_cast<unsigned>(job->library.tracks.size()), static_cast<unsigned>(job->result.cached),
                     static_cast<unsigned>(job->result.probed), static_cast<unsigned long long>(job->elapsed_ms));
            // Publish the completed library. Do not access job after this store:
            // the controller can move its contents and delete it immediately.
            job->finished.store(true, std::memory_order_release);
            vTaskDelete(nullptr);
        }, "sd_catalog", 16384, job, 1, nullptr, 0) != pdPASS) {
            scan_job = nullptr; delete job; notice("Cannot start SD scan task");
        }
    }
    void poll_scan(uint64_t now) {
        if (!scan_job) return;
        if (!scan_job->finished.load(std::memory_order_acquire)) {
            if (now - last_scan_progress >= 500) {
                char text[144];
                snprintf(text, sizeof text, "Scanning /Music: %u items, %u tracks, %u cached ...",
                    scan_job->visited.load(std::memory_order_relaxed), scan_job->found.load(std::memory_order_relaxed),
                    scan_job->cached.load(std::memory_order_relaxed));
                notice(scan_job->scanning.load()?text:"Loading saved library into memory ..."); last_scan_progress = now;
            }
            return;
        }
        auto *job = scan_job; scan_job = nullptr;
        mounted = job->mounted;
        if (!mounted) {
            delete job; notice("SD unavailable. Insert an exFAT card, then restart or UPDATE LIBRARY."); return;
        }
        if(!storage_layout_ready)adopt_storage_layout(std::move(job->layout));
        load_persistence();
        auto result = job->result;
        if(result.out_of_memory){
            // Only a forced update reaches this: the worker resolves startup scans.
            const bool scheduled = job->rescan_scheduled;
            delete job;
            ESP_LOGW(TAG,"SCAN cancelled: memory budget exhausted; previous library retained%s",
                     scheduled ? ", full scan at next startup" : "; startup marker could not be written");
            notice(scheduled ? "Not enough memory to update library. Previous library kept; restart to update at startup." :
                               "Not enough memory to update library. Previous library kept.");
            return;
        }
        const bool indexed = job->from_index, rescan_failed = job->startup_rescan_failed;
        storage = job->storage;
        library = std::move(job->library);
        playback_queue.clear();history.clear();
        delete job;
        current = -1; // Resolve the saved path against the completed new library.
        if (!settings.folder[0] || !player::valid_music_path(settings.folder, MUSIC)) snprintf(settings.folder, sizeof settings.folder, "%s", MUSIC);
        if (std::find(library.folders.begin(), library.folders.end(), settings.folder) == library.folders.end()) snprintf(settings.folder, sizeof settings.folder, "%s", MUSIC);
        rebuild_queue();filter();update_folders();
        char text[160];
        snprintf(text, sizeof text, "%s | %u tracks | %u cached entries | tags load on play%s", storage.exfat ? "exFAT" : "FAT32",
            static_cast<unsigned>(library.tracks.size()), static_cast<unsigned>(result.cached),
            result.limited ? " | scan limit reached" : "");
        notice(result.readable ? text : "Cannot read /Music. Check the SD card.");
        if (indexed) notice("Saved library ready. New or removed music? SETTINGS > UPDATE LIBRARY.");
        if (library.tracks.empty()) notice("Copy MP3, WAV, FLAC or AAC files to /Music, then SETTINGS > UPDATE LIBRARY.");
        if (rescan_failed) { // Last, so it is the notice that stays visible.
            ESP_LOGW(TAG,"SCAN at startup ran out of memory; %s", indexed ? "saved library kept" : "no saved library");
            notice(indexed ? "Library too large to update on the device. Saved library kept." :
                             "Library too large to scan on the device. Remove some music from /Music.");
        }
    }
    void save() {
        if (!mounted || !persistence_loaded || scan_job) return;
        if(storage_layout.persistent_read_error){
            dirty=true;dirty_since=now_ms();save_retry_after=dirty_since+10000;
            notice("SD settings could not be read safely. Changes stay in memory; restart to retry.");return;
        }
        if (track()) {
            if (track()->path != settings.last_path) settings.resume_ms = 0;
            snprintf(settings.last_path, sizeof settings.last_path, "%s", track()->path.c_str());
            if (track()->path == audio.path) {
                if (audio.state == AUDIO_PLAYING || audio.state == AUDIO_PAUSED) settings.resume_ms = audio.position_ms;
                else if (audio.state == AUDIO_ENDED) settings.resume_ms = 0;
            }
        }
        const bool eq_ok=!eq_dirty || eq_store.save(eq_bank);
        if(eq_ok)eq_dirty=false;
        const bool reverb_ok=!reverb_dirty || reverb_store.save(reverb_config);
        if(reverb_ok)reverb_dirty=false;
        const bool settings_ok=store.save(settings);
        if(dialog_kind==DIALOG_REVERB){reverb_status=settings_ok&&reverb_ok?"Settings saved to SD.":"Save failed; retrying. Previous valid copy retained.";reverb_refresh_pending=true;}
        if (settings_ok && eq_ok && reverb_ok) { dirty = false; last_save = now_ms(); save_retry_after = 0; }
        else {
            dirty = true; dirty_since = now_ms(); save_retry_after = dirty_since + 10000;
            notice("Settings write failed. Previous valid copy retained.");
        }
    }
    void apply_loaded_metadata() {
        size_t index = 0;
        while (index < library.tracks.size() && library.tracks[index].path != audio.path) ++index;
        if (index == library.tracks.size()) return;
        audio_file_info_t info = library.tracks[index].info;
        info.format = audio.format; info.sample_rate = audio.sample_rate;
        info.duration_ms = audio.duration_ms; info.bitrate_kbps = audio.bitrate_kbps;
        info.bits_per_sample = audio.bits_per_sample; info.channels = audio.channels;
        info.block_align = info.channels * (info.bits_per_sample / 8);
        info.file_size = audio.file_size; info.duration_exact = audio.duration_exact;
        if (audio.title[0]) snprintf(info.title, sizeof info.title, "%s", audio.title);
        snprintf(info.artist, sizeof info.artist, "%s", audio.artist);
        snprintf(info.album, sizeof info.album, "%s", audio.album);
        if (library.update_metadata(index, info)) {
            player_ui_track_t row{info.title, info.artist, info.duration_ms, favorites.contains(audio.path), audio.path};
            const int visible = library.find_visible(audio.path);
            if (visible >= 0 && tab5_display_lock(1000)) {
                player_ui_update_track(visible, &row);
                tab5_display_unlock();
            }
            last_state = 0;
        }
    }
    bool play_path(const std::string &path, bool remember = true, uint32_t resume = 0, bool paused=false) {
        if (scan_job) { notice("Finishing SD scan before playback ..."); return false; }
        if (!audio_ready || path.empty()) return false;
        const int index = library.find_visible(path);
        const int previous_queue=queue_index();
        if (bt_ready && !update_audio_route()) return false;
        artwork_io_allowed.store(false);
        bluetooth_bridge_set_activity(true, dialog_kind == DIALOG_BT);
        if (audio_engine_play_paused(path.c_str(),paused) != ESP_OK) { notice("Audio busy - try again"); return false; }
        request_main_artwork(path.c_str());
        play_pending = true; pending_paused = paused;
        resume_pending = false;
        if (remember && settings.shuffle && previous_queue >= 0 && track() && track()->path != path) {
            if (history.size() >= 128) history.erase(history.begin());
            history.push_back(previous_queue);
        }
        current = index; pending_resume = resume; settings.resume_ms = resume;
        snprintf(settings.last_path, sizeof settings.last_path, "%s", path.c_str());
        // Selection changes are handled by set_state; keep all track strings.
        mark_dirty(); last_state = 0; notice(""); return true;
    }
    bool play(int index, bool remember = true, uint32_t resume = 0) {
        if (index < 0 || static_cast<size_t>(index) >= library.visible.size()) return false;
        if(!play_path(library.tracks[library.visible[index]].path, remember, resume))return false;
        rebuild_queue(true);
        return true;
    }
    bool skip_keeps_paused() const {
        return player::skip_keeps_paused(audio.state, resume_pending, play_pending, pending_paused);
    }
    void next(bool automatic) {
        if (automatic && settings.repeat == 1 && audio.path[0] &&
            (!playback_favorites || favorites.contains(audio.path))) { play_path(audio.path, false); return; }
        if (playback_queue.empty()) return;
        const int at=queue_index();int index;
        if(settings.shuffle)index=shuffle.next(at,settings.repeat==2);
        else {index=at+1;if(size_t(index)>=playback_queue.size())index=settings.repeat==2?0:-1;}
        if(index>=0)play_queue(index,true,!automatic&&skip_keeps_paused());
    }
    void previous() {
        if(audio.position_ms>3000 && audio.seek_supported &&
           (!playback_favorites || (track() && favorites.contains(track()->path))))audio_engine_seek_ms(0);
        else if(settings.shuffle&&!history.empty()){int p=history.back();history.pop_back();play_queue(p,false,skip_keeps_paused());}
        else if(!playback_queue.empty()){int at=queue_index();play_queue(at>0?at-1:settings.repeat==2?int(playback_queue.size())-1:0,false,skip_keeps_paused());}
    }
    void apply_volume() {
        audio_engine_set_volume(settings.volume, settings.muted);
        if (bt_ready) bluetooth_bridge_set_volume(settings.muted ? 0 : settings.bt_volume);
    }
    bool update_audio_route() {
        const bool local = !settings.bt_enabled || !bt.connected;
        if (static_cast<bool>(settings.local_output) == local) return true;
        if (local && audio_ready) {
            if (now_ms() < route_retry_after) return false;
            // Decoder PAUSED alone is not enough: the output task must finish
            // its in-flight PCM write and mute before enabling the speaker.
            if (audio_engine_pause_and_wait(2500) != ESP_OK) {
                route_retry_after = now_ms() + 2000;
                notice("Could not pause audio. Speaker remains muted; retrying ...");
                ESP_LOGW(TAG, "ROUTE local postponed: audio pause not confirmed");
                return false;
            }
            audio_engine_get_snapshot(&audio);
            // EOF may race with disconnection. Consume that event so the next
            // loop cannot automatically start another track on the speaker.
            if (audio.event == AUDIO_EVENT_ENDED) last_event = audio.event_sequence;
            play_pending = resume_pending = false;
        }
        route_retry_after = 0;
        settings.local_output = local;
        bluetooth_bridge_set_local_output(local);
        mark_dirty(); last_state = 0;
        ESP_LOGI(TAG, "ROUTE %s; audio_state=%d position=%lums", local ? "LOCAL (paused)" : "BLUETOOTH ONLY", audio.state, static_cast<unsigned long>(audio.position_ms));
        notice(local ? "Headphones disconnected. Playback paused; Play uses the speaker." : "Bluetooth connected. Audio switched to headphones.");
        return true;
    }
    void search_dialog() {
        auto *panel = new_dialog("SEARCH MUSIC / filename, title, artist, album");
        dialog_kind = DIALOG_SEARCH;
        keyboard_trace_remaining = 8;
        auto *input = lv_textarea_create(panel); lv_obj_set_pos(input, 12, 66); lv_obj_set_size(input, 795, 64);
        search_input = input;
        lv_textarea_set_one_line(input, true); lv_textarea_set_max_length(input, 96);
        lv_textarea_set_text(input, search_query.c_str());
        button(panel, "CLEAR FILTER", 12, 146, 195, SEARCH_CLEAR);
        auto *apply = button(panel, "SEARCH", 606, 146, 195, -1);
        lv_obj_add_event_cb(apply, [](lv_event_t *e) {
            enqueue_text(SEARCH_APPLY, lv_textarea_get_text(static_cast<lv_obj_t *>(lv_event_get_user_data(e))));
        }, LV_EVENT_CLICKED, input);
        auto *keyboard = player_search_keyboard(panel,input);
        lv_obj_add_event_cb(keyboard, [](lv_event_t *e) {
            enqueue_text(SEARCH_APPLY, lv_textarea_get_text(static_cast<lv_obj_t *>(lv_event_get_user_data(e))));
        }, LV_EVENT_READY, input);
        lv_obj_add_event_cb(keyboard,[](lv_event_t*){enqueue(CLOSE);},LV_EVENT_CANCEL,nullptr);
    }
    void keyboard_event(const tab5_keyboard_event_t &e) {
        // Entire method runs under the LVGL lock. Only enqueue controller work:
        // no storage, decoder or I2C transactions while holding that lock.
        if(shutdown_requested)return;
        lv_display_trigger_activity(lv_display_get_default());
        if(dimmed){
            player::sleep_tap_arm(false);
            keyboard_wake_hold=true;
            if(screen_lock.locked())lock_show_pin_ui();
            player_ui_set_sleeping(false);
            if(tab5_display_sleep(false,settings.brightness)==ESP_OK){
                dimmed=false;last_state=0;audio_engine_set_visuals_enabled(settings.animations&&!dialog&&!lock_active());
            }else player_ui_set_sleeping(true);
            return;
        }
        if(keyboard_wake_hold){if(e.repeat)return;keyboard_wake_hold=false;}
        if(lock_active()){
            if(e.repeat||(e.modifiers&(TAB5_KEY_MOD_CTRL|TAB5_KEY_MOD_ALT)))return;
            uint32_t key=keyboard_ui_key(e);
            if(e.key>='0'&&e.key<='9')key=e.key;
            if(e.key==TAB5_KEY_BACKSPACE||e.key==TAB5_KEY_DELETE)key=LV_KEY_BACKSPACE;
            if(key)player_ui_keyboard(key);
            return;
        }
        const bool ctrl=(e.modifiers&TAB5_KEY_MOD_CTRL)!=0;
        const bool alt=(e.modifiers&TAB5_KEY_MOD_ALT)!=0;
        if(dialog_kind==DIALOG_SEARCH&&search_input){
            if(e.key==TAB5_KEY_ESC&&!e.repeat){enqueue(CLOSE);return;}
            if(e.key==TAB5_KEY_ENTER&&!e.repeat){enqueue_text(SEARCH_APPLY,lv_textarea_get_text(search_input));return;}
            if(alt)return;
            if(ctrl){if(e.key=='u'||e.key=='U')lv_textarea_set_text(search_input,"");return;}
            switch(e.key){
            case TAB5_KEY_BACKSPACE:lv_textarea_delete_char(search_input);break;
            case TAB5_KEY_DELETE:lv_textarea_delete_char_forward(search_input);break;
            case TAB5_KEY_LEFT:lv_textarea_cursor_left(search_input);break;
            case TAB5_KEY_RIGHT:lv_textarea_cursor_right(search_input);break;
            case TAB5_KEY_HOME:lv_textarea_set_cursor_pos(search_input,0);break;
            case TAB5_KEY_END:lv_textarea_set_cursor_pos(search_input,LV_TEXTAREA_CURSOR_LAST);break;
            default:if(e.key>=32&&e.key<=126){char text[]{static_cast<char>(e.key),0};lv_textarea_add_text(search_input,text);}break;
            }
            return;
        }
        if(dialog){dialog_keyboard(e);return;}
        if(!ctrl&&!alt){
            const uint32_t key=keyboard_ui_key(e);
            if(key&&player_ui_keyboard(key))return;
        }
        if(!player_ui_keyboard_text_allowed())return;
        using S=player::KeyboardShortcut;
        const S shortcut=player::keyboard_shortcut(e);
        const int volume=settings.bt_enabled&&!settings.local_output?settings.bt_volume:settings.volume;
        switch(shortcut){
        case S::PlayPause:enqueue(PLAYER_UI_PLAY_PAUSE);break;
        case S::Previous:enqueue(PLAYER_UI_PREVIOUS);break;
        case S::Next:enqueue(PLAYER_UI_NEXT);break;
        case S::VolumeUp:enqueue(PLAYER_UI_VOLUME,std::min(100,volume+5));break;
        case S::VolumeDown:enqueue(PLAYER_UI_VOLUME,std::max(0,volume-5));break;
        case S::SeekBack:enqueue(PLAYER_UI_SEEK_MS,std::max<int64_t>(0,static_cast<int64_t>(audio.position_ms)-5000));break;
        case S::SeekForward:enqueue(PLAYER_UI_SEEK_MS,std::min<int64_t>(INT32_MAX,static_cast<int64_t>(audio.position_ms)+5000));break;
        case S::Search:search_dialog();break;
        case S::NextTab:enqueue(PLAYER_UI_LIBRARY_TAB,(settings.library_tab+1)%3);break;
        case S::PreviousTab:enqueue(PLAYER_UI_LIBRARY_TAB,(settings.library_tab+2)%3);break;
        case S::Settings:enqueue(PLAYER_UI_SETTINGS);break;
        case S::Bluetooth:enqueue(PLAYER_UI_BLUETOOTH);break;
        case S::Mute:enqueue(PLAYER_UI_MUTE);break;
        case S::None:
            if(!ctrl&&!alt&&e.key>32&&e.key<=126){
                search_dialog();char text[]{static_cast<char>(e.key),0};lv_textarea_set_text(search_input,text);
            }
            break;
        }
    }
    void poll_keyboard() {
        // Fetch without the UI lock: a large search dialog can render longer
        // than the previous 2 ms try-lock on every iteration. Wait only when
        // an input event exists, retaining it across a busy display instead
        // of starving input. No lock at all for an idle/absent keyboard.
        if(!tab5_keyboard_connected()){keyboard_event_pending=false;return;}
        const uint64_t now=now_ms();
        if(keyboard_event_pending&&now-keyboard_pending_since>=2000)keyboard_event_pending=false;
        if(!keyboard_event_pending){
            if(!tab5_keyboard_read_event(&keyboard_pending))return;
            keyboard_event_pending=true;keyboard_pending_since=now;
        }
        if(!tab5_display_lock(100))return;
        if(!tab5_keyboard_connected()){keyboard_event_pending=false;tab5_display_unlock();return;}
        const bool was_sleeping=dimmed;
        const size_t before=search_input?strlen(lv_textarea_get_text(search_input)):0;
        keyboard_event(keyboard_pending);
        const bool trace=keyboard_trace_remaining&&!keyboard_pending.repeat&&dialog_kind==DIALOG_SEARCH;
        const size_t after=search_input?strlen(lv_textarea_get_text(search_input)):0;
        if(trace)--keyboard_trace_remaining;
        keyboard_event_pending=false;
        tab5_display_unlock();
        // Bounded diagnostics contain lengths/context only, never query text.
        if(trace)ESP_LOGI(TAG,"KEYBOARD_SEARCH mods=%u chars=%u->%u wake=%d",
            unsigned(keyboard_pending.modifiers),unsigned(before),unsigned(after),was_sleeping);
    }
    void update_bt_dialog() {
        if (dialog_kind != DIALOG_BT || !bt_status || !bt_list) return;
        if(bt_airpods){if(bt.airpods_capable)lv_obj_remove_flag(bt_airpods,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(bt_airpods,LV_OBJ_FLAG_HIDDEN);}
        char status[240];
        snprintf(status, sizeof status, "%s  |  %s  |  Pico %s\n%s%s",
            settings.bt_enabled ? (bt.connected ? "CONNECTED" : bt.online ? "READY" : "ASLEEP / NO LINK") : "BLUETOOTH OFF",
            settings.local_output ? "LOCAL OUTPUT ON" : "HEADPHONES ONLY",
            bt.version[0] ? bt.version : "--", bt.message, bt.volume_pending ? "  / volume awaiting ACK" : "");
        if (strcmp(lv_label_get_text(bt_status), status)) lv_label_set_text(bt_status, status);
        bool changed = bt_display_count != bt.device_count;
        if (!changed) for (unsigned i = 0; i < bt.device_count; ++i)
            if (strcmp(bt_display_devices[i].mac, bt.devices[i].mac) || strcmp(bt_display_devices[i].name, bt.devices[i].name) ||
                bt_display_devices[i].saved != bt.devices[i].saved || bt_display_devices[i].selected != bt.devices[i].selected) { changed = true; break; }
        if (!changed) return;
        const int32_t scroll = lv_obj_get_scroll_y(bt_list);
        lv_obj_clean(bt_list); bt_display_count = bt.device_count;
        memcpy(bt_display_devices, bt.devices, sizeof bt_display_devices);
        for (unsigned i = 0; i < bt.device_count; ++i) {
            const auto &device = bt_display_devices[i]; char caption[150];
            snprintf(caption, sizeof caption, "%s%s  %s  %s", device.selected ? "> " : "", device.name[0] ? device.name : device.mac,
                device.saved ? "[saved]" : "[new]", device.mac);
            auto *row = lv_list_add_button(bt_list, LV_SYMBOL_BLUETOOTH, caption); lv_obj_set_height(row, 50);
            lv_obj_add_event_cb(row, [](lv_event_t *e) {
                enqueue_text(BT_SELECT, static_cast<const char *>(lv_event_get_user_data(e)));
            }, LV_EVENT_CLICKED, const_cast<char *>(device.mac));
        }
        if (!bt.device_count) lv_list_add_text(bt_list, "Turn on Bluetooth, then SCAN to find headphones.");
        lv_obj_scroll_to_y(bt_list, scroll, LV_ANIM_OFF);
    }
    void bluetooth_dialog() {
        auto *panel = new_dialog("PICO BLUETOOTH AUDIO"); dialog_kind = DIALOG_BT;
        button(panel, settings.bt_enabled ? "BT OFF" : "BT ON", 12, 55, 189, BT_ENABLE);
        button(panel, "SCAN", 214, 55, 189, BT_SCAN); button(panel, "RECONNECT", 416, 55, 189, BT_RECONNECT);
        bt_airpods=button(panel,"AIRPODS",618,55,189,AIRPODS_OPEN);
        bt_status = label(panel, "", 12, 114, 790);
        bt_list = lv_list_create(panel); lv_obj_set_pos(bt_list, 12, 181); lv_obj_set_size(bt_list, 795, 315);
        bt_display_count = 255; update_bt_dialog();
    }
    AirPodsDialogState airpods_view() const {
        AirPodsDialogState s{};s.capable=bt.airpods_capable;s.headphones_connected=bt.connected;
        s.channel_connected=bt.airpods_connected;s.connecting=bt.airpods_connecting;s.pending=bt.airpods_pending;
        s.mode=bt.airpods_mode;s.battery_left=bt.airpods_left;s.battery_right=bt.airpods_right;s.battery_case=bt.airpods_case;
        s.ears=bt.airpods_ears;s.auto_pause=bt.airpods_auto_pause;s.auto_pause_known=bt.airpods_connected;
        s.status=bt.airpods_message;return s;
    }
    void open_airpods() {
        if(!bt.airpods_capable){notice("AirPods controls require Pico 0.9.4 or newer.");return;}
        if(!tab5_display_lock(20))return;
        auto *panel=new_dialog("AIRPODS",false);dialog_kind=DIALOG_AIRPODS;dialog_closable=true;
        airpods_dialog_build(panel,airpods_view(),airpods_ui_action);tab5_display_unlock();
    }
    void airpods_action(int kind,int value) {
        if(dialog_kind!=DIALOG_AIRPODS)return;
        if(kind==AIRPODS_CLOSE){if(tab5_display_lock(20)){close_dialog();tab5_display_unlock();}return;}
        esp_err_t result=ESP_ERR_INVALID_ARG;
        if(kind==AIRPODS_SET_MODE)result=bluetooth_bridge_airpods(BT_AIRPODS_MODE,value);
        else if(kind==AIRPODS_SET_AUTOPAUSE){airpods_playback.manual();result=bluetooth_bridge_airpods(BT_AIRPODS_AUTO_PAUSE,value);}
        else if(kind==AIRPODS_CONNECT)result=bluetooth_bridge_airpods(BT_AIRPODS_CONNECT,0);
        if(result!=ESP_OK)notice("AirPods are not ready or a request is still pending.");
    }
    void poll_bluetooth(uint64_t now) {
        if (!bt_ready) return;
        bluetooth_bridge_get_snapshot(&bt);
        update_audio_route();
        if(bt.stream_failed && settings.bt_enabled && !settings.local_output &&
           (audio.state==AUDIO_PLAYING || play_pending || resume_pending)){
            audio_engine_pause(true);play_pending=resume_pending=false;notice("Bluetooth stream stalled. Press Play to retry.");
            audio_engine_get_snapshot(&audio);
        }
        const bool use = play_pending || resume_pending || audio.state == AUDIO_PLAYING || audio.state == AUDIO_LOADING || audio.state == AUDIO_SEEKING;
        bluetooth_bridge_set_activity(use, (dialog_kind == DIALOG_BT||dialog_kind==DIALOG_AIRPODS) && !dimmed);
        if (settings.bt_enabled && bt.online && bt.devices_ready && settings.bt_target[0] && !bt_target_restored) {
            for (unsigned i = 0; i < bt.device_count; ++i) if (bt.devices[i].saved && !strcmp(bt.devices[i].mac, settings.bt_target)) {
                if (bt.devices[i].selected || bluetooth_bridge_select(settings.bt_target) == ESP_OK) bt_target_restored = true;
                break;
            }
        }
        player::SavedReceiver saved[8]{}; uint8_t count = 0;
        if (bt.online && bt.devices_ready) {
            for (unsigned i = 0; i < bt.device_count && count < 8; ++i) if (bt.devices[i].saved) {
                snprintf(saved[count].mac, sizeof saved[count].mac, "%s", bt.devices[i].mac);
                snprintf(saved[count].name, sizeof saved[count].name, "%s", bt.devices[i].name); ++count;
            }
            // Empty/transient lists are not evidence that remembered receivers
            // were forgotten. Bond keys always remain authoritative on Pico.
            if (count && (count != settings.receiver_count || memcmp(saved, settings.receivers, sizeof saved))) {
                settings.receiver_count = count; memcpy(settings.receivers, saved, sizeof saved); mark_dirty();
            }
        }
        if (bt_was_connected && !bt.connected && settings.bt_enabled) bluetooth_bridge_reconnect();
        bt_was_connected = bt.connected;
        for (bt_remote_action_t remote; (remote = bluetooth_bridge_pop_remote()) != BT_REMOTE_NONE;) {
            // Discard commands queued by a receiver that has just disconnected.
            if (!settings.bt_enabled || !bt.connected) continue;
            airpods_playback.manual(); // An explicit PAUSE also cancels ear-owned resume when already paused.
            Command c{};
            if (remote == BT_REMOTE_PLAY) {
                if (!bt.connected || audio.state == AUDIO_PLAYING || play_pending || resume_pending) continue;
                c.action = PLAYER_UI_PLAY_PAUSE;
            } else if (remote == BT_REMOTE_PAUSE) {
                if (audio.state == AUDIO_PAUSED || audio.state == AUDIO_STOPPED || audio.state == AUDIO_ENDED) continue;
                c.action = PLAYER_UI_PLAY_PAUSE;
            } else c.action = remote == BT_REMOTE_NEXT ? PLAYER_UI_NEXT : PLAYER_UI_PREVIOUS;
            action(c,true);
        }
        const auto ear_action=airpods_playback.update(settings.bt_enabled&&bt.connected&&bt.airpods_connected,
            bt.airpods_auto_pause,bt.airpods_ears,audio.state==AUDIO_PLAYING,audio.state==AUDIO_PAUSED);
        if(ear_action!=tab5bt::AirPodsPlayback::Action::None){
            const bool paused=ear_action==tab5bt::AirPodsPlayback::Action::Pause;
            if(!paused)bluetooth_bridge_set_activity(true,dialog_kind==DIALOG_BT||dialog_kind==DIALOG_AIRPODS);
            if(audio_engine_pause(paused)==ESP_OK){if(!paused)resume_pending=true;else play_pending=resume_pending=false;mark_dirty();}
            else airpods_playback.manual();
        }
        if(dialog_kind==DIALOG_AIRPODS&&now-bt_dialog_updated>250&&tab5_display_lock(10)){
            airpods_dialog_refresh(dialog,airpods_view());tab5_display_unlock();bt_dialog_updated=now;
        }
        if (dialog_kind == DIALOG_BT && now - bt_dialog_updated > 250 && tab5_display_lock(100)) {
            update_bt_dialog(); tab5_display_unlock(); bt_dialog_updated = now;
        }
    }
    void clock_dialog() {
        auto *panel = new_dialog("SET DATE AND TIME");
        label(panel, "LOCAL CLOCK    Year / Month / Day / Hour / Minute", 12, 188);
        time_t stamp = time(nullptr); struct tm now{}; localtime_r(&stamp, &now);
        int values[]{clock_valid ? now.tm_year + 1900 : 2026, clock_valid ? now.tm_mon + 1 : 1, clock_valid ? now.tm_mday : 1, clock_valid ? now.tm_hour : 12, clock_valid ? now.tm_min : 0};
        const int low[]{2000,1,1,0,0}, high[]{2099,12,31,23,59};
        for (int i = 0; i < 5; ++i) {
            int x = 12 + i * 142;
            auto *spin = lv_spinbox_create(panel); date_fields[i] = spin;
            lv_obj_set_pos(spin, x, 274); lv_obj_set_size(spin, 118, 46);
            lv_spinbox_set_digit_format(spin, i == 0 ? 4 : 2, 0); lv_spinbox_set_range(spin, low[i], high[i]);
            lv_spinbox_set_value(spin, values[i]); lv_spinbox_set_step(spin, 1);
            auto *up = button(panel, "+", x, 222, 118, -1);
            auto *down = button(panel, "-", x, 326, 118, -1);
            lv_obj_add_event_cb(up, [](lv_event_t *e) { lv_spinbox_increment(static_cast<lv_obj_t *>(lv_event_get_user_data(e))); }, LV_EVENT_CLICKED, spin);
            lv_obj_add_event_cb(down, [](lv_event_t *e) { lv_spinbox_decrement(static_cast<lv_obj_t *>(lv_event_get_user_data(e))); }, LV_EVENT_CLICKED, spin);
        }
        auto *set = button(panel, "SET CLOCK", 12, 386, 180, -1);
        lv_obj_add_event_cb(set, [](lv_event_t *) {
            Command c{}; c.action = CLOCK_SET;
            for (int i = 0; i < 5; ++i) c.fields[i] = lv_spinbox_get_value(date_fields[i]);
            if (xQueueSend(commands, &c, 0) != pdTRUE) player_ui_set_notice("Busy - try again");
        }, LV_EVENT_CLICKED, nullptr);
    }

    void eq_refresh() {
        eq_refresh_pending=true;
        if(dialog_kind==DIALOG_EQ && tab5_display_lock(30)){
            portENTER_CRITICAL(&eq_input_mux);const bool pending=eq_input_count!=0;portEXIT_CRITICAL(&eq_input_mux);
            // A newer touch sample already on screen must not be replaced
            // by the controller's older model. Retry after its input is drained.
            if(dialog_kind==DIALOG_EQ && !pending){
                eq_dialog_refresh(dialog,eq_bank.current,eq_bank.selected,audio.sample_rate,eq_status);eq_refresh_pending=false;
            }
            tab5_display_unlock();
        }else if(dialog_kind!=DIALOG_EQ)eq_refresh_pending=false;
    }
    void eq_apply() {
        eq_bank.current.preamp_db=AUDIO_MASTER_GAIN_DB;
        audio_engine_set_eq(&eq_bank.current);eq_dirty=true;mark_dirty();
    }
    void poll_eq_edits() {
        EqInput input{};bool changed=false;
        // Bounded per controller iteration, preserving the exact touch order.
        for(unsigned n=0;n<32 && eq_take_input(input);n++){
            if(dialog_kind==DIALOG_REVERB){
                if(input.kind>=100&&input.kind<=100+REVERB_CLOSE)reverb_action(input.kind-100,input.value);
                if(input.kind==100+REVERB_CLOSE)break; // Busy display retries on the next controller iteration.
                continue;
            }
            if(dialog_kind!=DIALOG_EQ)continue;
            if(input.kind==EQ_BAND && input.index>=0 && input.index<11){
                eq_bank.current.gain_db[input.index]=std::clamp(input.value,-12,12);changed=true;
            }else if(input.kind==EQ_PREAMP || input.kind==EQ_HEADROOM){
                continue; /* Retired controls cannot change the fixed level. */
            }else{
                if(changed){eq_apply();changed=false;}
                Command c{};c.action=EQ_ACTION;c.value=input.kind;c.fields[0]=input.index;c.fields[1]=input.value;eq_action(c);
            }
        }
        if(changed){eq_apply();eq_status="Live changes - Save stores them in the selected preset";}
        if(eq_refresh_pending)eq_refresh();
        if(reverb_refresh_pending&&dialog_kind==DIALOG_REVERB&&tab5_display_lock(2)){
            portENTER_CRITICAL(&eq_input_mux);const bool pending=eq_input_count!=0;portEXIT_CRITICAL(&eq_input_mux);
            if(!pending){reverb_dialog_refresh(dialog,reverb_config,settings.effect==2,reverb_status);reverb_refresh_pending=false;}
            tab5_display_unlock();
        }
    }
    void open_reverb() {
        if(!tab5_display_lock(1000))return;
        auto *panel=new_dialog("REVERB",false);dialog_kind=DIALOG_REVERB;dialog_closable=true;
        reverb_status=mounted?"Changes are saved automatically.":"SD unavailable; changes apply until restart.";
        reverb_dialog_build(panel,reverb_config,settings.effect==2,reverb_status,reverb_ui_action);tab5_display_unlock();
    }
    void reverb_action(int kind,int value) {
        if(dialog_kind!=DIALOG_REVERB)return;
        if(kind==REVERB_CLOSE){if(tab5_display_lock(20)){close_dialog();tab5_display_unlock();}else reverb_ui_action(kind,value);return;}
        if(kind==REVERB_ENABLE){
            const int effect=value?2:0;if(audio_engine_set_effect(effect)==ESP_OK){settings.effect=effect;mark_dirty();last_state=0;}
        }else{
            auto next=reverb_config;
            if(kind==REVERB_AMOUNT)next.amount=std::clamp(value,0,60);
            else if(kind==REVERB_DECAY)next.decay_ms=std::clamp(value,500,4000);
            else if(kind==REVERB_TONE)next.tone_hz=std::clamp(value,int(next.low_cut_hz)+100,20000);
            else if(kind==REVERB_LOW_CUT)next.low_cut_hz=std::clamp(value,0,int(next.tone_hz)-100);
            else if(kind==REVERB_RESET)next=audio_reverb_default_config();
            else return;
            if(audio_engine_set_reverb(&next)==ESP_OK){reverb_config=next;reverb_dirty=true;mark_dirty();}
        }
        reverb_status=!mounted?"SD unavailable; changes apply until restart.":settings.effect==2?"Live changes; saving automatically.":"Enable Reverb to hear these settings.";
        reverb_refresh_pending=true;
    }
    void open_eq() {
        if(tab5_display_lock(1000)){
            auto *panel=new_dialog("11-BAND EQUALIZER",false);
            dialog_kind=DIALOG_EQ;dialog_closable=true;
            lv_obj_set_size(panel,1180,630);lv_obj_center(panel);
            eq_status="Choose a slot, then Save to keep your preset";
            eq_dialog_build(panel,eq_bank.current,eq_bank.selected,audio.sample_rate,eq_status,eq_ui_action);
            eq_display_rate=audio.sample_rate;
            tab5_display_unlock();
        }
    }
    void eq_action(const Command &c) {
        if(dialog_kind!=DIALOG_EQ)return;
        switch(c.value){
        case EQ_CLOSE:
            if(tab5_display_lock(1000)){close_dialog();tab5_display_unlock();}last_state=0;return;
        case EQ_SELECT_SLOT:
            eq_bank.selected=std::clamp(c.fields[0],0,3);eq_dirty=true;mark_dirty();
            eq_status=(eq_bank.saved_mask&(1<<eq_bank.selected))?"Saved preset selected - press Load":"Empty slot - press Save to store current settings";break;
        case EQ_ENABLE: eq_bank.current.enabled=!eq_bank.current.enabled;eq_apply();eq_status=eq_bank.current.enabled?"EQ bands on - volume stays on the main knob":"EQ bands bypassed - same base volume";break;
        case EQ_RESET: memset(eq_bank.current.gain_db,0,sizeof(eq_bank.current.gain_db));eq_apply();eq_status="Flat bands - base volume and saved presets kept";break;
        case EQ_PREAMP: case EQ_HEADROOM:return;
        case EQ_FACTORY:
            if(c.fields[0]<0 || unsigned(c.fields[0])>=player::EQ_FACTORY_PRESET_COUNT)return;
            memcpy(eq_bank.current.gain_db,player::eq_factory_presets[c.fields[0]].gain_db,sizeof(eq_bank.current.gain_db));
            eq_bank.current.enabled=1;eq_apply();eq_status="Factory curve applied - same base volume. Save to keep it in a User slot.";break;
        case EQ_LOAD:
            if(eq_bank.saved_mask&(1<<eq_bank.selected)){eq_bank.current=eq_bank.presets[eq_bank.selected];eq_apply();eq_status="Preset loaded";}
            else {eq_status="This slot is empty - save a preset first";}
            break;
        case EQ_SAVE:
            eq_bank.current.preamp_db=AUDIO_MASTER_GAIN_DB;
            eq_bank.presets[eq_bank.selected]=eq_bank.current;eq_bank.saved_mask|=1<<eq_bank.selected;eq_dirty=true;mark_dirty();
            if(!mounted)eq_status="Not saved: SD card unavailable";
            else if(scan_job)eq_status="Save queued until library scan completes";
            else {save();eq_status=eq_dirty?"Save failed - previous valid copy retained":"Preset saved to SD";}
            break;
        default:return;
        }
        eq_refresh_pending=true;
    }
    void action(const Command &c,bool headset=false) {
        if (shutdown_requested && c.action != POWER_RETRY && c.action != RESTART) return;
        if(lock_action(c))return;
        const bool media=headset&&(c.action==PLAYER_UI_PLAY_PAUSE||c.action==PLAYER_UI_NEXT||c.action==PLAYER_UI_PREVIOUS);
        if(lock_active()&&!media)return;
        if(c.action==PLAYER_UI_PLAY_PAUSE||c.action==PLAYER_UI_NEXT||c.action==PLAYER_UI_PREVIOUS||
           c.action==PLAYER_UI_SELECT_TRACK||c.action==PLAYER_UI_SEEK_MS||c.action==BT_ENABLE)airpods_playback.manual();
        switch (c.action) {
        case PLAYER_UI_POWER_OFF: case POWER_OFF: request_shutdown(); break;
        case POWER_RETRY:
            if (shutdown_failed) request_shutdown();
            break;
        case RESTART: if (shutdown_failed) esp_restart(); break;
        case EQ_ACTION: eq_action(c);break;
        case PLAYER_UI_REVERB_SETTINGS: open_reverb();break;
        case AIRPODS_OPEN:open_airpods();break;
        case AIRPODS_ACTION:airpods_action(c.value,c.fields[0]);break;
        case PLAYER_UI_EFFECT:
            if (c.value >= 0 && c.value <= 3 && audio_engine_set_effect(c.value) == ESP_OK) {
                settings.effect = c.value; mark_dirty();if(c.value==3){audio_engine_set_eq(&eq_bank.current);open_eq();}
            }
            break;
        case PLAYER_UI_LIBRARY_TAB:
            if (c.value >= 0 && c.value <= 2) {
                settings.library_tab = c.value; filter(); mark_dirty(); last_state=0;
                if (c.value == 1) { update_folders(); notice(library.folders.empty() ? "No folders yet. Add music to /Music and update the library." : ""); }
                else if (library.visible.empty()) notice(c.value == 2 ? "No favorites in this view. Tap a heart to add music." : "No tracks match this view.");
                else notice("");
            }
            break;
        case PLAYER_UI_FOLDER:
            settings.library_tab=1;filter();update_folders();mark_dirty();
            if(tab5_display_lock(1000)){close_dialog();player_ui_open_files();tab5_display_unlock();}
            notice("");break;
        case PLAYER_UI_FORMAT_FILTER:
            if (c.value >= 0 && c.value <= 4) {
                settings.format_filter = c.value; filter(); mark_dirty();
                notice(library.visible.empty() ? "No tracks match this format. Tap the selected format again for all." : "");
            }
            break;
        case PLAYER_UI_FAVORITE: {
            const player::Track *selected = c.value >= 0 && static_cast<size_t>(c.value) < library.visible.size() ? &library.tracks[library.visible[c.value]] : track();
            if (!selected) break;
            const std::string path = selected->path;
            if (!favorites.toggle(path)) { notice("Could not change favorite: invalid path or list is full."); break; }
            const bool favorite = favorites.contains(path);
            if(playback_favorites)rebuild_queue();
            if (settings.library_tab == 2) filter();
            else {
                const int visible = library.find_visible(path);
                player_ui_track_t row{selected->info.title, selected->info.artist, selected->info.duration_ms, favorite, selected->path.c_str()};
                if (visible >= 0 && tab5_display_lock(1000)) { player_ui_update_track(visible, &row); tab5_display_unlock(); }
            }
            notice(favorite ? "Added to favorites" : "Removed from favorites");
            break;
        }
        case PLAYER_UI_PLAY_PAUSE:
            if (bt_ready && !update_audio_route()) break;
            if (audio.state == AUDIO_PLAYING || audio.state == AUDIO_PAUSED) {
                if (audio.state == AUDIO_PAUSED) bluetooth_bridge_set_activity(true, dialog_kind == DIALOG_BT);
                if (audio_engine_pause(audio.state != AUDIO_PAUSED) != ESP_OK) notice("Audio busy - try again");
                else { if (audio.state == AUDIO_PAUSED) resume_pending = true; pending_paused = audio.state != AUDIO_PAUSED; }
                mark_dirty();
            } else if (audio.state == AUDIO_LOADING || audio.state == AUDIO_SEEKING) {
                if (audio_engine_pause(true) != ESP_OK) notice("Audio busy - try again");
                else pending_paused = true;
            } else if(playback_favorites){
                const int at=queue_index();
                if(at>=0)play_path(library.tracks[playback_queue[at]].path,false,track()&&track()->path==settings.last_path?settings.resume_ms:0);
                else if(!playback_queue.empty())play_queue(0,false);
            } else play(current < 0 ? 0 : current, false, track() && track()->path == settings.last_path ? settings.resume_ms : 0);
            break;
        case PLAYER_UI_NEXT: next(false); break;
        case PLAYER_UI_PREVIOUS: previous(); break;
        case PLAYER_UI_SELECT_TRACK:
            if (play(c.value)) shuffle.reset(playback_queue.size(), queue_index(), esp_random());
            break;
        case PLAYER_UI_SEEK_MS:
            if (audio.seek_supported) { if (audio_engine_seek_ms(std::max(c.value, 0)) != ESP_OK) notice("Seek busy - try again"); }
            else notice("Start playback before seeking");
            break;
        case PLAYER_UI_VOLUME:
            settings.muted=false;
            if (settings.bt_enabled && !settings.local_output) settings.bt_volume = std::clamp(c.value, 0, 100);
            else settings.volume = std::clamp(c.value, 0, 100);
            apply_volume(); mark_dirty(); break;
        case PLAYER_UI_MUTE: settings.muted = !settings.muted; apply_volume(); mark_dirty(); break;
        case PLAYER_UI_SCREENSHOT: take_screenshot();break;
        case PLAYER_UI_SHUFFLE: settings.shuffle = !settings.shuffle; shuffle.reset(playback_queue.size(), queue_index(), esp_random());history.clear(); mark_dirty(); break;
        case PLAYER_UI_REPEAT: settings.repeat = settings.repeat==0?2:settings.repeat==2?1:0; mark_dirty(); break;
        case PLAYER_UI_VISUAL_MODE: settings.visual = std::clamp(c.value, 0, 2); mark_dirty(); break;
        case PLAYER_UI_VIEW: settings.grid = !settings.grid; mark_dirty(); break;
        case PLAYER_UI_SORT_SET: case PLAYER_UI_SORT: settings.sort = c.action==PLAYER_UI_SORT_SET?std::clamp(c.value,0,2):(settings.sort + 1) % 3; filter(); mark_dirty(); notice(settings.sort == 0 ? "Sort: file path" : settings.sort == 1 ? "Sort: title / filename" : "Sort: known duration; unread tracks last"); break;
        case PLAYER_UI_DRAGON_TYPE: settings.dragon_type=std::clamp(c.value,0,1);mark_dirty();break;
        case PLAYER_UI_TRACK_SPEED: settings.speed_percent=std::clamp(c.value,75,125);audio_engine_set_speed(settings.speed_percent);mark_dirty();break;
        case PLAYER_UI_DRAGON_SPEED: settings.dragon_speed=std::clamp(c.value,4,16);mark_dirty();break;
        case PLAYER_UI_DRAGON_COLORS: settings.dragon_colors=!settings.dragon_colors;mark_dirty();break;
        case PLAYER_UI_ANIMATIONS: settings.animations=!settings.animations;audio_engine_set_visuals_enabled(settings.animations&&!dimmed);mark_dirty();break;
        case PLAYER_UI_CLOCK_FORMAT: settings.clock24=!settings.clock24;mark_dirty();break;
        case PLAYER_UI_RESET_VISUAL:
            settings.dragon_type=1;settings.dragon_speed=10;settings.dragon_colors=1;
            settings.animations=1;settings.clock24=1;settings.brightness=100;
            tab5_brightness_set(settings.brightness);audio_engine_set_visuals_enabled(true);mark_dirty();break;
        case PLAYER_UI_BRIGHTNESS:
        case BRIGHTNESS: settings.brightness = std::clamp(c.value, 10, 100); tab5_brightness_set(settings.brightness); mark_dirty(); break;
        case PLAYER_UI_DIM_TIME: case DIM_TIME: settings.dim_seconds = c.value; mark_dirty(); break;
        case PLAYER_UI_FOLDER_SELECT: case FOLDER_SELECT:
            if (c.value >= 0 && static_cast<size_t>(c.value) < library.folders.size()) {
                // Browsing folders does not interrupt the currently playing file.
                search_query.clear(); settings.library_tab = 0;
                snprintf(settings.folder, sizeof settings.folder, "%s", library.folders[c.value].c_str());
                if(!playback_favorites)rebuild_queue();
                filter();update_folders();mark_dirty();
                ESP_LOGI(TAG,"VIEW folder_selected=%ld queue_tracks=%u playback_tracks=%u",static_cast<long>(c.value),static_cast<unsigned>(library.visible.size()),static_cast<unsigned>(playback_queue.size()));
                notice(library.visible.empty()?"No tracks match this folder and file filter.":"");
            }
            if (tab5_display_lock(1000)) { close_dialog(); tab5_display_unlock(); } break;
        case PLAYER_UI_RESCAN: case RESCAN:
            if (tab5_display_lock(1000)) { close_dialog(); tab5_display_unlock(); } save(); search_query.clear(); scan(true); break;
        case SEARCH_APPLY: case SEARCH_CLEAR:
            search_query = c.action == SEARCH_CLEAR ? "" : c.text;
            filter();
            if (tab5_display_lock(1000)) { close_dialog(); tab5_display_unlock(); }
            notice(search_query.empty() ? "Search filter cleared" : std::to_string(library.visible.size()) + " results / " + search_query);
            break;
        case PLAYER_UI_COVERS: case COVERS:
            settings.covers = !settings.covers;
            request_main_artwork(track() ? track()->path.c_str() : nullptr);
            if (!settings.covers) {
                player_artwork_request_thumbnails(nullptr, 0);
                if (tab5_display_lock(1000)) { player_ui_clear_thumbnails(); tab5_display_unlock(); }
            }
            last_artwork = 0;
            mark_dirty(); break;
        case BT_ENABLE:
            settings.bt_enabled = !settings.bt_enabled; bt_target_restored = false;
            update_audio_route(); bluetooth_bridge_set_enabled(settings.bt_enabled); apply_volume();
            mark_dirty(); if (tab5_display_lock(1000)) { bluetooth_dialog(); tab5_display_unlock(); } break;
        case BT_VOLUME: settings.bt_volume = std::clamp(c.value, 0, 100); apply_volume(); mark_dirty(); break;
        case BT_SCAN:
            settings.bt_enabled = 1; bluetooth_bridge_set_enabled(true); bluetooth_bridge_scan(); mark_dirty();
            if (tab5_display_lock(1000)) { bluetooth_dialog(); tab5_display_unlock(); } break;
        case BT_RECONNECT:
            settings.bt_enabled = 1; bluetooth_bridge_set_enabled(true); bluetooth_bridge_reconnect(); mark_dirty(); break;
        case BT_SELECT:
            settings.bt_enabled = 1; bluetooth_bridge_set_enabled(true);
            if (bluetooth_bridge_select(c.text) == ESP_OK) {
                snprintf(settings.bt_target, sizeof settings.bt_target, "%.17s", c.text); bt_target_restored = true; mark_dirty();
            } else notice("Bluetooth command busy. Try selecting the receiver again.");
            break;
        case BT_TEST: bluetooth_bridge_test(); break;
        case PLAYER_UI_SAVE: case SAVE:
            if (scan_job) { mark_dirty(); notice("Settings will be saved when the scan finishes"); }
            else { save(); notice(dirty ? "Could not save settings" : "Settings saved to SD"); }
            break;
        case CLOCK_SET: {
            struct tm t{}; t.tm_year = c.fields[0] - 1900; t.tm_mon = c.fields[1] - 1; t.tm_mday = c.fields[2]; t.tm_hour = c.fields[3]; t.tm_min = c.fields[4];
            if (tab5_rtc_write(&t) == ESP_OK) { timeval v{mktime(&t), 0}; settimeofday(&v, nullptr); clock_valid = true; notice("RTC clock set"); }
            else notice("Clock not set. Check date and RTC connection.");
            break;
        }
        default:
            if (!tab5_display_lock(1000)) break;
            switch (c.action) {
            case CLOSE: close_dialog(); break;
            case PLAYER_UI_SETTINGS: case PLAYER_UI_ADVANCED: close_dialog();player_ui_open_settings();break;
            case PLAYER_UI_SET_CLOCK: clock_dialog();break;
            case PLAYER_UI_SEARCH: search_dialog(); break;
            case PLAYER_UI_WIFI: information("WI-FI STATUS", "Wi-Fi is OFF in this local SD player.\n\nPlayback works offline. The C6 radio is not initialized,\nwhich avoids radio power consumption and SDIO contention.\n\nInternet streaming / Wi-Fi provisioning are not included."); break;
            case PLAYER_UI_BLUETOOTH: bluetooth_dialog(); break;
        case PLAYER_UI_FILE_INFO: {
                const player::Track *selected = c.value >= 0 && static_cast<size_t>(c.value) < library.visible.size() ? &library.tracks[library.visible[c.value]] : track();
                if (selected) {
                    if (!selected->metadata_ready) {
                        information("FILE INFORMATION", "Start playback to load this file's tags, duration and audio format.\n\nMetadata is loaded on demand so large SD libraries appear quickly.");
                        break;
                    }
                    char text[1024]; const auto &t = *selected;
                    snprintf(text, sizeof text, "%s\n\n%s / %u Hz / %u bit / %u channels\nSize: %.2f MiB\nDuration: %s\n\nLocal: stereo PCM16 at source rate.\nBluetooth: stereo PCM16 / 44.1 kHz.\nFX applies to both outputs. Normal leaves the signal unchanged.\nFLAC/AAC seek decodes from the beginning and may take longer.\n\nSD: %s\nSettings generation: %llu", t.path.c_str(), format_name(t.info.format), static_cast<unsigned>(t.info.sample_rate), static_cast<unsigned>(t.info.bits_per_sample), static_cast<unsigned>(t.info.channels), static_cast<double>(t.info.file_size)/1048576., t.info.duration_exact ? "exact" : "estimated until EOF", storage.exfat ? "exFAT" : "FAT", static_cast<unsigned long long>(store.sequence()));
                    information("FILE / OUTPUT INFORMATION", text);
                }
                break;
            }
            }
            tab5_display_unlock();
        }
        last_state = 0;
    }
    void render(uint64_t now) {
        if(dimmed){last_state=now;return;}
        const auto *t = track();
        const bool live = t && t->path == audio.path && audio.state != AUDIO_STOPPED;
        char filename[512]{};
        if (t) snprintf(filename, sizeof filename, "%s", t->path.c_str() + t->path.find_last_of('/') + 1);
        char modified[32] = "--", clock[16] = "--:--", date[24] = "SET CLOCK";
        if (t) { time_t stamp = t->modified; struct tm tm{}; localtime_r(&stamp, &tm); strftime(modified, sizeof modified, "%Y.%m.%d %H:%M", &tm); }
        if (clock_valid) { time_t stamp = time(nullptr); struct tm tm{}; localtime_r(&stamp, &tm); strftime(clock, sizeof clock, settings.clock24 ? "%H:%M" : "%I:%M%p", &tm); strftime(date, sizeof date, "%Y.%m.%d", &tm); }
        player_ui_state_t s{};
        s.title = t ? t->info.title : "NO TRACKS"; s.artist = t && t->info.artist[0] ? t->info.artist : "LOCAL SD AUDIO";
        s.album = t && t->info.album[0] ? t->info.album : "AURORA PLAYER";
        s.file_name = filename; s.format = t ? format_name(t->info.format) : "--";
        s.modified = modified; s.clock_text = clock; s.date_text = date; s.firmware = esp_app_get_description()->version;
        s.position_ms = live ? audio.position_ms : (t && t->path == settings.last_path ? settings.resume_ms : 0);
        s.duration_ms = live && audio.duration_ms ? audio.duration_ms : t ? t->info.duration_ms : 0;
        s.sample_rate = t ? t->info.sample_rate : 0; s.bit_depth = t ? t->info.bits_per_sample : 0;
        s.file_bytes = t ? t->info.file_size : 0; s.sd_total_bytes = storage.total_bytes; s.sd_used_bytes = storage.total_bytes - std::min(storage.total_bytes, storage.free_bytes);
        s.sd_free_known = storage.free_known;
        s.track_index = current >= 0 ? current : SIZE_MAX; s.track_count = library.visible.size();
        s.volume = settings.bt_enabled && !settings.local_output ? settings.bt_volume : settings.volume;
        s.speed_percent=settings.speed_percent;s.repeat = settings.repeat; s.visual_mode = settings.visual; s.battery_percent = battery;
        s.dragon_type=settings.dragon_type;s.dragon_speed=settings.dragon_speed;s.dragon_colors=settings.dragon_colors;
        s.animations=settings.animations;s.clock24=settings.clock24;s.brightness=settings.brightness;
        s.effect = settings.effect; s.library_tab = settings.library_tab; s.format_filter = settings.format_filter;
        const char *folder_name=strrchr(settings.folder,'/');
        s.folder_label=!settings.folder[0]||!strcmp(settings.folder,MUSIC)?"All music":folder_name?folder_name+1:settings.folder;
        s.favorite = t && favorites.contains(t->path); s.artwork_key = t ? t->path.c_str() : "";
        s.bitrate_kbps = live ? audio.bitrate_kbps : t ? t->info.bitrate_kbps : 0;
        s.battery_charging = battery_status.charging_known && battery_status.charging;
        s.battery_estimated = battery >= 0;s.battery_mv=battery_status.millivolts;s.battery_ma=battery_status.milliamps;s.battery_charging_known=battery_status.charging_known;
        s.dim_seconds=settings.dim_seconds;s.covers=settings.covers;s.sort=settings.sort;s.headphones_known=tab5_headphones_status(&s.headphones);
        s.bluetooth_available = bt_ready && bt.online; s.bluetooth_enabled = settings.bt_enabled; s.bluetooth_connected = bt.connected;
        s.playing = live && audio.state == AUDIO_PLAYING; s.shuffle = settings.shuffle; s.muted = settings.muted; s.grid_view = settings.grid;
        if (tab5_display_lock(100)) {
            if(!dialog){player_ui_set_state(&s);player_artwork_poll();}
            tab5_display_unlock();
        }
        last_state = now;
    }
    void profile(uint64_t now) {
        if (now - last_profile < 10000) return;
        player_ui_render_stats_t stats{};
        if (!tab5_display_lock(50)) return;
        player_ui_get_render_stats(&stats);
        tab5_display_unlock();
        const uint32_t frames = stats.frames - profile_frames;
        const uint64_t elapsed = stats.total_ms - profile_render_ms;
        const uint32_t mean_ms = frames ? static_cast<uint32_t>(elapsed / frames) : 0;
        // Latest audio data at 30 Hz; a slow frame never permanently ratchets down the animation.
        visual_period_ms = 33;
        ESP_LOGI(TAG, "PERF frames=%lu mean=%lums max=%lums visual_period=%lums audio_state=%d position=%lums output_errors=%lu starvations=%lu read_max=%luus decode_max=%luus heap=%lu",
                 static_cast<unsigned long>(frames), static_cast<unsigned long>(mean_ms), static_cast<unsigned long>(stats.max_ms),
                 static_cast<unsigned long>(visual_period_ms), audio.state, static_cast<unsigned long>(audio.position_ms),
                 static_cast<unsigned long>(audio.output_errors), static_cast<unsigned long>(audio.buffer_starvations),
                 static_cast<unsigned long>(audio.max_read_us), static_cast<unsigned long>(audio.max_decode_us),
                 static_cast<unsigned long>(esp_get_free_heap_size()));
        ESP_LOGI(TAG,"EQ effect=%u enabled=%u last_us=%lu max_us=%lu rate=%lu reduction_db=%.1f base_db=%d fx_last_us=%lu fx_max_us=%lu",settings.effect,eq_bank.current.enabled,
                 (unsigned long)audio.eq_last_us,(unsigned long)audio.eq_max_us,(unsigned long)audio.sample_rate,
                 (double)audio.eq_reduction_db,AUDIO_MASTER_GAIN_DB,
                 (unsigned long)audio.fx_last_us,(unsigned long)audio.fx_max_us);
        const uint32_t scroll_frames=stats.scroll_frames-profile_previous.scroll_frames;
        ESP_LOGI(TAG,"UI_WINDOW frames=%lu max_ms=%lu slow33=%lu stall50=%lu scroll_frames=%lu scroll_mean_ms=%lu scroll_slow33=%lu scroll_stall50=%lu mode=%u dragon=%u",
                 (unsigned long)frames,(unsigned long)stats.interval_max_ms,
                 (unsigned long)(stats.slow_frames-profile_previous.slow_frames),
                 (unsigned long)(stats.stall_frames-profile_previous.stall_frames),
                 (unsigned long)scroll_frames,
                 (unsigned long)(scroll_frames?(stats.scroll_total_ms-profile_previous.scroll_total_ms)/scroll_frames:0),
                 (unsigned long)(stats.scroll_slow_frames-profile_previous.scroll_slow_frames),
                 (unsigned long)(stats.scroll_stall_frames-profile_previous.scroll_stall_frames),
                 settings.visual,settings.dragon_type);
        profile_previous=stats;
        profile_frames = stats.frames; profile_render_ms = stats.total_ms; last_profile = now;
        ESP_LOGI(TAG, "STATUS battery=%d%% voltage=%dmV current=%+dmA charging=%d charging_known=%d BT enabled=%d online=%d connected=%d devices=%u pico=%s errors=%lu",
                 battery, static_cast<int>(battery_status.millivolts), battery_status.milliamps,
                 battery_status.charging, battery_status.charging_known,
                 settings.bt_enabled, bt.online, bt.connected, bt.device_count, bt.version,
                 static_cast<unsigned long>(bt.output_errors));
        ESP_LOGI(TAG,"BATTERY_FILTER raw=%d%% filtered_mv=%u sample_age_ms=%lu",battery_status.raw_estimated_percent,
                 unsigned(battery_status.filtered_millivolts),static_cast<unsigned long>(battery_status.sample_age_ms));
        if(settings.bt_enabled)ESP_LOGI(TAG,"BT_AUDIO dropped=%lu empty=%lu buffered=%lu rx=%lu callbacks=%lu consumed=%lu recoveries=%lu/%lu input_stalls=%lu flushes=%lu failed=%d",
                 (unsigned long)bt.pico_dropped,(unsigned long)bt.pico_empty,(unsigned long)bt.pico_buffered,
                 (unsigned long)bt.pico_rx_blocks,(unsigned long)bt.pico_callbacks,(unsigned long)bt.pico_consumed,
                 (unsigned long)bt.stream_recoveries,(unsigned long)bt.pico_recoveries,(unsigned long)bt.pico_input_stalls,(unsigned long)bt.pico_flushes,bt.stream_failed);
        if (scan_job) ESP_LOGI(TAG, "SCAN progress: %u items, %u tracks, %u cached",
                              scan_job->visited.load(), scan_job->found.load(), scan_job->cached.load());
    }
    void run() {
        ESP_LOGI(TAG, "BOOT 01: player task started; free heap=%lu", static_cast<unsigned long>(esp_get_free_heap_size()));
        commands = xQueueCreate(32, sizeof(Command));
        screen_lock.load();
        lock_input_gate.store(true,std::memory_order_release);
        if (!tab5_display_init(0)) {
            // Keep the failure visible in USB logs even after the initial message.
            for (;;) {
                ESP_LOGE(TAG, "Display startup failed: %s", tab5_display_last_error());
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
        }
        player::StartupView startup;
        while(!tab5_display_lock(1000))vTaskDelay(pdMS_TO_TICKS(10));
        {
            auto *initial_screen=lv_screen_active();
            startup.create(esp_app_get_description()->version);
            lv_obj_delete(initial_screen);
            if(!commands)startup.progress("Command queue allocation failed. Restart the device.");
            tab5_display_unlock();
        }
        if (!commands) {
            if(tab5_display_lock(1000)){tab5_display_reveal(70);tab5_display_unlock();}
            for (;;) { ESP_LOGE(TAG, "Command queue allocation failed"); vTaskDelay(pdMS_TO_TICKS(5000)); }
        }
        // Mount once before allocating the player assets, so the optional SD
        // artwork is already visible throughout the slower initialization.
        mounted = tab5_storage_mount() == ESP_OK;
        lock_wallpaper_image_t startup_image{};
        if(mounted){
            uint64_t image_deadline=now_ms()+15000;
            startup_image=startup_artwork_load([](void *p,bool){return now_ms()<*static_cast<const uint64_t*>(p);},&image_deadline);
            if(startup_image.pixels){
                while(!tab5_display_lock(1000))vTaskDelay(pdMS_TO_TICKS(10));
                startup.artwork(startup_image,&aurora_sans18);tab5_display_unlock();
                ESP_LOGI(TAG,"BOOT artwork loaded from /Aurora/startup.jpg or startup.png");
            }else ESP_LOGI(TAG,"BOOT artwork unavailable (%s); keeping original startup text",lock_wallpaper_error_text(startup_image.error));
        }
        // Never light the temporary text/empty LVGL screen while SD artwork
        // is being decoded. The first visible frame is the final logo, or the
        // unchanged text fallback when no usable image exists on the card.
        while(!tab5_display_lock(1000))vTaskDelay(pdMS_TO_TICKS(10));
        const auto first_frame=tab5_display_reveal(70);tab5_display_unlock();
        if(first_frame!=ESP_OK){
            for(;;){ESP_LOGE(TAG,"First startup frame failed: %s",esp_err_to_name(first_frame));vTaskDelay(pdMS_TO_TICKS(5000));}
        }
        ESP_LOGI(TAG,"BOOT first visible frame: %s",startup_image.pixels?"SD artwork":"original text fallback");
        ESP_LOGI(TAG, "BOOT 06: creating player interface");
        lv_obj_t *main_screen=nullptr;
        // Build on an inactive screen. No uninitialized controls or private
        // library contents become visible, and the animation timer stays idle.
        while(!tab5_display_lock(1000))vTaskDelay(pdMS_TO_TICKS(10));
        {
            main_screen=lv_obj_create(nullptr);
            lv_obj_set_style_bg_color(main_screen,lv_color_black(),0);
            player_ui_create(main_screen, ui_action, nullptr);
            player_ui_set_overlay(true);
            lock_preferences_ui();
            tab5_display_unlock();
        }
        if(!player_ui_root()){
            for(;;){
                if(tab5_display_lock(1000)){startup.stalled();startup.progress("Not enough memory to create the player. Restart the device.");tab5_display_unlock();}
                if(startup.restart_requested())esp_restart();
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        ESP_LOGI(TAG, "BOOT 07: interface ready; initializing clock and SD");
        const auto keyboard_status=tab5_keyboard_init();
        if(keyboard_status!=ESP_OK)ESP_LOGW(TAG,"Keyboard unavailable: %s",esp_err_to_name(keyboard_status));
        setenv("TZ", "UTC0", 1); tzset(); // RTC is explicitly device-local civil time.
        const auto charger=tab5_charger_init();if(charger!=ESP_OK)ESP_LOGW(TAG,"Charger init: %s",esp_err_to_name(charger));
        const auto jack=tab5_headphones_init();if(jack!=ESP_OK)ESP_LOGE(TAG,"Jack detection: %s",esp_err_to_name(jack));
        tab5_sensors_init(); struct tm rtc{};
        if (tab5_rtc_read(&rtc) == ESP_OK) { timeval v{mktime(&rtc),0}; settimeofday(&v, nullptr); clock_valid = true; }
        load_persistence();
        audio_engine_set_visuals_enabled(false);
        // The old manual LOCAL setting remains in the on-card format only for
        // compatibility. Actual routing always follows a live BT connection.
        settings.local_output = 1;
        if (!player::valid_music_path(settings.last_path, MUSIC)) { settings.last_path[0] = 0; settings.resume_ms = 0; }
        tab5_brightness_set(settings.brightness);
        audio_output_t local{nullptr, tab5_audio_configure, tab5_audio_write, tab5_audio_volume}, output = local;
        bt_ready = bluetooth_bridge_init(&local, &output) == ESP_OK;
        if (bt_ready) {
            bluetooth_bridge_set_local_output(settings.local_output); bluetooth_bridge_set_enabled(settings.bt_enabled);
            bt_bridge_device_t remembered[8]{};
            for (unsigned i = 0; i < settings.receiver_count; ++i) {
                snprintf(remembered[i].mac, sizeof remembered[i].mac, "%s", settings.receivers[i].mac);
                snprintf(remembered[i].name, sizeof remembered[i].name, "%s", settings.receivers[i].name);
                remembered[i].saved = true;
            }
            bluetooth_bridge_restore_devices(remembered, settings.receiver_count);
        }
        audio_ready = audio_engine_init(&output) == ESP_OK;
        if (audio_ready) { apply_volume(); audio_engine_set_effect(settings.effect);audio_engine_set_eq(&eq_bank.current);audio_engine_set_reverb(&reverb_config);audio_engine_set_speed(settings.speed_percent); }
        player_artwork_init([](void *) {
            if (!artwork_io_allowed.load(std::memory_order_relaxed)) return false;
            audio_snapshot_t snapshot{}; audio_engine_get_snapshot(&snapshot);
            return snapshot.state != AUDIO_LOADING && snapshot.state != AUDIO_SEEKING &&
                (snapshot.state != AUDIO_PLAYING || snapshot.buffered_blocks >= 4);
        }, nullptr);
        if(audio_ready&&tab5_display_lock(1000)){
            player_ui_set_visual_source([](float *bands,int16_t *wave,void*){
                audio_snapshot_t sample{};audio_engine_get_snapshot(&sample);
                if(sample.state!=AUDIO_PLAYING)return false;
                memcpy(bands,sample.spectrum,sizeof sample.spectrum);memcpy(wave,sample.waveform,sizeof sample.waveform);return true;
            },nullptr);tab5_display_unlock();
        }
        scan();
        // Wait for the actual catalog worker, not a fixed splash delay. The
        // worker owns SD access and yields; LVGL continues drawing this screen.
        // On missing/unreadable SD or task-allocation failure scan_job is null
        // after its error has been preserved in message, so boot still finishes.
        uint64_t startup_progress_at=now_ms(),startup_update_at=0;
        unsigned startup_visited=UINT32_MAX,startup_found=UINT32_MAX;
        bool startup_stalled=false;
        while(scan_job){
            const uint64_t now=now_ms();
            const unsigned visited=scan_job->visited.load(std::memory_order_relaxed);
            const unsigned found=scan_job->found.load(std::memory_order_relaxed);
            if(visited!=startup_visited||found!=startup_found){
                startup_visited=visited;startup_found=found;startup_progress_at=now;startup_stalled=false;
            }
            poll_scan(now);
            if(scan_job && now-startup_update_at>=250 && tab5_display_lock(1000)){
                if(now-startup_progress_at>=180000){
                    startup.stalled();
                    if(!startup_stalled)ESP_LOGW(TAG,"BOOT waiting for SD; restart is available on startup screen");
                    startup_stalled=true;
                }
                else{
                    startup.resumed();
                    char progress[160];
                    if(scan_job->scanning.load(std::memory_order_relaxed))snprintf(progress,sizeof progress,"Loading music library: %u tracks / %u items checked",found,visited);
                    else snprintf(progress,sizeof progress,"Loading saved music library...");
                    startup.progress(progress);
                }
                tab5_display_unlock();startup_update_at=now;
            }
            if(startup.restart_requested())esp_restart();
            tab5_keyboard_event_t discarded{};
            for(unsigned i=0;i<64&&tab5_keyboard_read_event(&discarded);++i){}
            vTaskDelay(pdMS_TO_TICKS(startup_stalled?500:20));
        }
        // Populate the final UI before making it public. With an enabled PIN,
        // the backlight is off only for the capture + mandatory-overlay swap.
        if(audio_ready)audio_engine_get_snapshot(&audio);
        render(now_ms());
        uint16_t *boot_capture=screen_lock.locked()?lock_alloc_capture():nullptr;
        if(screen_lock.locked())tab5_brightness_set(0);
        while(!tab5_display_lock(1000))vTaskDelay(pdMS_TO_TICKS(10));
        lv_screen_load(main_screen);
        if(screen_lock.locked()){
            lock_flow=LockFlow::Unlock;
            lock_capture_main_ui(boot_capture);boot_capture=nullptr;lock_image_pending=true;
            lock_show_pin_ui();
            tab5_display_sleep(true,settings.brightness);
            tab5_display_sleep(false,settings.brightness);
        }else player_ui_set_overlay(false);
        for(auto *input=lv_indev_get_next(nullptr);input;input=lv_indev_get_next(input))
            if(lv_indev_get_type(input)==LV_INDEV_TYPE_POINTER)lv_indev_wait_release(input);
        lock_clear_queued_input();
        tab5_keyboard_event_t discarded{};
        for(unsigned i=0;i<64&&tab5_keyboard_read_event(&discarded);++i){}
        lv_display_trigger_activity(lv_display_get_default());
        lock_input_gate.store(screen_lock.locked(),std::memory_order_release);
        startup.destroy();tab5_display_unlock();
        heap_caps_free(boot_capture);lock_wallpaper_image_free(&startup_image);
        if (!audio_ready) notice("Audio initialization failed. Restart and inspect serial log.");
        if (!tab5_touch_ready() && !screen_lock.locked() && tab5_display_lock(1000)) {
            information("TOUCH INITIALIZATION FAILED", tab5_display_last_error());
            tab5_display_unlock();
        }
        ESP_LOGI(TAG, "BOOT 08: controls ready; SD=%d audio=%d touch=%d; initial library ready (%u tracks), PIN=%d", mounted, audio_ready, tab5_touch_ready(),unsigned(library.tracks.size()),screen_lock.locked());
        last_save = now_ms();
#if AURORA_TAP_CONTROLS
        if(!player::sleep_tap_start())ESP_LOGW(TAG,"Tap input worker unavailable; player remains usable");
#else
        // Built with -DAURORA_TAP_CONTROLS=0 (1.3.19/1.3.20 default, see
        // docs/AUDIT-1.3.18.md). Without the worker every sleep_tap_* call is
        // a no-op and the IMU is never powered.
        ESP_LOGI(TAG,"Tap controls disabled in this build");
#endif
        for (;;) {
            uint64_t now = now_ms();
            poll_scan(now);
            if (shutdown_requested) {
                if (!scan_job && !shutdown_attempted) { shutdown_attempted = true; perform_shutdown(); }
                restore_shutdown_error();
                Command pending{};
                while (xQueueReceive(commands, &pending, 0) == pdTRUE) action(pending);
                vTaskDelay(pdMS_TO_TICKS(25));
                continue;
            }
            if (audio_ready) audio_engine_get_snapshot(&audio);
            // An accepted resume is asynchronous. A stale PAUSED snapshot must
            // not authorize SD writes before the decoder handles that command.
            if (resume_pending && audio.state != AUDIO_PAUSED) resume_pending = false;
            if (audio.event_sequence != last_event) {
                last_event = audio.event_sequence;
                if (audio.event == AUDIO_EVENT_LOADED) {
                    if (track() && track()->path == audio.path) play_pending = false;
                    apply_loaded_metadata(); notice("");
                }
                else if (audio.event == AUDIO_EVENT_ENDED && track() && track()->path == audio.path) { apply_loaded_metadata(); settings.resume_ms = 0; next(true); mark_dirty(); }
                else if (audio.event == AUDIO_EVENT_ERROR) {
                    if (track() && track()->path == audio.path) play_pending = false;
                    notice(std::string("Audio: ") + audio.error);
                }
            }
            if (pending_resume && track() && track()->path == audio.path && audio.seek_supported && audio.state != AUDIO_ERROR && audio.state != AUDIO_ENDED) {
                const uint32_t target = audio.duration_exact && audio.duration_ms ? std::min(pending_resume, audio.duration_ms - 1) : pending_resume;
                if (audio_engine_seek_ms(target) == ESP_OK) pending_resume = 0;
            }
            Command command{};
            poll_keyboard();
            if(!lock_active())poll_eq_edits();
            while (xQueueReceive(commands, &command, 0) == pdTRUE) action(command);
            int volume=latest_volume.exchange(-1);if(volume>=0){Command c{};c.action=PLAYER_UI_VOLUME;c.value=volume;action(c);}
            if (shutdown_requested) continue;
            now = now_ms();
            screen_power();poll_screenshot();poll_bluetooth(now);poll_lock(now);
            poll_sleep_taps();
            if(dialog_kind==DIALOG_EQ && audio.sample_rate!=eq_display_rate){eq_display_rate=audio.sample_rate;eq_refresh();}
            // Meter updates touch only its label, never redraw the sliders.
            if(dialog_kind==DIALOG_EQ && now-last_eq_meter>=100 && tab5_display_lock(2)){
                if(dialog_kind==DIALOG_EQ)eq_dialog_meter(dialog,audio.eq_reduction_db,
                    audio.effect==3 && audio.state==AUDIO_PLAYING);
                tab5_display_unlock();last_eq_meter=now;
            }
            audio_engine_set_visuals_enabled(settings.animations && !dimmed && !dialog && !lock_active());
            artwork_io_allowed.store(mounted && !dimmed && !dialog && !lock_active() && !lock_job && !screenshot_job && !scan_job && !play_pending && !resume_pending, std::memory_order_relaxed);
            if (now - last_sensor >= 250) {
                battery = tab5_battery_status_read(&battery_status) == ESP_OK && battery_status.present ? battery_status.estimated_percent : -1;
                last_sensor = now;
            }
            const bool playing = audio.state == AUDIO_PLAYING;
            if (!dimmed && !dialog && !lock_active() && now - last_state >= (playing ? 250U : 500U)) render(now);
            service_artwork(now);
            // Poll completed cover results promptly. FFT rendering is driven
            // on the LVGL timer, never delayed by catalog/controller work.
            if(!dimmed&&!dialog&&!lock_active()&&now-last_visual>=30&&tab5_display_lock(2)){
                player_artwork_poll();tab5_display_unlock();last_visual=now;
            }
            // Both automatic save paths must honor failure backoff, including
            // periodic saves whose last successful write is already overdue.
            if (mounted && now >= save_retry_after &&
                ((dirty && now >= dirty_since && now - dirty_since >= 1500) || (playing && now - last_save >= 30000))) save();
            if (mounted && !scan_job && !play_pending && !resume_pending && now - last_metadata_save >= 2000 &&
                (audio.state == AUDIO_PAUSED || audio.state == AUDIO_STOPPED || audio.state == AUDIO_ENDED)) {
                // Persist one learned record while audio is idle; do not contend
                // with playback or rewrite thousands of unrelated cache records.
                library.flush_metadata_cache(1); last_metadata_save = now;
                if (favorites.dirty() && now >= favorites_retry_after) {
                    if (storage_layout.persistent_read_error || !favorites.save()) {
                        favorites_retry_after = now + 10000;
                        notice("Favorites write failed; changes remain in memory. Retry on pause or POWER OFF.");
                    } else favorites_retry_after = 0;
                }
            }
            profile(now);
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
};
}
extern "C" void app_main() {
    ESP_LOGI(TAG, "BOOT 00: Orbital %s, CPU %d MHz, reset=%d", esp_app_get_description()->version, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, esp_reset_reason());
    auto *player = new Player;
    if (xTaskCreatePinnedToCore([](void *p) { static_cast<Player *>(p)->run(); }, "player_app", 16384, player, 3, nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create player task"); delete player;
    }
}
