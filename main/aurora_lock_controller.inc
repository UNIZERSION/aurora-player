// Included inside Player. Only the controller owns credentials and jobs.
// All storage/decoding runs outside LVGL; workers publish immutable results.
player::EspScreenLockStore lock_store;
player::EspScreenLockCrypto lock_crypto;
player::ScreenLock screen_lock{lock_store,lock_crypto};
enum class LockFlow { None, Setup, ConfirmSetup, ChangeAuth, ChangeNew, ChangeConfirm, DisableAuth, Unlock, Backgrounds };
LockFlow lock_flow=LockFlow::None;
char lock_digits[5]{},lock_new_pin[5]{},lock_old_pin[5]{};
unsigned lock_digit_count=0;
uint32_t lock_last_countdown=UINT32_MAX;
uint64_t lock_clock_minute=UINT64_MAX;
bool lock_present_pending=false,lock_pin_pending=false,lock_exit_pending=false,lock_exit_settings=false;
std::string lock_exit_status;
std::string lock_hint;
struct LockJob {
    enum Kind { List, Frost, Image } kind=Frost;
    std::atomic_bool cancel{false},done{false},allow_io{false};
    uint16_t *snapshot=nullptr;
    lock_wallpaper_list_t *list=nullptr;
    lock_wallpaper_image_t image{};
    char name[LOCK_WALLPAPER_NAME_BYTES]{};
    uint64_t started=0,elapsed=0;
};
LockJob *lock_job=nullptr;
uint16_t *lock_capture=nullptr;
lock_wallpaper_image_t lock_frost{},lock_custom{};
lv_image_dsc_t lock_frost_desc{},lock_custom_desc{};
std::vector<std::string> lock_names;
bool lock_list_pending=false,lock_list_ready=false,lock_image_pending=false;
std::string lock_loaded_name,lock_list_status;

