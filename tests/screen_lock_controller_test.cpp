#undef NDEBUG /* zig -O1/-O2 predefines NDEBUG; keep assert() checks active */
// Reuse the SDK/storage/crypto fixtures. The runner executes the independent
// model suite separately; flows below use the real production controller.
#define main screen_lock_core_test_entry
#include "screen_lock_test.cpp"
#undef main
#include "player_ui.h"
#include "lock_wallpaper.h"
#include <atomic>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <new>

struct Command { int action=0,value=0; };
static std::deque<Command> queued;
static int commands=1;
static std::atomic_bool lock_input_gate{false};
static std::atomic_int latest_volume{-1};
static unsigned eq_input_read=0,eq_input_count=0;
static int eq_input_mux=0;
static bool display_available=true;
static unsigned display_wait_ms=0;
static uint64_t fake_now=0;
static unsigned ui_count=0,ui_close_count=0,settings_open_count=0;
static bool ui_visible=false,ui_cancel=false,ui_enabled=false;
static std::string ui_title,ui_help;
static const lv_image_dsc_t *ui_background=nullptr;
static constexpr int pdTRUE=1,pdPASS=1,MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(value) (value)
static void portENTER_CRITICAL(int *) {}
static void portEXIT_CRITICAL(int *) {}
static uint64_t now_ms() { return fake_now; }
static void vTaskDelay(uint32_t amount) { fake_now+=amount; }
static void vTaskDelete(void *) {}
static int xQueueReceive(int,Command *out,unsigned) {
    if(queued.empty())return 0;
    *out=queued.front();queued.pop_front();return pdTRUE;
}
static int xTaskCreatePinnedToCore(void (*)(void *),const char *,unsigned,void *,unsigned,void *,unsigned) { return 0; }
static void *heap_caps_malloc(size_t bytes,int) { return malloc(bytes); }
static size_t heap_caps_get_free_size(int) { return 16U*1024U*1024U; }
static void heap_caps_free(void *memory) { free(memory); }
static bool tab5_display_lock(unsigned wait_ms) { display_wait_ms=wait_ms;return display_available; }
static void tab5_display_unlock() {}
static int tab5_display_snapshot_rgb565(uint16_t *,size_t) { return ESP_OK; }
static void close_dialog() {}
static lv_display_t *lv_display_get_default() { static lv_display_t display;return &display; }
static void lv_display_trigger_activity(lv_display_t *) {}
static void lv_image_cache_drop(lv_image_dsc_t *) {}
enum { AUDIO_LOADING,AUDIO_SEEKING,AUDIO_PLAYING,AUDIO_STOPPED };
struct audio_snapshot_t {int state=AUDIO_STOPPED;unsigned buffered_blocks=8;};
static void audio_engine_get_snapshot(audio_snapshot_t *out) { *out={}; }
static tm *test_localtime_r(const time_t *stamp,tm *out) { *out=*std::localtime(stamp);return out; }
#define localtime_r test_localtime_r

void player_ui_set_lock_preferences(bool,const char *) {}
void player_ui_set_lock_background(const lv_image_dsc_t *image) { ui_background=image; }
void player_ui_show_lock_pin(const char *title,const char *help,unsigned count,bool cancel,bool enabled) {
    ui_visible=true;ui_title=title;ui_help=help;ui_count=count;ui_cancel=cancel;ui_enabled=enabled;
}
void player_ui_show_lock_backgrounds(const char *const *,size_t,int,const char *status) { ui_visible=true;ui_help=status; }
void player_ui_prepare_lock_capture() { ui_visible=false; }
void player_ui_close_lock_panel() { ui_visible=false;++ui_close_count; }
void player_ui_open_display_settings() { ++settings_open_count; }
void player_ui_set_lock_clock(const char *) {}
void lock_wallpaper_list(lock_wallpaper_list_t *,lock_wallpaper_continue_cb,void *) {}
lock_wallpaper_image_t lock_wallpaper_frosted(const uint16_t *,size_t,lock_wallpaper_continue_cb,void *) { return {}; }
lock_wallpaper_image_t lock_wallpaper_load(const char *,lock_wallpaper_continue_cb,void *) { return {}; }
void lock_wallpaper_image_free(lock_wallpaper_image_t *image) { if(image){free(image->pixels);*image={};} }
const char *lock_wallpaper_error_text(lock_wallpaper_error_t) { return "Image error"; }

