#include "bluetooth_bridge.h"
#include "bluetooth_dsp.hpp"
#include "bluetooth_protocol.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <new>
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {
constexpr uart_port_t uart=UART_NUM_2;
constexpr gpio_num_t uartTx=GPIO_NUM_6,uartRx=GPIO_NUM_46;
constexpr uint32_t pauseGraceMs=120000;
struct Command {char line[40];uint32_t generation=0;};
struct RemoteEvent {bt_remote_action_t action;uint32_t time,generation;};
enum class Link {Sleeping,LowPulse,BootWait,Waiting,Online};
struct Bridge {
    audio_output_t local{};
    bt_bridge_snapshot_t snapshot{};
    SemaphoreHandle_t state_mutex=nullptr,audio_mutex=nullptr;
    QueueHandle_t commands=nullptr,remote=nullptr;
    i2s_chan_handle_t tx=nullptr;
    tab5bt::Resampler resampler;
    tab5bt::VolumeUpdate volume_update;
    tab5bt::StreamHandshake stream_handshake;
    tab5bt::LegacyStreamHealth stream_health;
    tab5bt::ShutdownHandshake shutdown;
    bool standby_confirmed=false,sleep_pending=false,shutdown_prepared=false,shutdown_audio_done=false,shutdown_done=false;
    uint32_t shutdown_deadline=0,shutdown_send_due=0;
    esp_err_t shutdown_audio_result=ESP_OK,shutdown_result=ESP_OK;
    bool playing=false,page=false,tx_active=false,audio_started=false,stream_ready=false;
    bool local_applied=false,muted=false;
    uint8_t local_volume=68;
    uint32_t rate=44100,local_rate=0,activity_ms=0,remote_generation=0;
    uint32_t remote_log_window=0,remote_log_suppressed=0;
    unsigned remote_log_count=0;
    Link link=Link::Sleeping;
    uint32_t phase_due=0,next_use=0,next_status=0,next_list=0,last_rx=0,list_started=0;
    bool listing=false,line_overflow=false;
    uint32_t stream_revision=1;
    bool recovery_requested=false,recovery_wait=false,recovered_once=false;
    uint32_t recovery_revision=0,recovery_due=0,recovery_last=0,recovery_window=0;
    unsigned recovery_count=0;
    int airpods_request=-1,airpods_value=0;
    uint32_t airpods_due=0;
    uint32_t airpods_sequence=0;
    unsigned line_used=0,incoming_count=0;
    char line[1024]{};
    bt_bridge_device_t incoming[BT_BRIDGE_MAX_DEVICES]{};
};
Bridge *bridge=nullptr;
uint32_t now_ms(){return uint32_t(esp_timer_get_time()/1000);}
void lock(){xSemaphoreTake(bridge->state_mutex,portMAX_DELAY);}
void unlock(){xSemaphoreGive(bridge->state_mutex);}
/* Diagnostic metadata only: at most eight records per second, never names,
 * paths or PCM. Logging happens after releasing the state mutex. */
void remote_log(bt_remote_action_t action,const char *result,uint32_t age=0) {
    lock();const uint32_t now=now_ms();
    if(uint32_t(now-bridge->remote_log_window)>=1000) {
        bridge->remote_log_window=now;bridge->remote_log_count=0;
    }
    if(bridge->remote_log_count>=8){++bridge->remote_log_suppressed;unlock();return;}
    ++bridge->remote_log_count;
    const unsigned suppressed=bridge->remote_log_suppressed;bridge->remote_log_suppressed=0;
    const bool enabled=bridge->snapshot.enabled,connected=bridge->snapshot.connected,playing=bridge->playing;
    unlock();
    const char *name=action==BT_REMOTE_PLAY?"PLAY":action==BT_REMOTE_PAUSE?"PAUSE":
        action==BT_REMOTE_NEXT?"NEXT":action==BT_REMOTE_PREVIOUS?"PREVIOUS":"NONE";
    ESP_LOGI("BT_REMOTE","action=%s result=%s age_ms=%lu enabled=%d connected=%d playing=%d suppressed=%u",
        name,result,static_cast<unsigned long>(age),enabled,connected,playing,suppressed);
}
void message(const char *text){std::snprintf(bridge->snapshot.message,sizeof(bridge->snapshot.message),"%s",text);++bridge->snapshot.revision;}
void airpods_message(const char *text){std::snprintf(bridge->snapshot.airpods_message,sizeof(bridge->snapshot.airpods_message),"%s",text);++bridge->snapshot.revision;}
void clear_airpods() {
    auto &s=bridge->snapshot;s.airpods_connected=s.airpods_pending=s.airpods_connecting=s.airpods_auto_pause=false;
    s.airpods_mode=s.airpods_left=s.airpods_right=s.airpods_case=s.airpods_ears=-1;bridge->airpods_request=-1;
    s.airpods_message[0]=0;
    s.sbc_rate=0;s.sbc_channels=s.sbc_bitpool_min=s.sbc_bitpool_max=-1;
}
void connection(bool connected) {
    if(bridge->snapshot.connected!=connected) {
        bridge->snapshot.connected=connected;++bridge->remote_generation;
        ++bridge->stream_revision;bridge->stream_ready=false;
        if(!connected){bridge->audio_started=false;bridge->recovery_requested=bridge->recovery_wait=false;bridge->snapshot.stream_failed=false;clear_airpods();}
        ++bridge->snapshot.revision;
    }
}
/* Called with state_mutex held. Repeated failures stop playback in the root
 * controller instead of hiding an endless recovery loop behind PLAYING. */
void request_recovery(uint32_t now) {
    if(!bridge->snapshot.enabled || !bridge->snapshot.connected || !bridge->playing ||
       !bridge->stream_ready || bridge->snapshot.stream_failed || bridge->recovery_requested || bridge->recovery_wait)return;
    if(bridge->recovered_once && uint32_t(now-bridge->recovery_last)<15000)return;
    if(!bridge->recovered_once || uint32_t(now-bridge->recovery_window)>=60000){bridge->recovery_window=now;bridge->recovery_count=0;}
    if(bridge->recovery_count>=3){bridge->snapshot.stream_failed=true;message("Bluetooth stream stalled. Playback paused; press Play to retry.");return;}
    bridge->recovery_requested=true;bridge->recovery_revision=bridge->stream_revision;
}
bool send_line(const char *line) {
    lock();const bool allowed=!bridge->shutdown.requested();unlock();
    if(!allowed)return false;
    uart_write_bytes(uart,line,std::strlen(line));uart_write_bytes(uart,"\n",1);
    return true;
}
void restarting(uint32_t now) {
    bridge->link=Link::Waiting;bridge->last_rx=now;
    bridge->next_status=now+1000;bridge->next_list=0;
    bridge->listing=false;bridge->incoming_count=0;
    lock();connection(false);bridge->snapshot.online=false;bridge->standby_confirmed=false;
    ++bridge->stream_revision;bridge->stream_ready=false;
    bridge->snapshot.devices_ready=false;
    bridge->snapshot.awake=true;bridge->volume_update.queue(bridge->snapshot.volume);
    bridge->snapshot.volume_pending=true;unlock();
}
void process_line(const char *line,uint32_t now) {
    lock();const bool shutdown=bridge->shutdown.requested();
    if(shutdown && !std::strcmp(line,"STANDBY")) {
        /* The request can arrive while receive_lines is already running. A
         * pending earlier SLEEP is still valid and must not lose its ACK. */
        if(bridge->link==Link::Sleeping && bridge->sleep_pending)bridge->shutdown.sleep_sent();
        bridge->shutdown.acknowledge();
    }
    unlock();
    if(shutdown)return;
    if(!std::strncmp(line,"AIRPODS ",8)) {
        lock();auto &s=bridge->snapshot;long value=0;
        if(s.airpods_capable && s.connected && tab5bt::integer_field(line,"connected",value) && (value==0||value==1)) {
            s.airpods_connected=value==1;
            const auto field=[&](const char *key,int8_t &target,int maximum){long v=0;if(tab5bt::integer_field(line,key,v)&&v>=-1&&v<=maximum){target=int8_t(v);return true;}return false;};
            const bool mode_reported=tab5bt::integer_field(line,"mode",value)&&(value==-1||(value>=1&&value<=4));
            if(mode_reported)s.airpods_mode=int8_t(value);
            field("left",s.airpods_left,100);field("right",s.airpods_right,100);field("case",s.airpods_case,100);field("ears",s.airpods_ears,2);
            const bool pause_reported=tab5bt::integer_field(line,"auto_pause",value)&&(value==0||value==1);
            if(pause_reported)s.airpods_auto_pause=value==1;
            if(!s.airpods_connected){s.airpods_mode=s.airpods_left=s.airpods_right=s.airpods_case=s.airpods_ears=-1;}
            const bool confirmed=(bridge->airpods_request==BT_AIRPODS_CONNECT&&s.airpods_connected)||
                (bridge->airpods_request==BT_AIRPODS_MODE&&s.airpods_connected&&mode_reported&&s.airpods_mode==bridge->airpods_value)||
                (bridge->airpods_request==BT_AIRPODS_AUTO_PAUSE&&s.airpods_connected&&pause_reported&&s.airpods_auto_pause==bool(bridge->airpods_value));
            if(confirmed){s.airpods_pending=s.airpods_connecting=false;bridge->airpods_request=-1;airpods_message("Settings confirmed");}
            ++s.revision;
        }
        unlock();return;
    }
    if(!std::strncmp(line,"AIRPODS_ERROR ",14)) {
        lock();if(bridge->snapshot.airpods_capable&&bridge->snapshot.connected){
            bridge->snapshot.airpods_pending=bridge->snapshot.airpods_connecting=false;bridge->airpods_request=-1;
            const char *error=line+14;
            airpods_message(!std::strcmp(error,"mode_not_confirmed")?"Headphones did not confirm this mode. Try Connect again.":
                !std::strcmp(error,"save_failed")?"Could not save automatic pause. Try again.":
                !std::strcmp(error,"select_device_first")?"Select and connect your AirPods in Bluetooth first.":
                "AirPods control channel is not ready. Try Connect again.");
        }unlock();return;
    }
    if(!std::strncmp(line,"CODEC type=SBC ",15)) {
        lock();auto &s=bridge->snapshot;long v=0;
        if(s.connected){
            if(tab5bt::integer_field(line,"rate",v)&&v>=8000&&v<=96000)s.sbc_rate=uint32_t(v);
            if(tab5bt::integer_field(line,"channels",v)&&v>=1&&v<=2)s.sbc_channels=int16_t(v);
            if(tab5bt::integer_field(line,"bitpool_min",v)&&v>=2&&v<=250)s.sbc_bitpool_min=int16_t(v);
            if(tab5bt::integer_field(line,"bitpool_max",v)&&v>=2&&v<=250)s.sbc_bitpool_max=int16_t(v);
        }unlock();return;
    }
    if(!std::strncmp(line,"STREAM_FAULT ",13)) {
        const char *end=std::strchr(line+13,' ');
        const bool input=end&&!std::strcmp(end," INPUT_STALLED"),failed=end&&!std::strcmp(end," TRANSPORT_FAILED");
        if((input||failed) && size_t(end-line-13)<=10) {
            char number[11]{};std::memcpy(number,line+13,size_t(end-line-13));uint32_t token=0;
            if(tab5bt::unsigned_value(number,token)){
                lock();if(bridge->playing&&bridge->snapshot.connected&&bridge->stream_handshake.matches(token,bridge->stream_revision)){
                    if(input)request_recovery(now);
                    else{bridge->snapshot.stream_failed=true;message("Bluetooth transport stalled. Press Play to retry.");}
                }unlock();
            }
        }
        return;
    }
    if(!std::strncmp(line,"STREAM_ACK ",11)) {
        uint32_t token=0;
        if(tab5bt::unsigned_value(line+11,token)) {
            lock();if(bridge->stream_handshake.acknowledge(token,true,bridge->stream_revision))bridge->stream_ready=true;unlock();
        }
        return;
    }
    if(!std::strcmp(line,"STREAM ON: Tab5 I2S to Bluetooth")) {
        lock();if(bridge->stream_handshake.acknowledge(0,false,bridge->stream_revision))bridge->stream_ready=true;unlock();return;
    }
    if(!std::strcmp(line,"SILENT")) {
        lock();bridge->stream_handshake.silent(bridge->stream_revision);unlock();return;
    }
    const bt_remote_action_t action=tab5bt::remote_line(line);
    if(action!=BT_REMOTE_NONE) {
        lock();const bool valid=bridge->snapshot.enabled && bridge->snapshot.connected;
        const uint32_t generation=bridge->remote_generation;unlock();
        if(valid) {
            const RemoteEvent event{action,now,generation};
            if(xQueueSend(bridge->remote,&event,0)!=pdTRUE){lock();++bridge->snapshot.remote_dropped;unlock();remote_log(action,"queue-full");}
            else remote_log(action,"received");
        } else remote_log(action,"disconnected");
        return;
    }
    if(!std::strncmp(line,"DEVICE ",7)) {
        bt_bridge_device_t device{};
        if(bridge->listing && tab5bt::device_line(line,device)) {
            unsigned index=0;
            while(index<bridge->incoming_count && std::strcmp(bridge->incoming[index].mac,device.mac))++index;
            if(index<BT_BRIDGE_MAX_DEVICES) {
                bridge->incoming[index]=device;
                if(index==bridge->incoming_count)++bridge->incoming_count;
            }
        }
        return;
    }
    if(!std::strcmp(line,"END_LIST")) {
        if(bridge->listing) {
            lock();
            bridge->snapshot.devices_ready=true;
            if(bridge->snapshot.device_count!=bridge->incoming_count ||
               std::memcmp(bridge->snapshot.devices,bridge->incoming,bridge->incoming_count*sizeof(bt_bridge_device_t))) {
                std::memcpy(bridge->snapshot.devices,bridge->incoming,bridge->incoming_count*sizeof(bt_bridge_device_t));
                bridge->snapshot.device_count=uint8_t(bridge->incoming_count);++bridge->snapshot.revision;
            }
            unlock();bridge->listing=false;
        }
        return;
    }
    if(!std::strncmp(line,"VOLUME ",7)) {
        char *end=nullptr;const long value=std::strtol(line+7,&end,10);
        if(end!=line+7 && !*end && value>=0 && value<=100) {
            lock();bridge->volume_update.acknowledge(int(value));
            bridge->snapshot.volume_pending=bridge->volume_update.busy();unlock();
        }
        return;
    }
    const bool status=!std::strncmp(line,"STATUS ",7),state=!std::strncmp(line,"STATE ",6);
    if(status || state) {
        long value=0;
        if(!tab5bt::integer_field(line,"connection",value) || value<0 || value>3)return;
        const bool connected=value==2;
        bridge->last_rx=now;bridge->link=Link::Online;
        lock();const bool changed=bridge->snapshot.connected!=connected || !bridge->snapshot.online;
        connection(connected);bridge->snapshot.online=true;bridge->snapshot.awake=true;bridge->standby_confirmed=false;
        if(tab5bt::integer_field(line,"audio",value))bridge->audio_started=value==2;
        bridge->snapshot.received_ms=now;
        if(status) {
            if(tab5bt::integer_field(line,"cap_airpods",value))bridge->snapshot.airpods_capable=value==1;
            if(tab5bt::integer_field(line,"scan",value))bridge->snapshot.scanning=value==1;
            if(tab5bt::integer_field(line,"scandone",value))bridge->snapshot.scan_done=value==1;
            if(tab5bt::integer_field(line,"standby",value))bridge->snapshot.awake=value==0;
            tab5bt::unsigned_field(line,"dropped",bridge->snapshot.pico_dropped);
            tab5bt::unsigned_field(line,"empty",bridge->snapshot.pico_empty);
            tab5bt::unsigned_field(line,"buffered",bridge->snapshot.pico_buffered);
            tab5bt::unsigned_field(line,"rx_blocks",bridge->snapshot.pico_rx_blocks);
            tab5bt::unsigned_field(line,"callbacks",bridge->snapshot.pico_callbacks);
            tab5bt::unsigned_field(line,"consumed",bridge->snapshot.pico_consumed);
            tab5bt::unsigned_field(line,"recoveries",bridge->snapshot.pico_recoveries);
            tab5bt::unsigned_field(line,"input_stalls",bridge->snapshot.pico_input_stalls);
            tab5bt::unsigned_field(line,"flushes",bridge->snapshot.pico_flushes);
            const char *version=std::strstr(line," version=");
            if(version)std::sscanf(version+9,"%19s",bridge->snapshot.version);
            unsigned major=0,minor=0,patch=0;const int parts=std::sscanf(bridge->snapshot.version,"%u.%u.%u",&major,&minor,&patch);
            const bool legacy=parts>=2 && !major && (minor<9 || (minor==9&&patch<3));
            if(bridge->stream_health.observe(now,legacy&&bridge->snapshot.enabled&&connected&&bridge->playing&&bridge->stream_ready,
               bridge->stream_revision,bridge->snapshot.pico_dropped,bridge->snapshot.pico_buffered))request_recovery(now);
            if(tab5bt::integer_field(line,"i2s_error",value) && value)message("Pico I2S input error; check wiring");
        }
        if(changed)message(connected?"Connected to selected receiver":"Waiting for selected receiver");
        unlock();return;
    }
    if(!std::strncmp(line,"READY ",6)) {
        bridge->last_rx=now;bridge->next_status=now+300;
        lock();const char *version=std::strstr(line,"version=");
        if(version)std::sscanf(version+8,"%19s",bridge->snapshot.version);
        long capability=0;bridge->snapshot.airpods_capable=tab5bt::integer_field(line,"cap_airpods",capability)&&capability==1;
        clear_airpods();
        connection(false);++bridge->stream_revision;bridge->stream_ready=false;bridge->standby_confirmed=false;
        bridge->snapshot.devices_ready=false;bridge->volume_update.queue(bridge->snapshot.volume);
        bridge->snapshot.volume_pending=true;unlock();return;
    }
    if(!std::strcmp(line,"STANDBY")) {
        bridge->link=Link::Sleeping;bridge->listing=false;
        lock();connection(false);bridge->snapshot.awake=false;bridge->snapshot.online=false;bridge->standby_confirmed=true;bridge->sleep_pending=false;
        bridge->snapshot.scanning=false;message("Pico sleeping; PLAY wakes it");unlock();return;
    }
    if(!std::strcmp(line,"SCAN_DONE")) {
        bridge->next_list=0;
        lock();bridge->snapshot.scanning=false;bridge->snapshot.scan_done=true;
        message("Scan finished; select a receiver");unlock();return;
    }
    if(!std::strncmp(line,"RESTART ",8)) {restarting(now);return;}
    if(!std::strncmp(line,"SELECTED ",9)) {
        if(std::strstr(line,"restarting"))restarting(now);
        else bridge->next_list=0;
        lock();message("Receiver selected; connecting");unlock();return;
    }
    if(!std::strncmp(line,"ERROR ",6)) {lock();message(line+6);unlock();}
}
void stop_tx_if_idle(bool wanted) {
    if(wanted || xSemaphoreTake(bridge->audio_mutex,0)!=pdTRUE)return;
    if(bridge->tx_active){i2s_channel_disable(bridge->tx);bridge->tx_active=false;bridge->resampler.reset();}
    xSemaphoreGive(bridge->audio_mutex);
}
void service_recovery(uint32_t now) {
    lock();
    if(bridge->recovery_wait && int32_t(now-bridge->recovery_due)>=0)bridge->recovery_wait=false;
    const bool requested=bridge->recovery_requested;unlock();
    if(!requested || xSemaphoreTake(bridge->audio_mutex,0)!=pdTRUE)return;
    lock();
    const bool valid=!bridge->shutdown.requested()&&bridge->snapshot.enabled&&bridge->snapshot.connected&&bridge->playing&&
        bridge->recovery_revision==bridge->stream_revision;
    bridge->recovery_requested=false;
    if(valid){
        bridge->recovery_wait=true;bridge->recovery_due=now+200;
        bridge->recovery_last=now;bridge->recovered_once=true;++bridge->recovery_count;
        ++bridge->snapshot.stream_recoveries;++bridge->stream_revision;
        bridge->stream_ready=bridge->audio_started=false;message("Recovering Bluetooth audio stream");
    }
    unlock();
    if(valid){if(bridge->tx_active){i2s_channel_disable(bridge->tx);bridge->tx_active=false;}bridge->resampler.reset();}
    xSemaphoreGive(bridge->audio_mutex);
}
/* Apply a route change even when playback is paused and no write follows it.
 * If a PCM write currently owns the codec, the UART worker retries within its
 * next 20 ms iteration. Never change codec registers concurrently with a write. */
void apply_local_route() {
    if(xSemaphoreTake(bridge->audio_mutex,0)!=pdTRUE)return;
    lock();const bool local=bridge->snapshot.local_output;
    const bool shutdown=bridge->shutdown.requested();unlock();
    if(!shutdown && bridge->local_applied!=local) {
        const esp_err_t result=bridge->local.volume?
            bridge->local.volume(bridge->local.user,bridge->local_volume,bridge->muted || !local):ESP_OK;
        if(result==ESP_OK)bridge->local_applied=local;
        else {lock();++bridge->snapshot.output_errors;message("Local output mute failed");unlock();}
    }
    xSemaphoreGive(bridge->audio_mutex);
}
void receive_lines(uint32_t now) {
    uint8_t input[768];
    const int count=uart_read_bytes(uart,input,sizeof(input),0);
    for(int i=0;i<count;i++) {
        const char c=char(input[i]);
        if(c=='\n') {
            if(!bridge->line_overflow && bridge->line_used) {
                bridge->line[bridge->line_used]=0;process_line(bridge->line,now);
            }
            bridge->line_used=0;bridge->line_overflow=false;
        } else if(c!='\r') {
            if(bridge->line_used<sizeof(bridge->line)-1 && !bridge->line_overflow)
                bridge->line[bridge->line_used++]=c;
            else bridge->line_overflow=true;
        }
    }
}
/* Runs exclusively on the UART worker. No normal UART traffic can follow its
 * SLEEP. Audio and codec operations stay outside state_mutex, so the public
 * shutdown call can enforce its deadline even if a driver operation stalls. */
void service_shutdown(uint32_t now) {
    lock();const bool done=bridge->shutdown_done;
    const uint32_t deadline=bridge->shutdown_deadline;unlock();
    if(done)return;
    if(int32_t(now-deadline)>=0) {
        lock();bridge->shutdown_done=true;bridge->shutdown_result=ESP_ERR_TIMEOUT;
        message("Shutdown timed out; Bluetooth remains disabled");unlock();return;
    }
    if(!bridge->shutdown_prepared) {
        const bool low=bridge->link==Link::LowPulse;
        if(low) {
            gpio_set_level(uartTx,1);
            uart_set_pin(uart,uartTx,uartRx,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE);
        }
        bridge->shutdown_send_due=low?now+650:bridge->link==Link::BootWait?bridge->phase_due:now;
        uart_wait_tx_done(uart,pdMS_TO_TICKS(50));
        bridge->listing=false;
        lock();
        const bool asleep=bridge->link==Link::Sleeping && bridge->standby_confirmed;
        const bool pending=bridge->link==Link::Sleeping && bridge->sleep_pending;
        if(asleep)bridge->shutdown.already_sleeping();
        else if(pending)bridge->shutdown.sleep_sent();
        unlock();
        /* A previous SLEEP can still be awaiting STANDBY. Preserve that ACK
         * and send nothing else: another UART start bit could wake Pico again. */
        if(!pending){uart_flush_input(uart);bridge->line_used=0;bridge->line_overflow=false;}
        bridge->shutdown_prepared=true;
    }
    if(!bridge->shutdown_audio_done && xSemaphoreTake(bridge->audio_mutex,0)==pdTRUE) {
        esp_err_t result=ESP_OK;
        if(bridge->tx_active) {
            result=i2s_channel_disable(bridge->tx);
            if(result==ESP_OK)bridge->tx_active=false;
        }
        bridge->resampler.reset();bridge->muted=true;bridge->local_applied=false;
        if(bridge->local.volume) {
            const esp_err_t mute_result=bridge->local.volume(bridge->local.user,bridge->local_volume,true);
            if(result==ESP_OK)result=mute_result;
        }
        xSemaphoreGive(bridge->audio_mutex);
        bridge->shutdown_audio_done=true;bridge->shutdown_audio_result=result;
    }
    lock();const bool send=!bridge->shutdown.sent() && !bridge->shutdown.acknowledged();unlock();
    if(send && int32_t(now-bridge->shutdown_send_due)>=0 && int32_t(now_ms()-deadline)<0) {
        /* Both writes precede STANDBY; sending anything after that ACK could
         * wake GPIO32 again. SLEEP itself stops the Pico stream. */
        uart_write_bytes(uart,"STOP\nSLEEP\n",11);
        lock();bridge->shutdown.sleep_sent();unlock();
    }
    receive_lines(now_ms());
    lock();
    if(bridge->shutdown.acknowledged() && bridge->shutdown_audio_done) {
        bridge->shutdown_done=true;bridge->shutdown_result=bridge->shutdown_audio_result;
        bridge->link=Link::Sleeping;bridge->standby_confirmed=true;bridge->sleep_pending=false;
        connection(false);bridge->stream_ready=bridge->audio_started=false;
        bridge->snapshot.awake=false;bridge->snapshot.online=false;
        message(bridge->shutdown_result==ESP_OK?"Bluetooth shut down":"Bluetooth stopped; output mute failed");
    }
    unlock();
}
void control_task(void *) {
    for(;;) {
        const uint32_t now=now_ms();
        lock();const bool shutdown=bridge->shutdown.requested();unlock();
        if(shutdown) {
            service_shutdown(now);vTaskDelay(pdMS_TO_TICKS(20));continue;
        }
        apply_local_route();
        if(bridge->link!=Link::LowPulse)receive_lines(now);
        service_recovery(now);
        lock();
        if(bridge->snapshot.airpods_pending&&int32_t(now-bridge->airpods_due)>=0){
            bridge->snapshot.airpods_pending=bridge->snapshot.airpods_connecting=false;bridge->airpods_request=-1;
            airpods_message("No confirmation. Try Connect again.");
        }
        if(bridge->playing || bridge->page || bridge->snapshot.connected)bridge->activity_ms=now;
        const bool wanted=bridge->snapshot.enabled &&
            (bridge->playing || bridge->page || bridge->snapshot.connected || uint32_t(now-bridge->activity_ms)<pauseGraceMs);
        const bool output_wanted=bridge->snapshot.enabled && bridge->snapshot.connected && bridge->playing;
        const bool page=bridge->page,scanning=bridge->snapshot.scanning;
        unlock();
        stop_tx_if_idle(output_wanted);
        if(bridge->listing && uint32_t(now-bridge->list_started)>4000)bridge->listing=false;
        if(bridge->link==Link::LowPulse) {
            if(int32_t(now-bridge->phase_due)>=0) {
                gpio_set_level(uartTx,1);uart_set_pin(uart,uartTx,uartRx,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE);
                uart_flush_input(uart);bridge->line_used=0;bridge->line_overflow=false;
                bridge->link=Link::BootWait;bridge->phase_due=now+650;
            }
        } else if(!wanted) {
            if(bridge->link!=Link::Sleeping) {
                if(!send_line("SLEEP")){vTaskDelay(pdMS_TO_TICKS(20));continue;}
                bridge->link=Link::Sleeping;bridge->listing=false;bridge->sleep_pending=true;
                xQueueReset(bridge->commands);
                lock();connection(false);bridge->snapshot.awake=false;bridge->snapshot.online=false;
                bridge->snapshot.scanning=false;message("Pico sleeping; PLAY wakes it");unlock();
            }
        } else if(bridge->link==Link::Sleeping ||
                  (bridge->link!=Link::BootWait && uint32_t(now-bridge->last_rx)>12000)) {
            uart_wait_tx_done(uart,pdMS_TO_TICKS(50));
            gpio_reset_pin(uartTx);gpio_set_direction(uartTx,GPIO_MODE_OUTPUT);gpio_set_level(uartTx,0);
            bridge->link=Link::LowPulse;bridge->phase_due=now+120;bridge->last_rx=now;
            bridge->listing=false;bridge->incoming_count=0;
            lock();connection(false);bridge->snapshot.awake=true;bridge->snapshot.online=false;bridge->standby_confirmed=false;bridge->sleep_pending=false;
            ++bridge->stream_revision;bridge->stream_ready=false;
            bridge->snapshot.devices_ready=false;
            message("Waking Pico");unlock();
        } else if(bridge->link==Link::BootWait) {
            if(int32_t(now-bridge->phase_due)>=0) {
                bridge->link=Link::Waiting;bridge->next_use=bridge->next_status=bridge->next_list=0;
                bridge->last_rx=now;
            }
        } else {
            if(int32_t(now-bridge->next_use)>=0){send_line("USE");bridge->next_use=now+3000;}
            if(int32_t(now-bridge->next_status)>=0){send_line("STATUS");bridge->next_status=now+(output_wanted?1000:page?2000:5000);}
            if(bridge->link==Link::Online) {
                lock();const bool connected=bridge->snapshot.connected;
                const bool playing=bridge->playing;unlock();
                if(connected) {
                    lock();unsigned major=0,minor=0;
                    std::sscanf(bridge->snapshot.version,"%u.%u",&major,&minor);
                    bridge->stream_handshake.update(playing&&!bridge->recovery_wait&&!bridge->snapshot.stream_failed,major>0 || minor>=9,bridge->stream_revision);
                    const auto request=bridge->stream_handshake.next(now);unlock();
                    if(request.command==tab5bt::StreamHandshake::Command::Stop)send_line("STOP");
                    else if(request.command==tab5bt::StreamHandshake::Command::Stream) {
                        if(request.tagged) {
                            char command_line[32];std::snprintf(command_line,sizeof(command_line),"STREAM %lu",static_cast<unsigned long>(request.token));
                            send_line(command_line);
                        } else send_line("STREAM");
                    }
                }
                Command command{};
                if(xQueueReceive(bridge->commands,&command,0)==pdTRUE) {
                    const bool airpods=!std::strncmp(command.line,"AIRPODS ",8);
                    lock();const bool permitted=!airpods||(bridge->snapshot.enabled&&bridge->snapshot.connected&&bridge->snapshot.airpods_capable&&
                        bridge->snapshot.airpods_pending&&command.generation==bridge->airpods_sequence);unlock();
                    if(!permitted){vTaskDelay(pdMS_TO_TICKS(20));continue;}
                    send_line(command.line);
                    if(!std::strcmp(command.line,"SCAN")) {
                        lock();bridge->snapshot.scanning=true;bridge->snapshot.scan_done=false;
                        message("Scanning for receivers (20 seconds)");unlock();
                    }
                    if(std::strcmp(command.line,"TEST")&&std::strncmp(command.line,"AIRPODS ",8))restarting(now);
                } else {
                    lock();const int volume=bridge->volume_update.take(now);unlock();
                    if(volume>=0){char command_line[24];std::snprintf(command_line,sizeof(command_line),"VOLUME %d",volume);send_line(command_line);}
                    if(!bridge->listing && int32_t(now-bridge->next_list)>=0) {
                        bridge->incoming_count=0;bridge->listing=true;bridge->list_started=now;
                        bridge->next_list=now+(scanning?2000:page?6000:15000);send_line("LIST");
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
esp_err_t output_configure(void *,uint32_t rate) {
    if(rate<8000 || rate>96000)return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(bridge->audio_mutex,portMAX_DELAY);
    lock();const bool shutdown=bridge->shutdown.requested();unlock();
    if(shutdown){xSemaphoreGive(bridge->audio_mutex);return ESP_ERR_INVALID_STATE;}
    bridge->rate=rate;bridge->resampler.configure(rate);
    const esp_err_t result=bridge->local.configure(bridge->local.user,rate);
    if(result==ESP_OK)bridge->local_rate=rate;
    xSemaphoreGive(bridge->audio_mutex);return result;
}
esp_err_t output_volume(void *,uint8_t percent,bool mute) {
    xSemaphoreTake(bridge->audio_mutex,portMAX_DELAY);
    lock();const bool shutdown=bridge->shutdown.requested();unlock();
    if(shutdown){xSemaphoreGive(bridge->audio_mutex);return ESP_ERR_INVALID_STATE;}
    bridge->local_volume=percent;bridge->muted=mute;
    lock();const bool local=bridge->snapshot.local_output;unlock();
    const esp_err_t result=bridge->local.volume?bridge->local.volume(bridge->local.user,percent,mute || !local):ESP_OK;
    if(result==ESP_OK)bridge->local_applied=local;
    xSemaphoreGive(bridge->audio_mutex);return result;
}
esp_err_t output_write(void *,const int16_t *stereo,size_t frames) {
    const uint32_t started=now_ms();
    for(;;) {
        lock();const bool shutdown=bridge->shutdown.requested();
        const bool waiting=!bridge->snapshot.local_output && bridge->snapshot.enabled && bridge->snapshot.connected && bridge->playing &&
            (!bridge->stream_ready || !bridge->audio_started);unlock();
        if(shutdown)return ESP_ERR_INVALID_STATE;
        if(!waiting)break;
        if(uint32_t(now_ms()-started)>=2000) {
            lock();++bridge->snapshot.output_errors;message("Pico A2DP stream did not start");unlock();
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    xSemaphoreTake(bridge->audio_mutex,portMAX_DELAY);
    lock();const bool shutdown=bridge->shutdown.requested();
    const bool local=bridge->snapshot.local_output;
    const bool bluetooth=bridge->snapshot.enabled && bridge->snapshot.connected && bridge->playing &&
        bridge->stream_ready && bridge->audio_started;
    unlock();
    if(shutdown){xSemaphoreGive(bridge->audio_mutex);return ESP_ERR_INVALID_STATE;}
    esp_err_t result=ESP_OK;
    if(bridge->local_applied!=local && bridge->local.volume) {
        result=bridge->local.volume(bridge->local.user,bridge->local_volume,bridge->muted || !local);
        if(result==ESP_OK)bridge->local_applied=local;
    }
    if(local && result==ESP_OK)result=bridge->local.write(bridge->local.user,stereo,frames);
    if(bluetooth && result==ESP_OK) {
        if(!bridge->tx_active) {
            bridge->resampler.reset();result=i2s_channel_enable(bridge->tx);
            bridge->tx_active=result==ESP_OK;
        }
        if(result==ESP_OK) {
            const bool success=bridge->resampler.push(stereo,frames,[&](const int16_t *pcm,size_t count) {
                static const int16_t silence[256*2]{};
                while(count) {
                    const size_t chunk=std::min<size_t>(count,256);size_t written=0;
                    result=i2s_channel_write(bridge->tx,bridge->muted?silence:pcm,chunk*4,&written,1000);
                    if(result==ESP_OK && written!=chunk*4)result=ESP_ERR_TIMEOUT;
                    if(result!=ESP_OK)return false;
                    count-=chunk;pcm+=chunk*2;
                }
                return true;
            });
            if(!success && result==ESP_OK)result=ESP_FAIL;
        }
    } else if(bridge->tx_active) {
        i2s_channel_disable(bridge->tx);bridge->tx_active=false;bridge->resampler.reset();
    }
    if(!local && !bluetooth) {
        /* Root pauses when BT-only loses its receiver. Keep bounded pacing
         * until it observes that event, never consume the whole file at CPU speed. */
        vTaskDelay(pdMS_TO_TICKS(std::max<uint32_t>(1,uint32_t(frames*1000/bridge->rate))));
    }
    if(result!=ESP_OK){lock();++bridge->snapshot.output_errors;message("Audio output error");unlock();}
    xSemaphoreGive(bridge->audio_mutex);return result;
}
esp_err_t queue_command(const char *line) {
    if(!bridge)return ESP_ERR_INVALID_STATE;
    lock();
    if(bridge->shutdown.requested()){unlock();return ESP_ERR_INVALID_STATE;}
    Command command{};std::snprintf(command.line,sizeof(command.line),"%s",line);
    if(xQueueSend(bridge->commands,&command,0)!=pdTRUE){unlock();return ESP_ERR_TIMEOUT;}
    if(!bridge->snapshot.enabled) {
        bridge->snapshot.enabled=true;++bridge->snapshot.revision;
        ++bridge->stream_revision;bridge->stream_ready=false;
        bridge->volume_update.queue(bridge->snapshot.volume);bridge->snapshot.volume_pending=true;
    }
    bridge->activity_ms=now_ms();unlock();return ESP_OK;
}
}

esp_err_t bluetooth_bridge_init(const audio_output_t *local,audio_output_t *routed) {
    if(bridge)return ESP_ERR_INVALID_STATE;
    if(!local || !local->configure || !local->write || !routed)return ESP_ERR_INVALID_ARG;
    void *memory=heap_caps_malloc(sizeof(Bridge),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!memory)return ESP_ERR_NO_MEM;
    Bridge *b=new(memory) Bridge;
    b->snapshot.airpods_mode=b->snapshot.airpods_left=b->snapshot.airpods_right=b->snapshot.airpods_case=b->snapshot.airpods_ears=-1;
    b->snapshot.sbc_channels=b->snapshot.sbc_bitpool_min=b->snapshot.sbc_bitpool_max=-1;
    b->local=*local;b->state_mutex=xSemaphoreCreateMutex();b->audio_mutex=xSemaphoreCreateMutex();
    b->commands=xQueueCreate(6,sizeof(Command));b->remote=xQueueCreate(16,sizeof(RemoteEvent));
    esp_err_t result=ESP_OK;
    if(!b->state_mutex || !b->audio_mutex || !b->commands || !b->remote)result=ESP_ERR_NO_MEM;
    /* This ESP-IDF BSP reserves I2S1 for ES8388 TX/RX (unlike the old Arduino
     * player). The external Pico transmitter must use I2S0. Pins stay the same. */
    i2s_chan_config_t channel=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,I2S_ROLE_MASTER);
    channel.dma_desc_num=8;channel.dma_frame_num=256;channel.auto_clear_after_cb=true;
    if(result==ESP_OK)result=i2s_new_channel(&channel,&b->tx,nullptr);
    i2s_std_config_t standard{};
    standard.clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(44100);
    standard.slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO);
    standard.gpio_cfg.mclk=I2S_GPIO_UNUSED;standard.gpio_cfg.bclk=GPIO_NUM_5;
    standard.gpio_cfg.ws=GPIO_NUM_18;standard.gpio_cfg.dout=GPIO_NUM_19;standard.gpio_cfg.din=I2S_GPIO_UNUSED;
    if(result==ESP_OK)result=i2s_channel_init_std_mode(b->tx,&standard);
    uart_config_t config{};config.baud_rate=115200;config.data_bits=UART_DATA_8_BITS;
    config.parity=UART_PARITY_DISABLE;config.stop_bits=UART_STOP_BITS_1;
    config.flow_ctrl=UART_HW_FLOWCTRL_DISABLE;config.source_clk=UART_SCLK_DEFAULT;
    bool uart_installed=false;
    if(result==ESP_OK)result=uart_param_config(uart,&config);
    if(result==ESP_OK)result=uart_set_pin(uart,uartTx,uartRx,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE);
    if(result==ESP_OK){result=uart_driver_install(uart,4096,2048,0,nullptr,0);uart_installed=result==ESP_OK;}
    if(result==ESP_OK) {
        b->snapshot.ready=true;b->snapshot.local_output=true;b->snapshot.volume=31;
        std::snprintf(b->snapshot.message,sizeof(b->snapshot.message),"Bluetooth disabled");
        b->activity_ms=now_ms();b->resampler.configure(44100);bridge=b;
        if(xTaskCreatePinnedToCore(control_task,"pico_control",6144,nullptr,3,nullptr,0)!=pdPASS)result=ESP_ERR_NO_MEM;
    }
    if(result!=ESP_OK) {
        bridge=nullptr;if(uart_installed)uart_driver_delete(uart);
        if(b->tx)i2s_del_channel(b->tx);
        if(b->commands)vQueueDelete(b->commands);
        if(b->remote)vQueueDelete(b->remote);
        if(b->state_mutex)vSemaphoreDelete(b->state_mutex);
        if(b->audio_mutex)vSemaphoreDelete(b->audio_mutex);
        b->~Bridge();free(memory);return result;
    }
    *routed={nullptr,output_configure,output_write,output_volume};return ESP_OK;
}
void bluetooth_bridge_get_snapshot(bt_bridge_snapshot_t *out) {
    if(!out)return;
    if(!bridge){*out={};return;}
    lock();*out=bridge->snapshot;unlock();
}
void bluetooth_bridge_set_enabled(bool enabled) {
    if(!bridge)return;
    lock();if(bridge->shutdown.requested()){unlock();return;}
    if(bridge->snapshot.enabled!=enabled) {
        bridge->snapshot.enabled=enabled;++bridge->snapshot.revision;bridge->activity_ms=now_ms();
        ++bridge->stream_revision;bridge->stream_ready=false;
        if(enabled){bridge->volume_update.queue(bridge->snapshot.volume);bridge->snapshot.volume_pending=true;}
    }
    unlock();
}
void bluetooth_bridge_set_local_output(bool enabled) {
    if(!bridge)return;
    lock();if(!bridge->shutdown.requested()){bridge->snapshot.local_output=enabled;++bridge->snapshot.revision;}unlock();
    apply_local_route();
}
esp_err_t bluetooth_bridge_set_volume(uint8_t percent) {
    if(!bridge)return ESP_ERR_INVALID_STATE;
    if(percent>100)return ESP_ERR_INVALID_ARG;
    lock();if(bridge->shutdown.requested()){unlock();return ESP_ERR_INVALID_STATE;}
    bridge->snapshot.volume=percent;bridge->snapshot.volume_pending=true;
    bridge->volume_update.queue(percent);++bridge->snapshot.revision;unlock();return ESP_OK;
}
void bluetooth_bridge_set_activity(bool playing,bool bluetooth_page) {
    if(!bridge)return;
    lock();
    if(bridge->shutdown.requested()){unlock();return;}
    if(bridge->playing!=playing || bridge->page!=bluetooth_page)bridge->activity_ms=now_ms();
    if(bridge->playing!=playing){
        ++bridge->stream_revision;bridge->stream_ready=false;bridge->recovery_wait=bridge->recovery_requested=false;
        if(playing){bridge->snapshot.stream_failed=false;bridge->recovery_count=0;bridge->recovered_once=false;}
    }
    bridge->playing=playing;bridge->page=bluetooth_page;unlock();
}
esp_err_t bluetooth_bridge_shutdown(uint32_t timeout_ms) {
    if(!bridge)return ESP_ERR_INVALID_STATE;
    if(!timeout_ms || timeout_ms>60000)return ESP_ERR_INVALID_ARG;
    const uint32_t started=now_ms();
    const auto remaining=[&]() {const uint32_t elapsed=uint32_t(now_ms()-started);return elapsed<timeout_ms?timeout_ms-elapsed:0;};
    if(xSemaphoreTake(bridge->state_mutex,std::max<TickType_t>(1,pdMS_TO_TICKS(timeout_ms)))!=pdTRUE)return ESP_ERR_TIMEOUT;
    if(!bridge->shutdown.requested()) {
        bridge->shutdown.request();bridge->shutdown_deadline=started+timeout_ms;
        bridge->playing=bridge->page=false;bridge->stream_ready=bridge->audio_started=false;
        ++bridge->stream_revision;++bridge->remote_generation;
        bridge->snapshot.enabled=false;bridge->snapshot.local_output=false;
        bridge->snapshot.connected=false;bridge->snapshot.scanning=false;
        bridge->snapshot.volume_pending=false;
        xQueueReset(bridge->commands);xQueueReset(bridge->remote);
        message("Shutting down Bluetooth");
    }
    unlock();
    for(;;) {
        const uint32_t left=remaining();if(!left)return ESP_ERR_TIMEOUT;
        if(xSemaphoreTake(bridge->state_mutex,std::max<TickType_t>(1,pdMS_TO_TICKS(left)))!=pdTRUE)return ESP_ERR_TIMEOUT;
        const bool done=bridge->shutdown_done;const esp_err_t result=bridge->shutdown_result;unlock();
        if(done)return result;
        vTaskDelay(std::max<TickType_t>(1,pdMS_TO_TICKS(std::min<uint32_t>(left,5))));
    }
}
esp_err_t bluetooth_bridge_scan(void){return queue_command("SCAN");}
esp_err_t bluetooth_bridge_reconnect(void){return queue_command("RECONNECT");}
esp_err_t bluetooth_bridge_test(void){return queue_command("TEST");}
esp_err_t bluetooth_bridge_airpods(bt_airpods_command_t kind,int value) {
    if(!bridge)return ESP_ERR_INVALID_STATE;
    if(kind<BT_AIRPODS_CONNECT||kind>BT_AIRPODS_AUTO_PAUSE ||
       (kind==BT_AIRPODS_MODE&&(value<1||value>4)) ||
       (kind==BT_AIRPODS_AUTO_PAUSE&&(value<0||value>1)))return ESP_ERR_INVALID_ARG;
    lock();auto &s=bridge->snapshot;
    if(bridge->shutdown.requested()||!s.enabled||!s.connected||!s.airpods_capable||s.airpods_pending||
       (kind!=BT_AIRPODS_CONNECT&&!s.airpods_connected)){unlock();return ESP_ERR_INVALID_STATE;}
    Command c{};
    if(kind==BT_AIRPODS_CONNECT)std::snprintf(c.line,sizeof c.line,"AIRPODS CONNECT");
    else std::snprintf(c.line,sizeof c.line,kind==BT_AIRPODS_MODE?"AIRPODS MODE %d":"AIRPODS AUTO_PAUSE %d",value);
    c.generation=++bridge->airpods_sequence;
    if(xQueueSend(bridge->commands,&c,0)!=pdTRUE){unlock();return ESP_ERR_TIMEOUT;}
    bridge->airpods_request=kind;bridge->airpods_value=value;bridge->airpods_due=now_ms()+(kind==BT_AIRPODS_CONNECT?15000:3000);
    s.airpods_pending=true;s.airpods_connecting=kind==BT_AIRPODS_CONNECT;airpods_message("Waiting for confirmation");
    unlock();return ESP_OK;
}
esp_err_t bluetooth_bridge_select(const char *mac) {
    if(!tab5bt::mac_valid(mac))return ESP_ERR_INVALID_ARG;
    if(!bridge)return ESP_ERR_INVALID_STATE;
    lock();if(bridge->shutdown.requested()){unlock();return ESP_ERR_INVALID_STATE;}
    bool already_selected=false;
    if(bridge->snapshot.connected && bridge->snapshot.devices_ready) {
        for(unsigned i=0;i<bridge->snapshot.device_count;i++) {
            const auto &device=bridge->snapshot.devices[i];
            if(device.selected && !strcasecmp(device.mac,mac)){already_selected=true;break;}
        }
    }
    unlock();if(already_selected)return ESP_OK;
    char command[40];std::snprintf(command,sizeof(command),"SELECTMAC %s",mac);return queue_command(command);
}
void bluetooth_bridge_restore_devices(const bt_bridge_device_t *devices,size_t count) {
    if(!bridge || !devices)return;
    lock();
    if(!bridge->shutdown.requested() && !bridge->snapshot.online) {
        bridge->snapshot.device_count=0;
        for(size_t i=0;i<std::min<size_t>(count,BT_BRIDGE_MAX_DEVICES);i++) {
            if(!std::memchr(devices[i].mac,0,sizeof(devices[i].mac)) || !tab5bt::mac_valid(devices[i].mac))continue;
            auto &device=bridge->snapshot.devices[bridge->snapshot.device_count++];device=devices[i];
            device.name[sizeof(device.name)-1]=0;
        }
        ++bridge->snapshot.revision;
    }
    unlock();
}
bt_remote_action_t bluetooth_bridge_pop_remote(void) {
    if(!bridge)return BT_REMOTE_NONE;
    RemoteEvent event{};
    while(xQueueReceive(bridge->remote,&event,0)==pdTRUE) {
        lock();const bool valid=bridge->snapshot.enabled && bridge->snapshot.connected && event.generation==bridge->remote_generation;unlock();
        const uint32_t age=uint32_t(now_ms()-event.time);
        if(valid && age<=2000){remote_log(event.action,"delivered",age);return event.action;}
        lock();++bridge->snapshot.remote_dropped;unlock();
        remote_log(event.action,valid?"stale":"session-ended",age);
    }
    return BT_REMOTE_NONE;
}
