#ifdef NDEBUG
#undef NDEBUG
#endif
/* Compile the real bridge with deterministic UART/I2S/RTOS substitutes. The
 * clock pumps its shutdown worker while the public blocking API waits. */
#include "../main/bluetooth_bridge.cpp"
#include <cassert>
#include <cstdarg>
#include <deque>
#include <string>
#include <vector>
#include "pico-v090/UseLease.hpp"

struct host_queue {size_t count,size;std::deque<std::vector<unsigned char>> items;};
static uint32_t clock_ms=100,ack_due=0;
static bool automatic_ack=true,local_muted=false,channel_running=false;
static unsigned disable_count=0,local_writes=0,wake_edges=0,mute_delay=0;
static esp_err_t mute_result=ESP_OK;
static std::string transmitted,received;
static std::vector<std::string> remote_logs;
static bool single_step=false,reply_status=false;
static unsigned use_count=0;
static UseLease pico_lease;
struct StepComplete {};
void host_bt_log(const char *tag,const char *format,...) {
    assert(std::string(tag)=="BT_REMOTE");
    char text[256];va_list args;va_start(args,format);vsnprintf(text,sizeof text,format,args);va_end(args);
    remote_logs.emplace_back(text);
}

int64_t esp_timer_get_time(){return int64_t(clock_ms)*1000;}
SemaphoreHandle_t xSemaphoreCreateMutex(){return new bool(false);}
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex,TickType_t ticks) {
    auto &locked=*static_cast<bool*>(mutex);
    if(locked){if(ticks!=portMAX_DELAY)clock_ms+=ticks;return pdFALSE;}
    locked=true;return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex){*static_cast<bool*>(mutex)=false;return pdTRUE;}
void vSemaphoreDelete(SemaphoreHandle_t mutex){delete static_cast<bool*>(mutex);}
QueueHandle_t xQueueCreate(unsigned count,unsigned size){return new host_queue{count,size,{}};}
BaseType_t xQueueSend(QueueHandle_t q,const void *item,TickType_t) {
    if(q->items.size()==q->count)return pdFALSE;
    const auto *p=static_cast<const unsigned char*>(item);q->items.emplace_back(p,p+q->size);return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q,void *item,TickType_t) {
    if(q->items.empty())return pdFALSE;
    std::memcpy(item,q->items.front().data(),q->size);q->items.pop_front();return pdTRUE;
}
BaseType_t xQueueReset(QueueHandle_t q){q->items.clear();return pdTRUE;}
void vQueueDelete(QueueHandle_t q){delete q;}
unsigned uxQueueMessagesWaiting(QueueHandle_t q){return unsigned(q->items.size());}
BaseType_t xTaskCreatePinnedToCore(void(*)(void*),const char*,unsigned,void*,unsigned,TaskHandle_t*,int){return pdPASS;}
void vTaskDelete(TaskHandle_t){}
void vTaskDelay(TickType_t ticks) {
    clock_ms+=ticks;
    if(single_step)throw StepComplete{};
    if(ack_due && int32_t(clock_ms-ack_due)>=0){received+="STANDBY\r\n";ack_due=0;}
    if(bridge && bridge->shutdown.requested())service_shutdown(clock_ms);
}
esp_err_t gpio_set_level(gpio_num_t,int level){if(!level)++wake_edges;return ESP_OK;}
esp_err_t gpio_reset_pin(gpio_num_t){return ESP_OK;}
esp_err_t gpio_set_direction(gpio_num_t,int){return ESP_OK;}
int uart_write_bytes(uart_port_t,const void *data,size_t length) {
    const std::string part(static_cast<const char*>(data),length);transmitted+=part;
    if(part=="USE"){pico_lease.renew(clock_ms);++use_count;}
    if(reply_status && part=="STATUS")received+="STATUS connection=2 audio=2 version=0.9.1\n";
    if(automatic_ack && part.find("SLEEP\n")!=std::string::npos)ack_due=clock_ms+25;
    return int(length);
}
int uart_read_bytes(uart_port_t,void *out,uint32_t maximum,TickType_t) {
    const auto count=std::min<size_t>(maximum,received.size());
    std::memcpy(out,received.data(),count);received.erase(0,count);return int(count);
}
esp_err_t uart_wait_tx_done(uart_port_t,TickType_t){return ESP_OK;}
esp_err_t uart_flush_input(uart_port_t){received.clear();return ESP_OK;}
esp_err_t uart_set_pin(uart_port_t,int,int,int,int){return ESP_OK;}
esp_err_t uart_param_config(uart_port_t,const uart_config_t*){return ESP_OK;}
esp_err_t uart_driver_install(uart_port_t,int,int,int,void*,int){return ESP_OK;}
esp_err_t uart_driver_delete(uart_port_t){return ESP_OK;}
esp_err_t i2s_new_channel(const i2s_chan_config_t*,i2s_chan_handle_t *handle,void*){*handle=reinterpret_cast<void*>(1);return ESP_OK;}
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t,const i2s_std_config_t*){return ESP_OK;}
esp_err_t i2s_channel_enable(i2s_chan_handle_t){channel_running=true;return ESP_OK;}
esp_err_t i2s_channel_disable(i2s_chan_handle_t){channel_running=false;++disable_count;return ESP_OK;}
esp_err_t i2s_channel_write(i2s_chan_handle_t,const void*,size_t amount,size_t *written,uint32_t){*written=amount;return ESP_OK;}
esp_err_t i2s_del_channel(i2s_chan_handle_t){return ESP_OK;}
static esp_err_t configure(void*,uint32_t){return ESP_OK;}
static esp_err_t write(void*,const int16_t*,size_t){++local_writes;return ESP_OK;}
static esp_err_t volume(void*,uint8_t,bool mute){local_muted=mute;clock_ms+=mute_delay;return mute_result;}