struct Controller {
    bool keyboard_event_pending=false,mounted=true,shutdown_requested=false,dimmed=false,clock_valid=false;
    struct { bool clock24=true; } settings;
    void *scan_job=nullptr,*screenshot_job=nullptr;
    uint64_t last_state=0;
    std::string notice_text;
    void notice(const std::string &text) { notice_text=text; }
#include "aurora_lock_controller.inc"
    Controller() {
        nvs_bytes.clear();nvs_exists=false;
        private_bytes.clear();private_exists=partition_present=corrupt_readback=false;markers[0]=markers[1]=false;raw_private.fill(0xff);
        required_markers[0]=required_markers[1]=staged_required[0]=staged_required[1]=false;
        staged[0]=staged[1]=staged_marker[0]=staged_marker[1]=false;private_init_result=private_write_result=ESP_OK;
        nvs_init_result=nvs_open_result=nvs_read_result=nvs_write_result=nvs_commit_result=ESP_OK;
        display_available=true;fake_now=0;queued.clear();lock_input_gate=false;latest_volume=-1;
        eq_input_read=eq_input_count=0;ui_count=ui_close_count=settings_open_count=0;
        ui_visible=ui_cancel=ui_enabled=false;ui_title.clear();ui_help.clear();ui_background=nullptr;
        assert(screen_lock.load()==LockLoadResult::Missing);
    }
    ~Controller() {
        if(lock_job)lock_free_job(lock_job);
        heap_caps_free(lock_capture);lock_wallpaper_image_free(&lock_frost);lock_wallpaper_image_free(&lock_custom);
    }
    void send(int action,int value=0) { assert(lock_action(Command{action,value})); }
    void pin(const char *text) { for(;*text;++text)send(PLAYER_UI_LOCK_INPUT,*text-'0'); }
    void setup(const char *text="1234") {
        send(PLAYER_UI_LOCK_ENABLE);pin(text);pin(text);
        assert(screen_lock.enabled()&&!screen_lock.locked()&&!lock_active());
    }
    bool secrets_empty() const {
        for(char c:lock_digits)if(c)return false;
        for(char c:lock_old_pin)if(c)return false;
        for(char c:lock_new_pin)if(c)return false;
        return lock_digit_count==0;
    }
};

static void controller_setup_and_confirmation() {
    Controller p;
    queued.push_back({PLAYER_UI_VOLUME,99});queued.push_back({PLAYER_UI_SETTINGS,0});
    latest_volume=99;eq_input_count=3;p.keyboard_event_pending=true;
    p.send(PLAYER_UI_LOCK_ENABLE);
    assert(p.lock_flow==Controller::LockFlow::Setup && lock_input_gate && queued.empty());
    assert(latest_volume==-1 && !eq_input_count && !p.keyboard_event_pending);
    assert(ui_visible&&ui_cancel&&ui_enabled&&ui_count==0);
    p.pin("0000");
    assert(p.lock_flow==Controller::LockFlow::ConfirmSetup&&!p.screen_lock.enabled());
    p.pin("0001");
    assert(!p.screen_lock.enabled() && p.lock_digit_count==0 && !p.lock_hint.empty());
    queued.push_back({PLAYER_UI_SETTINGS,0});latest_volume=88;
    p.pin("0000");
    assert(p.screen_lock.enabled()&&!p.screen_lock.locked()&&!p.lock_active());
    assert(!lock_input_gate&&!ui_visible&&p.secrets_empty()&&queued.empty()&&latest_volume==-1);
    assert(settings_open_count==1);
    p.lock_begin(Controller::LockFlow::Unlock,false);
    assert(p.screen_lock.locked()&&!ui_cancel&&ui_enabled);
    p.send(PLAYER_UI_LOCK_INPUT,-2);
    assert(p.screen_lock.locked()&&p.lock_flow==Controller::LockFlow::Unlock);
    p.send(PLAYER_UI_LOCK_DISABLE); // stale/forged settings command cannot replace mandatory unlock
    assert(p.lock_flow==Controller::LockFlow::Unlock);
    p.pin("0001");assert(p.screen_lock.locked()&&p.lock_digit_count==0);
    p.pin("0000");assert(!p.screen_lock.locked()&&!p.lock_active()&&p.secrets_empty());
}