bool lock_active() const { return lock_flow!=LockFlow::None || screen_lock.locked(); }
static void lock_wipe(char *p,size_t n) { volatile char *v=p;while(n--)*v++=0; }
void lock_clear_digits() {lock_wipe(lock_digits,sizeof lock_digits);lock_digit_count=0;}
void lock_clear_secrets() {lock_clear_digits();lock_wipe(lock_new_pin,sizeof lock_new_pin);lock_wipe(lock_old_pin,sizeof lock_old_pin);}
void lock_clear_queued_input() {
    latest_volume.store(-1);
    Command discard{};while(xQueueReceive(commands,&discard,0)==pdTRUE)lock_wipe(reinterpret_cast<char*>(&discard),sizeof discard);
    portENTER_CRITICAL(&eq_input_mux);eq_input_read=eq_input_count=0;portEXIT_CRITICAL(&eq_input_mux);
    keyboard_event_pending=false;
}
void lock_preferences_ui() {player_ui_set_lock_preferences(screen_lock.enabled(),screen_lock.wallpaper().c_str());}
void lock_choose_background_ui() {
    const bool custom=!screen_lock.wallpaper().empty() && lock_loaded_name==screen_lock.wallpaper() && lock_custom.pixels;
    player_ui_set_lock_background(custom?&lock_custom_desc:lock_frost.pixels?&lock_frost_desc:nullptr);
}
void lock_show_pin_ui() {
    const char *title="Enter PIN",*help="Enter your 4-digit PIN to unlock";
    switch(lock_flow){
    case LockFlow::Setup:case LockFlow::ChangeNew:title="Create PIN";help="Choose four digits. Remember your PIN.";break;
    case LockFlow::ConfirmSetup:case LockFlow::ChangeConfirm:title="Confirm PIN";help="Enter the same four digits again";break;
    case LockFlow::ChangeAuth:title="Change PIN";help="Enter your current PIN";break;
    case LockFlow::DisableAuth:title="Disable screen lock";help="Enter your current PIN to turn off the lock";break;
    default:break;
    }
    const auto wait=screen_lock.retry_after_ms(now_ms());
    char countdown[96];
    if(wait){snprintf(countdown,sizeof countdown,"Too many attempts. Try again in %lu sec.",static_cast<unsigned long>((wait+999)/1000));help=countdown;}
    else if(screen_lock.storage_error())help="PIN storage could not be read. Restart to retry. The screen stays locked.";
    else if(!lock_hint.empty())help=lock_hint.c_str();
    player_ui_show_lock_pin(title,help,lock_digit_count,lock_flow!=LockFlow::Unlock,
        !wait&&!screen_lock.storage_error());
    lock_last_countdown=(wait+999)/1000;
    lock_pin_pending=false;
}
void lock_show_list_ui() {
    lock_clock_minute=UINT64_MAX;
    std::vector<const char*> names;names.reserve(lock_names.size());int selected=-1;
    for(size_t i=0;i<lock_names.size();++i){names.push_back(lock_names[i].c_str());if(lock_names[i]==screen_lock.wallpaper())selected=int(i);}
    player_ui_show_lock_backgrounds(names.data(),names.size(),selected,lock_list_status.c_str());
}
// Capture only while the main panel is already public (before locking or during
// settings). Boot calls this with backlight off, before showing the first PIN.
void lock_capture_main_ui(uint16_t *pixels) {
    close_dialog();player_ui_prepare_lock_capture();
    if(pixels && tab5_display_snapshot_rgb565(pixels,1280*720)==ESP_OK){
        heap_caps_free(lock_capture);lock_capture=pixels;
    }else heap_caps_free(pixels);
}
uint16_t *lock_alloc_capture() {
    if(heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)<1280*720*2+2*1024*1024)return nullptr;
    return static_cast<uint16_t*>(heap_caps_malloc(1280*720*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
}
void lock_begin(LockFlow next,bool capture=true) {
    lock_input_gate.store(true,std::memory_order_release);
    lock_clear_queued_input();lock_clear_secrets();lock_hint.clear();lock_flow=next;
    lock_present_pending=true;lock_pin_pending=false;lock_exit_pending=false;lock_exit_status.clear();lock_clock_minute=UINT64_MAX;
    if(next==LockFlow::Unlock)screen_lock.lock();
    if(next!=LockFlow::Backgrounds)lock_list_pending=false;
    // Do not compete with a large PNG allocation. Cancellation is cooperative;
    // cached frosted pixels remain a safe fallback if a worker is still closing.
    const bool idle=lock_stop_worker(250);
    auto *pixels=capture&&idle?lock_alloc_capture():nullptr;
    if(tab5_display_lock(1000)){
        if(capture)lock_capture_main_ui(pixels);else {heap_caps_free(pixels);close_dialog();}
        lock_preferences_ui();lock_choose_background_ui();
        if(next==LockFlow::Backgrounds)lock_show_list_ui();else lock_show_pin_ui();
        lock_present_pending=false;
        tab5_display_unlock();
    }else heap_caps_free(pixels);
    lock_image_pending=true;
    ESP_LOGI(TAG,"LOCK flow=%u enabled=%d locked=%d",unsigned(next),screen_lock.enabled(),screen_lock.locked());
}
void lock_finish(bool settings_return,const char *status=nullptr) {
    // Keep the input gate closed until old queued gestures have been discarded
    // and the mandatory overlay has actually disappeared.
    if(!lock_exit_pending){lock_exit_settings=settings_return;lock_exit_status=status?status:"";}
    lock_exit_pending=true;
    lock_clear_secrets();
    if(!tab5_display_lock(1000))return;
    lock_clear_queued_input();lock_clear_secrets();lock_flow=LockFlow::None;lock_hint.clear();
    player_ui_close_lock_panel();lock_preferences_ui();
    if(lock_exit_settings)player_ui_open_display_settings();
    lv_display_trigger_activity(lv_display_get_default());
    lock_input_gate.store(false,std::memory_order_release);tab5_display_unlock();
    last_state=0;
    lock_present_pending=lock_pin_pending=lock_exit_pending=false;
    if(!lock_exit_status.empty())notice(lock_exit_status);
    lock_exit_status.clear();
    ESP_LOGI(TAG,"LOCK unlocked enabled=%d",screen_lock.enabled());
}
void lock_result(player::LockResult result) {
    lock_clear_digits();
    if(result==player::LockResult::WrongPin||result==player::LockResult::InvalidPin)lock_hint="Incorrect PIN. Try again.";
    else if(result==player::LockResult::StorageError)lock_hint="Could not save PIN settings. Please try again.";
    else if(result==player::LockResult::CryptoError)lock_hint="PIN check unavailable. Please restart and retry.";
    else if(result==player::LockResult::Cooldown)lock_hint.clear();
    else lock_hint="The change could not be completed. Please try again.";
}
void lock_digit(int value) {
    if(lock_flow==LockFlow::None||lock_exit_pending)return;
    if(value==-2){if(lock_flow!=LockFlow::Unlock)lock_finish(true);return;}
    if(lock_flow==LockFlow::Backgrounds||screen_lock.storage_error()||screen_lock.retry_after_ms(now_ms()))return;
    if(value==-1){if(lock_digit_count)lock_digits[--lock_digit_count]=0;}
    else if(value>=0&&value<=9&&lock_digit_count<4){lock_digits[lock_digit_count++]=char('0'+value);lock_digits[lock_digit_count]=0;}
    else return;
    if(lock_digit_count==4){
        using R=player::LockResult;R result=R::Ok;
        switch(lock_flow){
        case LockFlow::Setup:case LockFlow::ChangeNew:
            memcpy(lock_new_pin,lock_digits,sizeof lock_new_pin);lock_clear_digits();
            lock_flow=lock_flow==LockFlow::Setup?LockFlow::ConfirmSetup:LockFlow::ChangeConfirm;lock_hint.clear();break;
        case LockFlow::ConfirmSetup:case LockFlow::ChangeConfirm:
            if(strcmp(lock_digits,lock_new_pin)){lock_clear_digits();lock_hint="PINs do not match. Enter the new PIN again.";break;}
            result=lock_flow==LockFlow::ConfirmSetup?screen_lock.set_pin(lock_digits):screen_lock.change_pin(lock_old_pin,lock_digits,now_ms());
            if(result==R::Ok){lock_finish(true,"Screen lock PIN saved");return;}lock_result(result);break;
        case LockFlow::ChangeAuth:
            result=screen_lock.unlock(lock_digits,now_ms());
            if(result==R::Ok){memcpy(lock_old_pin,lock_digits,sizeof lock_old_pin);lock_clear_digits();lock_flow=LockFlow::ChangeNew;lock_hint.clear();}
            else lock_result(result);
            break;
        case LockFlow::DisableAuth:
            result=screen_lock.disable(lock_digits,now_ms());
            if(result==R::Ok){lock_finish(true,"Screen lock disabled");return;}lock_result(result);break;
        case LockFlow::Unlock:
            result=screen_lock.unlock(lock_digits,now_ms());
            if(result==R::Ok){lock_finish(false);return;}lock_result(result);break;
        default:break;
        }
    }
    // Do not hold up the controller/audio transport while LVGL finishes a
    // backdrop frame. The accepted digit stays in RAM and the view retries.
    lock_pin_pending=true;
    if(tab5_display_lock(20)){lock_show_pin_ui();tab5_display_unlock();}
}
bool lock_action(const Command &c) {
    if(c.action<PLAYER_UI_LOCK_ENABLE||c.action>PLAYER_UI_LOCK_INPUT)return false;
    if(c.action==PLAYER_UI_LOCK_INPUT){lock_digit(c.value);return true;}
    if(c.action==PLAYER_UI_LOCK_BACKGROUND_SELECT){
        if(lock_flow!=LockFlow::Backgrounds||c.value< -1||(c.value>=0&&size_t(c.value)>=lock_names.size()))return true;
        const std::string name=c.value<0?"":lock_names[c.value];
        if(screen_lock.set_wallpaper(name)!=player::LockResult::Ok){lock_list_status="Could not save the background. Please try again.";}
        else {
            if(lock_job&&lock_job->kind!=LockJob::List)lock_job->cancel.store(true);
            lock_loaded_name.clear();lock_image_pending=true;
            lock_list_status=name.empty()?"Frosted glass selected.":"Loading background...";
        }
        if(tab5_display_lock(1000)){lock_preferences_ui();lock_choose_background_ui();lock_show_list_ui();tab5_display_unlock();}
        return true;
    }
    if(lock_active())return true;
    switch(c.action){
    case PLAYER_UI_LOCK_ENABLE:if(!screen_lock.enabled())lock_begin(LockFlow::Setup);break;
    case PLAYER_UI_LOCK_CHANGE:if(screen_lock.enabled())lock_begin(LockFlow::ChangeAuth);break;
    case PLAYER_UI_LOCK_DISABLE:if(screen_lock.enabled())lock_begin(LockFlow::DisableAuth);break;
    case PLAYER_UI_LOCK_BACKGROUND:
        lock_names.clear();lock_list_ready=false;lock_list_pending=true;lock_list_status="Reading JPG / PNG files from SD...";
        lock_begin(LockFlow::Backgrounds);break;
    default:break;
    }
    return true;
}
static bool lock_worker_continue(void *arg,bool sd) {
    auto *j=static_cast<LockJob*>(arg);
    while(!j->cancel.load(std::memory_order_acquire)){
        if(!sd)return true;
        if(j->allow_io.load(std::memory_order_acquire)){
            audio_snapshot_t a{};audio_engine_get_snapshot(&a);
            if(a.state!=AUDIO_LOADING&&a.state!=AUDIO_SEEKING&&(a.state!=AUDIO_PLAYING||a.buffered_blocks>=4))return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}
static void lock_worker(void *arg) {
    auto *j=static_cast<LockJob*>(arg);j->started=now_ms();
    if(j->kind==LockJob::List)lock_wallpaper_list(j->list,lock_worker_continue,j);
    else if(j->kind==LockJob::Frost)j->image=lock_wallpaper_frosted(j->snapshot,1280,lock_worker_continue,j);
    else j->image=lock_wallpaper_load(j->name,lock_worker_continue,j);
    heap_caps_free(j->snapshot);j->snapshot=nullptr;j->elapsed=now_ms()-j->started;
    j->done.store(true,std::memory_order_release);vTaskDelete(nullptr);
}
static void lock_free_job(LockJob *j) {heap_caps_free(j->snapshot);delete j->list;lock_wallpaper_image_free(&j->image);delete j;}
bool lock_publish(lock_wallpaper_image_t &target,lv_image_dsc_t &desc,lock_wallpaper_image_t &source,const char *published_name=nullptr) {
    // Never free pixels while LVGL can still read their old descriptor.
    if(!tab5_display_lock(1000))return false;
    player_ui_set_lock_background(nullptr);lv_image_cache_drop(&desc);
    auto old=target;target=source;source={};desc={};
    desc.header.magic=LV_IMAGE_HEADER_MAGIC;desc.header.cf=LV_COLOR_FORMAT_RGB565;
    desc.header.w=target.width;desc.header.h=target.height;desc.header.stride=target.width*2;
    desc.data_size=target.width*target.height*2;desc.data=reinterpret_cast<const uint8_t*>(target.pixels);
    if(published_name)lock_loaded_name=published_name;
    lock_choose_background_ui();tab5_display_unlock();lock_wallpaper_image_free(&old);return true;
}
bool lock_stop_worker(uint32_t timeout_ms) {
    if(!lock_job)return true;
    lock_job->cancel.store(true,std::memory_order_release);const auto start=now_ms();
    while(!lock_job->done.load(std::memory_order_acquire)&&now_ms()-start<timeout_ms)vTaskDelay(pdMS_TO_TICKS(10));
    if(!lock_job->done.load(std::memory_order_acquire))return false;
    lock_free_job(lock_job);lock_job=nullptr;return true;
}
void poll_lock(uint64_t now) {
    if(lock_exit_pending)lock_finish(lock_exit_settings);
    if(lock_present_pending&&tab5_display_lock(20)){
        close_dialog();player_ui_prepare_lock_capture();lock_choose_background_ui();
        if(lock_flow==LockFlow::Backgrounds)lock_show_list_ui();else lock_show_pin_ui();
        lock_present_pending=false;tab5_display_unlock();
    }
    if(lock_pin_pending&&!dimmed&&!lock_exit_pending&&lock_flow!=LockFlow::None&&lock_flow!=LockFlow::Backgrounds&&tab5_display_lock(20)){
        lock_show_pin_ui();tab5_display_unlock();
    }
    if(lock_job){
        lock_job->allow_io.store(mounted&&!scan_job&&!screenshot_job&&!shutdown_requested,std::memory_order_release);
        if(lock_job->done.load(std::memory_order_acquire)){
            auto *j=lock_job;lock_job=nullptr;
            if(!j->cancel.load()){
                if(j->kind==LockJob::List){
                    lock_names.clear();for(size_t i=0;i<j->list->count;++i)lock_names.emplace_back(j->list->names[i]);
                    lock_list_ready=true;
                    lock_list_status=j->list->error!=LOCK_WALLPAPER_OK?lock_wallpaper_error_text(j->list->error):j->list->truncated?"Showing the first 128 images. Move extra files to another folder.":"Choose a JPG / PNG, or keep the frosted player view.";
                }else if(j->kind==LockJob::Frost){
                    if(j->image.pixels&&!lock_publish(lock_frost,lock_frost_desc,j->image)){lock_job=j;return;}
                }
                else if(screen_lock.wallpaper()==j->name){
                    if(j->image.pixels){
                        if(!lock_publish(lock_custom,lock_custom_desc,j->image,j->name)){lock_job=j;return;}
                        lock_list_status="Background selected.";
                    }
                    else {lock_loaded_name.clear();lock_list_status=std::string(lock_wallpaper_error_text(j->image.error))+" Using frosted glass.";}
                }
                ESP_LOGI(TAG,"LOCK background kind=%u result=%u elapsed=%llums",unsigned(j->kind),unsigned(j->kind==LockJob::List?j->list->error:j->image.error),j->elapsed);
                if(tab5_display_lock(1000)){
                    lock_choose_background_ui();
                    if(lock_flow==LockFlow::Backgrounds)lock_show_list_ui();
                    tab5_display_unlock();
                }
            }
            lock_free_job(j);
        }
    }
    if(!lock_job&&!shutdown_requested){
        auto kind=LockJob::Frost;bool start=false;
        if(lock_list_pending&&!scan_job&&!screenshot_job){
            kind=LockJob::List;start=mounted;lock_list_pending=false;
            if(!mounted){
                lock_list_status="SD card unavailable. Insert the card and open Background again.";
                if(lock_flow==LockFlow::Backgrounds&&tab5_display_lock(1000)){lock_show_list_ui();tab5_display_unlock();}
            }
        }
        else if(lock_capture){kind=LockJob::Frost;start=true;}
        else if(lock_image_pending&&!scan_job&&!screenshot_job){
            lock_image_pending=false;
            if(mounted&&!screen_lock.wallpaper().empty()&&(lock_loaded_name!=screen_lock.wallpaper()||!lock_custom.pixels)){kind=LockJob::Image;start=true;}
        }
        if(start){
            auto *j=new(std::nothrow) LockJob;
            if(j){
                j->kind=kind;j->allow_io.store(mounted&&!scan_job&&!screenshot_job);
                if(kind==LockJob::List)j->list=new(std::nothrow) lock_wallpaper_list_t;
                if(kind==LockJob::Frost){j->snapshot=lock_capture;lock_capture=nullptr;}
                snprintf(j->name,sizeof j->name,"%s",screen_lock.wallpaper().c_str());
                if((kind!=LockJob::List||j->list)&&xTaskCreatePinnedToCore(lock_worker,"lock_backdrop",10240,j,1,nullptr,0)==pdPASS)lock_job=j;
                else {lock_free_job(j);lock_list_status="Not enough memory to prepare background.";}
            }
        }
    }
    if(!dimmed&&lock_flow!=LockFlow::None&&lock_flow!=LockFlow::Backgrounds){
        const auto seconds=(screen_lock.retry_after_ms(now)+999)/1000;
        if(seconds!=lock_last_countdown&&tab5_display_lock(20)){lock_show_pin_ui();tab5_display_unlock();}
    }
    if(!dimmed&&lock_active()&&clock_valid){
        const time_t stamp=time(nullptr);const auto minute=uint64_t(stamp)/60;
        if(minute!=lock_clock_minute&&tab5_display_lock(20)){
            struct tm local{};localtime_r(&stamp,&local);char text[20];
            strftime(text,sizeof text,settings.clock24?"%H:%M":"%I:%M%p",&local);
            player_ui_set_lock_clock(text);lock_clock_minute=minute;tab5_display_unlock();
        }
    }
}