static audio_output_t fresh() {
    if(bridge) {
        vSemaphoreDelete(bridge->state_mutex);vSemaphoreDelete(bridge->audio_mutex);
        vQueueDelete(bridge->commands);vQueueDelete(bridge->remote);
        bridge->~Bridge();std::free(bridge);bridge=nullptr;
    }
    clock_ms=100;ack_due=0;automatic_ack=true;local_muted=channel_running=false;
    disable_count=local_writes=wake_edges=mute_delay=0;mute_result=ESP_OK;transmitted.clear();received.clear();
    single_step=reply_status=false;use_count=0;pico_lease={};remote_logs.clear();
    const audio_output_t local{nullptr,configure,write,volume};audio_output_t routed{};
    assert(bluetooth_bridge_init(&local,&routed)==ESP_OK);return routed;
}
static void connected() {
    bridge->link=Link::Online;bridge->snapshot.awake=bridge->snapshot.online=true;
    bridge->snapshot.connected=bridge->snapshot.enabled=true;
    bridge->playing=bridge->audio_started=bridge->stream_ready=true;
    bridge->tx_active=channel_running=true;
}
static void control_step() {
    single_step=true;
    try {control_task(nullptr);}catch(const StepComplete&){}
    single_step=false;
}
static uint32_t ready_stream() {
    connected();std::snprintf(bridge->snapshot.version,sizeof bridge->snapshot.version,"0.9.3");
    bridge->last_rx=clock_ms;bridge->next_use=bridge->next_status=bridge->next_list=clock_ms+60000;
    bridge->volume_update={};bridge->stream_handshake.update(true,true,bridge->stream_revision);
    const auto request=bridge->stream_handshake.next(clock_ms);assert(request.command==tab5bt::StreamHandshake::Command::Stream);
    char ack[48];std::snprintf(ack,sizeof ack,"STREAM_ACK %lu",static_cast<unsigned long>(request.token));process_line(ack,clock_ms);
    assert(bridge->stream_ready&&bridge->stream_handshake.matches(request.token,bridge->stream_revision));return request.token;
}
static void fault(uint32_t token) {
    char text[80];std::snprintf(text,sizeof text,"STREAM_FAULT %lu INPUT_STALLED",static_cast<unsigned long>(token));process_line(text,clock_ms);
}
static void at(uint32_t now){clock_ms=now;bridge->last_rx=clock_ms;control_step();}
static uint32_t finish_recovery(uint32_t old_token) {
    const uint32_t due=bridge->recovery_due;
    at(due-1);assert(bridge->recovery_wait&&transmitted.find("STREAM ")==std::string::npos);
    at(due);assert(!bridge->recovery_wait);
    const auto pos=transmitted.find("STREAM ");assert(pos!=std::string::npos);
    unsigned token=0;assert(std::sscanf(transmitted.c_str()+pos,"STREAM %u",&token)==1&&token!=old_token);
    char ack[48];std::snprintf(ack,sizeof ack,"STREAM_ACK %lu",static_cast<unsigned long>(old_token));process_line(ack,clock_ms);assert(!bridge->stream_ready);
    std::snprintf(ack,sizeof ack,"STREAM_ACK %u",token);process_line(ack,clock_ms);assert(bridge->stream_ready);
    process_line("STATE connection=2 audio=2",clock_ms);assert(bridge->audio_started);return token;
}
static void stream_recovery_tests() {
    auto sink=fresh();uint32_t token=ready_stream();transmitted.clear();const uint32_t revision=bridge->stream_revision;
    for(const char *bad:{"STREAM_FAULT 1 OTHER","STREAM_FAULT 1 INPUT_STALLED extra","STREAM_FAULT -1 INPUT_STALLED","STREAM_FAULT 4294967296 INPUT_STALLED","STREAM_FAULT  INPUT_STALLED"}){
        process_line(bad,clock_ms);assert(!bridge->recovery_requested);
    }
    fault(token+1);assert(!bridge->recovery_requested);fault(token);assert(bridge->recovery_requested);
    fault(token);assert(bridge->snapshot.stream_recoveries==0);control_step();
    assert(transmitted=="STOP\n"&&!bridge->stream_ready&&!bridge->audio_started&&!channel_running);
    assert(bridge->stream_revision==revision+1&&bridge->recovery_wait&&bridge->snapshot.stream_recoveries==1&&disable_count==1);
    token=finish_recovery(token);assert(transmitted=="STOP\nSTREAM 2\n");
    assert(sink.configure(nullptr,44100)==ESP_OK);int16_t pcm[8]{};assert(sink.write(nullptr,pcm,4)==ESP_OK&&channel_running);
    // A current-token repeated fault cannot hammer the radio. At15 seconds a
    // genuinely persistent stall gets a new session; the fourth in one minute
    // is exposed to Aurora so PLAYING need not silently continue forever.
    transmitted.clear();clock_ms=bridge->recovery_last+14999;fault(token);assert(!bridge->recovery_requested&&!bridge->snapshot.stream_failed);
    for(unsigned recovery=2;recovery<=3;recovery++){
        clock_ms=bridge->recovery_last+15000;bridge->last_rx=clock_ms;transmitted.clear();fault(token);assert(bridge->recovery_requested);control_step();
        assert(bridge->snapshot.stream_recoveries==recovery&&transmitted=="STOP\n");token=finish_recovery(token);
    }
    transmitted.clear();clock_ms=bridge->recovery_last+15000;fault(token);
    assert(bridge->snapshot.stream_failed&&!bridge->recovery_requested&&bridge->snapshot.stream_recoveries==3);
    assert(std::strstr(bridge->snapshot.message,"Playback paused"));at(clock_ms);assert(transmitted=="STOP\n");
    bluetooth_bridge_set_activity(false,false);bluetooth_bridge_set_activity(true,false);
    assert(!bridge->snapshot.stream_failed&&bridge->recovery_count==0&&!bridge->recovered_once);

    // A queued fault cannot survive pause, link loss, a newer stream revision,
    // Bluetooth OFF or final shutdown. Audio-mutex contention only defers it.
    for(unsigned cancel=0;cancel<5;cancel++){
        sink=fresh();token=ready_stream();transmitted.clear();fault(token);assert(bridge->recovery_requested);
        assert(xSemaphoreTake(bridge->audio_mutex,0)==pdTRUE);service_recovery(clock_ms);assert(bridge->recovery_requested&&bridge->snapshot.stream_recoveries==0);xSemaphoreGive(bridge->audio_mutex);
        if(cancel==0)bluetooth_bridge_set_activity(false,false);
        else if(cancel==1)connection(false);
        else if(cancel==2)++bridge->stream_revision;
        else if(cancel==3)bluetooth_bridge_set_enabled(false);
        else {assert(bluetooth_bridge_shutdown(2500)==ESP_OK);}
        service_recovery(clock_ms);assert(!bridge->recovery_requested&&!bridge->recovery_wait&&bridge->snapshot.stream_recoveries==0);
        if(cancel==4){const auto sent=transmitted;fault(token);at(clock_ms+1000);assert(transmitted==sent&&sent=="STOP\nSLEEP\n");}
    }
    // A pause during the200ms STOP fence must not later resume on its own.
    fresh();token=ready_stream();fault(token);control_step();assert(bridge->recovery_wait);transmitted.clear();
    bluetooth_bridge_set_activity(false,false);at(clock_ms+500);assert(!bridge->recovery_wait&&transmitted.find("STREAM ")==std::string::npos);

    // Legacy STATUS telemetry triggers the same actual recovery path;0.9.3
    // uses explicit current-token fault reports and must ignore that inference.
    for(const char *version:{"0.9.2","0.9.3"}){
        fresh();ready_stream();transmitted.clear();char status[160];
        for(unsigned second=0;second<=6;second++){
            clock_ms=100+second*1000;std::snprintf(status,sizeof status,"STATUS connection=2 audio=2 dropped=%u buffered=4096 version=%s",second*20000,version);process_line(status,clock_ms);
        }
        assert(bridge->recovery_requested==(std::strcmp(version,"0.9.2")==0));
    }
    // A latched Pico transport failure must become visible only for the live
    // stream. Late events from an old token or a paused session do not stop it.
    fresh();token=ready_stream();char failed[80];
    std::snprintf(failed,sizeof failed,"STREAM_FAULT %lu TRANSPORT_FAILED",static_cast<unsigned long>(token+1));process_line(failed,clock_ms);assert(!bridge->snapshot.stream_failed);
    std::snprintf(failed,sizeof failed,"STREAM_FAULT %lu TRANSPORT_FAILED",static_cast<unsigned long>(token));process_line(failed,clock_ms);assert(bridge->snapshot.stream_failed&&!bridge->recovery_requested);
    assert(std::strstr(bridge->snapshot.message,"transport stalled"));
    bluetooth_bridge_set_activity(false,false);bluetooth_bridge_set_activity(true,false);assert(!bridge->snapshot.stream_failed);process_line(failed,clock_ms);assert(!bridge->snapshot.stream_failed);
    fresh();token=ready_stream();bluetooth_bridge_set_activity(false,false);
    std::snprintf(failed,sizeof failed,"STREAM_FAULT %lu TRANSPORT_FAILED",static_cast<unsigned long>(token));process_line(failed,clock_ms);assert(!bridge->snapshot.stream_failed);
    std::puts("Bluetooth stream recovery: validated current-token faults, stale ACK rejection, STOP/200ms/new STREAM, audio restart,15s rate limit/3per-minute failure visibility, pause/link/revision/OFF/shutdown cancellation and version-gated legacy inference passed");
}
static void airpods_bridge_tests() {
    fresh();ready_stream();auto &s=bridge->snapshot;
    assert(!s.airpods_capable&&!s.airpods_connected&&!s.airpods_pending&&s.airpods_mode==-1&&s.airpods_left==-1&&s.airpods_ears==-1);
    assert(bluetooth_bridge_airpods(BT_AIRPODS_CONNECT,0)==ESP_ERR_INVALID_STATE);
    process_line("AIRPODS connected=1 mode=2 left=80 ears=2 auto_pause=1",clock_ms);assert(!s.airpods_connected&&s.airpods_mode==-1);
    process_line("STATUS connection=2 audio=2 cap_airpods=1 version=0.9.3",clock_ms);assert(s.airpods_capable);
    for(int mode:{-1,0,5,100})assert(bluetooth_bridge_airpods(BT_AIRPODS_MODE,mode)==ESP_ERR_INVALID_ARG);
    for(int value:{-1,2})assert(bluetooth_bridge_airpods(BT_AIRPODS_AUTO_PAUSE,value)==ESP_ERR_INVALID_ARG);
    assert(bluetooth_bridge_airpods(static_cast<bt_airpods_command_t>(100),0)==ESP_ERR_INVALID_ARG);
    assert(bluetooth_bridge_airpods(BT_AIRPODS_MODE,2)==ESP_ERR_INVALID_STATE);
    Command queued{};std::snprintf(queued.line,sizeof queued.line,"TEST");
    while(xQueueSend(bridge->commands,&queued,0)==pdTRUE){}
    assert(bluetooth_bridge_airpods(BT_AIRPODS_CONNECT,0)==ESP_ERR_TIMEOUT&&!s.airpods_pending);xQueueReset(bridge->commands);
    const auto revision=bridge->stream_revision;const auto generation=bridge->remote_generation;const auto disables=disable_count;
    transmitted.clear();assert(bluetooth_bridge_airpods(BT_AIRPODS_CONNECT,0)==ESP_OK&&s.airpods_pending&&s.airpods_connecting&&!s.airpods_connected);
    assert(bluetooth_bridge_airpods(BT_AIRPODS_CONNECT,0)==ESP_ERR_INVALID_STATE);control_step();
    assert(transmitted=="AIRPODS CONNECT\n"&&bridge->link==Link::Online&&bridge->stream_revision==revision&&bridge->remote_generation==generation&&disable_count==disables&&bridge->stream_ready);
    process_line("AIRPODS connected=1 mode=1 left=80 right=77 case=-1 ears=2 auto_pause=0",clock_ms);
    assert(s.airpods_connected&&!s.airpods_pending&&!s.airpods_connecting&&s.airpods_mode==1&&s.airpods_left==80&&s.airpods_case==-1&&s.airpods_ears==2);
    transmitted.clear();assert(bluetooth_bridge_airpods(BT_AIRPODS_MODE,2)==ESP_OK&&s.airpods_mode==1);control_step();assert(transmitted=="AIRPODS MODE 2\n"&&bridge->link==Link::Online&&bridge->stream_revision==revision);
    process_line("AIRPODS connected=1 mode=1",clock_ms);assert(s.airpods_pending&&s.airpods_mode==1);
    process_line("AIRPODS connected=1 mode=2",clock_ms);assert(!s.airpods_pending&&s.airpods_mode==2);
    // No optimistic mode change and no cached-field acknowledgement from an
    // unrelated battery update, including reselecting the current mode.
    assert(bluetooth_bridge_airpods(BT_AIRPODS_MODE,2)==ESP_OK);control_step();process_line("AIRPODS connected=1 left=81",clock_ms);assert(s.airpods_pending);
    process_line("AIRPODS connected=1 mode=2",clock_ms);assert(!s.airpods_pending);
    assert(bluetooth_bridge_airpods(BT_AIRPODS_AUTO_PAUSE,0)==ESP_OK);control_step();process_line("AIRPODS connected=1 right=78",clock_ms);assert(s.airpods_pending);
    process_line("AIRPODS connected=1 auto_pause=0",clock_ms);assert(!s.airpods_pending&&!s.airpods_auto_pause);
    assert(bluetooth_bridge_airpods(BT_AIRPODS_AUTO_PAUSE,1)==ESP_OK&&!s.airpods_auto_pause);control_step();process_line("AIRPODS connected=1 auto_pause=1",clock_ms);assert(!s.airpods_pending&&s.airpods_auto_pause);
    process_line("AIRPODS connected=1 mode=0 left=101 right=-2 case=300 ears=3",clock_ms);
    assert(s.airpods_mode==2&&s.airpods_left==81&&s.airpods_right==78&&s.airpods_case==-1&&s.airpods_ears==2);
    process_line("AIRPODS connected=1 mode=-1 left=-1 right=-1 case=-1 ears=-1",clock_ms);
    assert(s.airpods_mode==-1&&s.airpods_left==-1&&s.airpods_right==-1&&s.airpods_case==-1&&s.airpods_ears==-1);
    process_line("AIRPODS connected=1 mode=3 left=100 right=0 case=42 ears=0",clock_ms);
    assert(s.airpods_mode==3&&s.airpods_left==100&&s.airpods_right==0&&s.airpods_case==42&&s.airpods_ears==0);
    assert(bluetooth_bridge_airpods(BT_AIRPODS_MODE,4)==ESP_OK);control_step();const auto due=bridge->airpods_due;
    at(due-1);assert(s.airpods_pending);at(due);assert(!s.airpods_pending&&s.airpods_mode==3&&std::strstr(s.airpods_message,"No confirmation"));
    assert(bluetooth_bridge_airpods(BT_AIRPODS_MODE,2)==ESP_OK);control_step();process_line("AIRPODS_ERROR mode_not_confirmed",clock_ms);assert(!s.airpods_pending&&s.airpods_mode==3&&std::strstr(s.airpods_message,"did not confirm"));
    assert(bluetooth_bridge_airpods(BT_AIRPODS_MODE,4)==ESP_OK);connection(false);assert(!s.airpods_connected&&!s.airpods_pending&&s.airpods_mode==-1&&!s.airpods_auto_pause);
    process_line("AIRPODS connected=1 mode=4 auto_pause=1",clock_ms);assert(!s.airpods_connected&&s.airpods_mode==-1);
    assert(bluetooth_bridge_airpods(BT_AIRPODS_CONNECT,0)==ESP_ERR_INVALID_STATE);
    fresh();ready_stream();process_line("STATUS connection=2 audio=2 cap_airpods=1 version=0.9.3",clock_ms);assert(bluetooth_bridge_airpods(BT_AIRPODS_CONNECT,0)==ESP_OK);control_step();
    const uint32_t connect_due=bridge->airpods_due;at(connect_due-1);assert(bridge->snapshot.airpods_pending);at(connect_due);assert(!bridge->snapshot.airpods_pending&&!bridge->snapshot.airpods_connected);
    assert(bluetooth_bridge_shutdown(2500)==ESP_OK);assert(bluetooth_bridge_airpods(BT_AIRPODS_CONNECT,0)==ESP_ERR_INVALID_STATE);
    // An expired or disconnected request still waiting in the UART command
    // queue must never execute later merely because the worker resumes.
    for(unsigned cancel=0;cancel<3;cancel++){
        fresh();ready_stream();process_line("STATUS connection=2 audio=2 cap_airpods=1 version=0.9.3",clock_ms);
        process_line("AIRPODS connected=1 mode=1 auto_pause=0",clock_ms);transmitted.clear();
        assert(bluetooth_bridge_airpods(BT_AIRPODS_MODE,2)==ESP_OK);
        if(cancel==0){at(bridge->airpods_due);assert(!bridge->snapshot.airpods_pending);}
        else if(cancel==1){connection(false);at(clock_ms);}
        else{process_line("STATUS connection=2 audio=2 cap_airpods=0 version=0.9.3",clock_ms);at(clock_ms);}
        assert(transmitted.find("AIRPODS ")==std::string::npos);
    }
    std::puts("AirPods bridge: capability gating, validated bounds/unknowns, full-queue rejection, acknowledgement-only modes/autopause, unrelated status rejection,3s/15s timeout, disconnect/shutdown cancellation; commands preserve A2DP session/I2S passed");
}
static void remote_pause_tests() {
    fresh();connected();bridge->last_rx=clock_ms;reply_status=true;
    bluetooth_bridge_set_activity(false,false);
    /* Five minutes of pause must keep the actual Pico USE lease alive without
     * running I2S, while explicit PAUSE/PLAY remain distinct commands. */
    for(unsigned i=0;i<15001;i++) {
        control_step();assert(!pico_lease.expired(clock_ms));
        assert(bridge->snapshot.connected && bridge->snapshot.awake);
    }
    assert(use_count>=100 && transmitted.find("SLEEP")==std::string::npos);
    assert(!channel_running && !bridge->tx_active);
    process_line("REMOTE PLAY",clock_ms);
    assert(bluetooth_bridge_pop_remote()==BT_REMOTE_PLAY);
    process_line("REMOTE PAUSE",clock_ms);
    process_line("REMOTE PAUSE",clock_ms);
    assert(bluetooth_bridge_pop_remote()==BT_REMOTE_PAUSE);
    assert(bluetooth_bridge_pop_remote()==BT_REMOTE_PAUSE); /* Never infer a toggle. */
    assert(bluetooth_bridge_pop_remote()==BT_REMOTE_NONE);
    bluetooth_bridge_set_enabled(false);control_step();
    assert(transmitted.find("SLEEP")!=std::string::npos);

    fresh();connected();bridge->last_rx=clock_ms;
    bluetooth_bridge_set_activity(false,false);
    connection(false);bridge->activity_ms=clock_ms;
    clock_ms+=120000;control_step();assert(transmitted=="SLEEP\n");

    fresh();connected();
    process_line("REMOTE PLAY",clock_ms);clock_ms+=2001;
    assert(bluetooth_bridge_pop_remote()==BT_REMOTE_NONE && bridge->snapshot.remote_dropped==1);
    assert(remote_logs.back().find("result=stale")!=std::string::npos);
    process_line("REMOTE PAUSE",clock_ms);connection(false);connection(true);
    assert(bluetooth_bridge_pop_remote()==BT_REMOTE_NONE && bridge->snapshot.remote_dropped==2);
    assert(remote_logs.back().find("result=session-ended")!=std::string::npos);
    const size_t before=remote_logs.size();
    for(unsigned i=0;i<100;i++) {process_line("REMOTE PLAY",clock_ms);(void)bluetooth_bridge_pop_remote();}
    assert(remote_logs.size()<=before+8);
    clock_ms+=1000;process_line("REMOTE PLAY",clock_ms);
    assert(remote_logs.back().find("suppressed=0")==std::string::npos);
    std::puts("Bluetooth remote: 5-minute connected pause retains Pico USE lease, I2S stops, OFF/disconnected idle sleeps; explicit PLAY/PAUSE, stale/session events and bounded diagnostic logs passed");
}
static void remains_off(const audio_output_t &sink) {
    const std::string before=transmitted;const unsigned edges=wake_edges;
    bluetooth_bridge_set_enabled(true);bluetooth_bridge_set_activity(true,true);
    bluetooth_bridge_set_local_output(true);
    assert(bluetooth_bridge_set_volume(75)==ESP_ERR_INVALID_STATE);
    assert(bluetooth_bridge_scan()==ESP_ERR_INVALID_STATE);
    assert(bluetooth_bridge_reconnect()==ESP_ERR_INVALID_STATE);
    assert(bluetooth_bridge_select("AA:BB:CC:DD:EE:FF")==ESP_ERR_INVALID_STATE);
    received="STATUS connection=2 audio=2 version=0.9.0\nREMOTE PLAY\nSTREAM_ACK 1\nREADY version=0.9.0\n";
    for(unsigned i=0;i<10;i++){vTaskDelay(1000);receive_lines(clock_ms);assert(!send_line("USE"));}
    int16_t pcm[4]{};
    assert(sink.configure(nullptr,44100)==ESP_ERR_INVALID_STATE);
    assert(sink.volume(nullptr,100,false)==ESP_ERR_INVALID_STATE);
    assert(sink.write(nullptr,pcm,2)==ESP_ERR_INVALID_STATE);
    assert(bluetooth_bridge_pop_remote()==BT_REMOTE_NONE);
    bt_bridge_snapshot_t snapshot{};bluetooth_bridge_get_snapshot(&snapshot);
    assert(!snapshot.enabled && !snapshot.local_output && !snapshot.connected);
    assert(transmitted==before && wake_edges==edges && local_muted && !local_writes);
}
int main() {
    airpods_bridge_tests();
    stream_recovery_tests();
    remote_pause_tests();
    auto sink=fresh();
    assert(sink.volume(nullptr,68,false)==ESP_OK && !local_muted);
    bridge->tx_active=channel_running=true;assert(bridge->resampler.configure(48000));
    size_t emitted=0;int16_t frame[2]{};
    const auto emit=[&](const int16_t*,size_t frames){emitted+=frames;return true;};
    assert(bridge->resampler.push(frame,1,emit) && emitted==0);
    bluetooth_bridge_set_local_output(false);assert(local_muted);
    assert(bridge->resampler.push(frame,1,emit) && emitted==1);
    assert(channel_running && !disable_count); /* Route mute must preserve BT phase/DMA. */
    assert(sink.volume(nullptr,68,true)==ESP_OK);
    bluetooth_bridge_set_local_output(true);assert(local_muted); /* Paused engine still owns mute. */
    assert(sink.volume(nullptr,68,false)==ESP_OK && !local_muted);
    assert(xSemaphoreTake(bridge->audio_mutex,0)==pdTRUE);
    bluetooth_bridge_set_local_output(false);assert(!local_muted);
    xSemaphoreGive(bridge->audio_mutex);apply_local_route();assert(local_muted);

    sink=fresh();connected();
    assert(bluetooth_bridge_scan()==ESP_OK);
    const RemoteEvent remote{BT_REMOTE_PLAY,clock_ms,bridge->remote_generation};
    assert(xQueueSend(bridge->remote,&remote,0)==pdTRUE);
    assert(bluetooth_bridge_shutdown(2500)==ESP_OK);
    assert(clock_ms<2500 && transmitted=="STOP\nSLEEP\n" && disable_count==1 && !channel_running);
    assert(!uxQueueMessagesWaiting(bridge->commands) && !uxQueueMessagesWaiting(bridge->remote));
    remains_off(sink);assert(bluetooth_bridge_shutdown(50)==ESP_OK);

    sink=fresh();connected();automatic_ack=false;
    received="STANDBY\n"; /* Old buffered ACK is not a new sleep confirmation. */
    assert(bluetooth_bridge_shutdown(80)==ESP_ERR_TIMEOUT);
    assert(clock_ms==180 && transmitted=="STOP\nSLEEP\n" && local_muted && !channel_running);
    remains_off(sink);

    sink=fresh();bridge->link=Link::LowPulse;bridge->phase_due=clock_ms+120;
    assert(bluetooth_bridge_shutdown(2500)==ESP_OK);
    assert(clock_ms>=750 && clock_ms<1000 && transmitted=="STOP\nSLEEP\n");
    remains_off(sink);

    sink=fresh();bridge->link=Link::BootWait;bridge->phase_due=clock_ms+500;
    assert(bluetooth_bridge_shutdown(2500)==ESP_OK && clock_ms>=600);remains_off(sink);

    sink=fresh();bridge->standby_confirmed=true;
    assert(bluetooth_bridge_shutdown(100)==ESP_OK && transmitted.empty());remains_off(sink);

    sink=fresh();bridge->sleep_pending=true;received="STANDBY\n";
    assert(bluetooth_bridge_shutdown(100)==ESP_OK && transmitted.empty());remains_off(sink);

    sink=fresh();bridge->sleep_pending=true;bridge->shutdown.request();
    process_line("STANDBY",clock_ms); /* Request arrived in an existing UART read. */
    assert(bridge->shutdown.acknowledged());

    sink=fresh();connected();mute_result=ESP_FAIL;
    assert(bluetooth_bridge_shutdown(2500)==ESP_FAIL && !channel_running);remains_off(sink);

    sink=fresh();bridge->link=Link::BootWait;bridge->phase_due=clock_ms+650;
    assert(bluetooth_bridge_shutdown(20)==ESP_ERR_TIMEOUT && transmitted.empty());remains_off(sink);
    std::puts("Bluetooth shutdown: actual bridge ACK/timeout, stale UART, pending/already asleep, wake cancellation, mute failure and permanent command/output lock passed; route mute preserves pause and BT phase/DMA");
}