static void controller_change_disable_cancel() {
    Controller p;p.setup();
    p.send(PLAYER_UI_LOCK_CHANGE);p.pin("9999");
    assert(p.lock_flow==Controller::LockFlow::ChangeAuth);
    p.pin("1234");assert(p.lock_flow==Controller::LockFlow::ChangeNew);
    p.pin("5678");assert(p.lock_flow==Controller::LockFlow::ChangeConfirm);
    p.pin("5679");assert(p.lock_flow==Controller::LockFlow::ChangeConfirm);
    p.pin("5678");assert(!p.lock_active()&&p.secrets_empty());
    p.lock_begin(Controller::LockFlow::Unlock,false);p.pin("1234");assert(p.screen_lock.locked());
    p.pin("5678");assert(!p.screen_lock.locked());
    p.send(PLAYER_UI_LOCK_DISABLE);p.pin("1234");assert(p.screen_lock.enabled());
    p.send(PLAYER_UI_LOCK_INPUT,-2);assert(!p.lock_active()&&p.screen_lock.enabled()&&p.secrets_empty());
    p.send(PLAYER_UI_LOCK_DISABLE);p.pin("5678");
    assert(!p.screen_lock.enabled()&&!p.lock_active()&&p.secrets_empty());
    p.send(PLAYER_UI_LOCK_ENABLE);p.pin("0012");p.send(PLAYER_UI_LOCK_INPUT,-2);
    assert(!p.screen_lock.enabled()&&p.secrets_empty());
}

static void controller_timeout_cooldown_and_errors() {
    Controller p;p.setup();
    p.send(PLAYER_UI_LOCK_CHANGE);p.pin("1234");p.pin("1111");
    assert(!p.secrets_empty());
    // This is the production transition called by screen_power on idle expiry.
    p.lock_begin(Controller::LockFlow::Unlock,false);
    assert(p.screen_lock.locked()&&p.secrets_empty()&&!ui_cancel);
    for(unsigned i=0;i<5;++i)p.pin("9999");
    assert(p.screen_lock.retry_after_ms(fake_now)==30000&&!ui_enabled);
    p.pin("1234");assert(p.screen_lock.locked()&&p.lock_digit_count==0);
    fake_now=29999;p.pin("1234");assert(p.screen_lock.locked());
    fake_now=30000;p.pin("1234");assert(!p.screen_lock.locked()&&p.secrets_empty());
    p.send(PLAYER_UI_LOCK_DISABLE);private_write_result=ESP_FAIL;p.pin("1234");
    assert(p.screen_lock.enabled()&&p.lock_flow==Controller::LockFlow::DisableAuth);
    private_write_result=ESP_OK;p.pin("1234");assert(!p.screen_lock.enabled());
    p.setup();nvs_read_result=ESP_FAIL;
    assert(p.screen_lock.load()==LockLoadResult::Error);
    p.lock_begin(Controller::LockFlow::Unlock,false);
    assert(!ui_enabled&&!ui_cancel);p.pin("1234");assert(p.screen_lock.locked());
}

static void controller_display_failure_and_sd_missing() {
    Controller p;p.setup();
    display_available=false;p.lock_begin(Controller::LockFlow::Unlock,false);
    assert(p.screen_lock.locked()&&p.lock_active()&&lock_input_gate);
    assert(p.lock_present_pending&&!ui_visible);
    display_available=true;p.poll_lock(fake_now);
    assert(ui_visible&&!ui_cancel&&!p.lock_present_pending);
    display_available=false;p.pin("1");
    assert(p.lock_pin_pending&&p.lock_digit_count==1&&display_wait_ms<=20&&ui_count==0);
    display_available=true;p.poll_lock(fake_now);
    assert(!p.lock_pin_pending&&ui_count==1);
    p.pin("23");display_available=false;p.pin("4");
    assert(!p.screen_lock.locked()&&p.lock_active()&&p.lock_exit_pending&&lock_input_gate);
    assert(p.secrets_empty());
    queued.push_back({PLAYER_UI_VOLUME,88});
    display_available=true;p.poll_lock(fake_now);
    assert(!p.lock_active()&&!p.lock_exit_pending&&!lock_input_gate&&queued.empty());
    display_available=false;p.send(PLAYER_UI_LOCK_BACKGROUND);
    assert(p.lock_present_pending&&!ui_visible);
    display_available=true;p.poll_lock(fake_now);
    assert(ui_visible&&!p.lock_present_pending);
    p.send(PLAYER_UI_LOCK_INPUT,-2);
    assert(!p.lock_active());
    p.mounted=false;p.send(PLAYER_UI_LOCK_BACKGROUND);p.poll_lock(fake_now);
    assert(!p.lock_list_pending&&!p.lock_job);
    assert(p.lock_list_status.find("unavailable")!=std::string::npos);
    p.send(PLAYER_UI_LOCK_INPUT,-2);assert(!p.lock_active());
}

static void controller_worker_publication_and_stop() {
    Controller p;p.setup();
    heap_caps_free(p.lock_capture);p.lock_capture=nullptr;p.lock_image_pending=false;
    assert(p.screen_lock.set_wallpaper("old.png")==LockResult::Ok);
    auto *old_pixels=static_cast<uint16_t*>(malloc(2));*old_pixels=0x1234;
    p.lock_custom={old_pixels,1,1,LOCK_WALLPAPER_OK};p.lock_loaded_name="old.png";
    assert(p.screen_lock.set_wallpaper("new.png")==LockResult::Ok);
    auto *j=new Controller::LockJob;
    j->kind=Controller::LockJob::Image;std::strcpy(j->name,"new.png");
    auto *new_pixels=static_cast<uint16_t*>(malloc(2));*new_pixels=0x5678;
    j->image={new_pixels,1,1,LOCK_WALLPAPER_OK};j->done=true;p.lock_job=j;
    display_available=false;p.poll_lock(fake_now);
    assert(p.lock_job==j&&j->image.pixels==new_pixels&&p.lock_custom.pixels==old_pixels&&p.lock_loaded_name=="old.png");
    display_available=true;p.poll_lock(fake_now);
    assert(!p.lock_job&&p.lock_custom.pixels==new_pixels&&p.lock_loaded_name=="new.png");
    p.send(PLAYER_UI_LOCK_BACKGROUND);
    heap_caps_free(p.lock_capture);p.lock_capture=nullptr;p.lock_list_pending=false;p.lock_image_pending=false;
    j=new Controller::LockJob;j->kind=Controller::LockJob::List;j->list=new lock_wallpaper_list_t;p.lock_job=j;
    p.send(PLAYER_UI_LOCK_BACKGROUND_SELECT,-1);
    assert(!j->cancel); // choosing frosted must not abandon the directory listing
    assert(!p.lock_stop_worker(30)&&p.lock_job==j&&j->cancel);
    j->done=true;assert(p.lock_stop_worker(30)&&!p.lock_job);
}

int main() {
    controller_setup_and_confirmation();
    controller_change_disable_cancel();
    controller_timeout_cooldown_and_errors();
    controller_display_failure_and_sd_missing();
    controller_worker_publication_and_stop();
    std::puts("Screen lock controller tests passed: real setup/change/disable/unlock flows, stale queues, secret cleanup, idle lock, cooldown, I/O failures, UI retries, absent SD, image publication and worker ownership.");
}
