#include "sleep_tap_input.h"
#include "fixed_rate_stream.h"
#include "tab5_hal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#ifndef AURORA_TAP_STREAM
#define AURORA_TAP_STREAM 0
#endif
#if AURORA_TAP_STREAM
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#endif

namespace player {
namespace {
constexpr const char *TAG="sleep_taps";
struct Event { uint32_t token, completed_ms; TapGesture gesture; };
QueueHandle_t events=nullptr;
// Bit 0 arms sampling; upper bits distinguish separate sleep sessions.
std::atomic_uint32_t token{0};
std::atomic_bool stopping{false}, quiesced{false}, playing{false};
bool started=false;
uint32_t millis(){return uint32_t(esp_timer_get_time()/1000);}

// FIFO drain cadence and transfer size; FixedRateStream (fixed_rate_stream.h)
// stamps the frames and checks the sensor's rate.
constexpr uint32_t PollMs=20;
constexpr unsigned FrameCapacity=40;
using Stream=FixedRateStream<TAB5_MOTION_RATE_HZ,TAB5_MOTION_LSB_PER_G>;

#if AURORA_TAP_STREAM
// Calibration only: every FIFO frame goes to the console, at most 16 per line,
// as TAP_FIFO seg=<segment> n=<index of first frame> t=<ms of the read>
// hex=<x,y,z raw int16 LE per frame>. A segment is a gap-free run of frames
// at the sensor rate; any discontinuity starts a new segment at n=0.
// While no USB host is connected (pocket and bag trials on battery), lines
// wait in a PSRAM backlog of about six minutes, then follow, in order and
// with their original n/t, once the host is back. Lines may thus appear late
// and interleaved; the decoder orders them by segment and index.
class FifoStream {
public:
    void restart(const char *reason){
        ++segment_;index_=0;
        if(usb_serial_jtag_is_connected())ESP_LOGI("tap_stream","TAP_FIFO seg=%lu start reason=%s",static_cast<unsigned long>(segment_),reason);
    }
    void log(const tab5_motion_sample_t *frames,size_t count,uint32_t read_ms){
        for(size_t done=0;done<count;){
            Line line{};
            line.segment=segment_;line.index=index_;line.read_ms=read_ms;
            line.count=uint8_t(std::min<size_t>(16,count-done));
            std::copy(frames+done,frames+done+line.count,line.frames);
            if(usb_serial_jtag_is_connected()&&!stored_)print(line);
            else store(line);
            index_+=line.count;done+=line.count;
        }
    }
    // Sends part of the backlog per poll, so FIFO reads continue meanwhile.
    void pump(){
        if(!stored_||!usb_serial_jtag_is_connected())return;
        for(unsigned i=0;i<40&&stored_;++i){print(ring_[head_]);head_=(head_+1)%Capacity;--stored_;}
        if(!stored_)ESP_LOGI("tap_stream","TAP_FIFO backlog done lost_lines=%lu",static_cast<unsigned long>(lost_));
    }
private:
    struct Line {uint32_t segment,index,read_ms;uint8_t count;tab5_motion_sample_t frames[16];};
    static constexpr unsigned Capacity=9000;
    Line *ring_=nullptr;
    unsigned head_=0,stored_=0;
    uint32_t segment_=0,index_=0,lost_=0;
    void store(const Line &line){
        if(!ring_)ring_=static_cast<Line*>(heap_caps_malloc(sizeof(Line)*Capacity,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        if(!ring_){++lost_;return;}
        if(stored_==Capacity){head_=(head_+1)%Capacity;--stored_;++lost_;}
        ring_[(head_+stored_)%Capacity]=line;++stored_;
    }
    static void print(const Line &line){
        static constexpr char digits[]="0123456789abcdef";
        char hex[16*12+1];unsigned written=0;
        for(unsigned i=0;i<line.count;++i){
            const uint16_t words[]={uint16_t(line.frames[i].x),uint16_t(line.frames[i].y),uint16_t(line.frames[i].z)};
            for(auto word:words)for(unsigned byte=0;byte<2;++byte){
                const unsigned value=(word>>(8*byte))&255;
                hex[written++]=digits[value>>4];hex[written++]=digits[value&15];
            }
        }
        hex[written]=0;
        ESP_LOGI("tap_stream","TAP_FIFO seg=%lu n=%lu t=%lu hex=%s",static_cast<unsigned long>(line.segment),
            static_cast<unsigned long>(line.index),static_cast<unsigned long>(line.read_ms),hex);
    }
};
#endif

void worker(void *) {
    TapGestureDetector detector;
    Stream stream;
    static tab5_motion_sample_t frames[FrameCapacity];
#if AURORA_TAP_STREAM
    static FifoStream fifo_log;
    ESP_LOGI(TAG,"TAP_STREAM calibration build: every IMU FIFO frame is logged");
#endif
    bool initialized=false, enabled=false, retry_pending=false;
    uint32_t seen=0, retry_at=0;
    uint32_t report_at=0,samples=0,overflows=0,rate_failures=0;float px=0,py=0,pz=0,max_step=0;
    auto restart=[&](const char *reason){
        detector.reset();stream.restart(millis());
#if AURORA_TAP_STREAM
        fifo_log.restart(reason);
#else
        (void)reason;
#endif
    };
    for(;;){
        const uint32_t current=token.load(std::memory_order_acquire), now=millis();
        if(current!=seen){
            seen=current;samples=0;max_step=0;report_at=now;retry_pending=false;
#if AURORA_TAP_STREAM
            ESP_LOGI("tap_stream","TAP_STREAM session=%lu armed=%u",static_cast<unsigned long>(current>>1),unsigned(current&1U));
#endif
            restart("session");
        }
        // The calibration build samples even while disarmed (screen on); the
        // controller still ignores gestures from a disarmed session token.
        if(stopping.load(std::memory_order_acquire)||(!AURORA_TAP_STREAM&&!(current&1U))){
            if(enabled){
                const auto result=tab5_motion_set_enabled(false);
                if(result!=ESP_OK)ESP_LOGW(TAG,"Disable IMU: %s",esp_err_to_name(result));
                enabled=false;
            }
            detector.reset();
            if(stopping.load(std::memory_order_acquire))quiesced.store(true,std::memory_order_release);
            vTaskDelay(pdMS_TO_TICKS(40));continue;
        }
        if(!enabled){
            // Never interpret an unset or previous-session deadline as a
            // future time after weeks of uptime / 32-bit clock rollover.
            if(retry_pending&&int32_t(now-retry_at)<0){vTaskDelay(pdMS_TO_TICKS(40));continue;}
            auto result=initialized?ESP_OK:tab5_motion_init();
            if(result==ESP_OK){initialized=true;result=tab5_motion_set_enabled(true);}
            if(result!=ESP_OK){
                ESP_LOGW(TAG,"Tap input unavailable: %s; retry in 5 s",esp_err_to_name(result));
                retry_at=now+5000;retry_pending=true;detector.reset();vTaskDelay(pdMS_TO_TICKS(40));continue;
            }
            enabled=true;retry_pending=false;restart("enable");
            ESP_LOGI(TAG,"Armed: screen off with PIN; 3 taps pause / 2 taps resume");
        }
        // Recheck after initialization, which can outlast a screen transition.
        if(token.load(std::memory_order_acquire)!=current||stopping.load(std::memory_order_acquire))continue;
        detector.set_playing(playing.load(std::memory_order_relaxed));
        // Drain the FIFO. A full read means more frames are waiting. A screen
        // transition or shutdown ends the drain between transfers.
        int64_t level_read_us=esp_timer_get_time();
        for(unsigned pass=0;pass<8;++pass){
            if(pass&&(token.load(std::memory_order_acquire)!=current||stopping.load(std::memory_order_acquire)))break;
            size_t count=0;bool overflow=false;
            level_read_us=esp_timer_get_time();
            const auto result=tab5_motion_read_fifo(frames,FrameCapacity,&count,&overflow);
            const uint32_t read_ms=millis();
            if(result!=ESP_OK){
                detector.reset();tab5_motion_set_enabled(false);enabled=false;retry_at=read_ms+1000;retry_pending=true;
                ESP_LOGW(TAG,"IMU read failed: %s; gesture discarded",esp_err_to_name(result));
                break;
            }
            if(overflow){
                ++overflows;restart("overflow");
                ESP_LOGW(TAG,"IMU FIFO overflow; gesture discarded");
                break;
            }
#if AURORA_TAP_STREAM
            fifo_log.log(frames,count,read_ms);
#endif
            for(size_t i=0;i<count;++i){
                stream.push(frames[i],[&](uint32_t at,float x,float y,float z){
                    if(samples)max_step=std::max(max_step,std::max({std::fabs(x-px),std::fabs(y-py),std::fabs(z-pz)}));
                    px=x;py=y;pz=z;++samples;
                    const auto gesture=detector.feed(at,x,y,z);
                    if(gesture!=TapGesture::None && token.load(std::memory_order_acquire)==current){
                        ESP_LOGI(TAG,"Detected %u taps",gesture==TapGesture::Triple?3U:2U);
                        const Event event{current,millis(),gesture};
                        if(xQueueSend(events,&event,0)!=pdTRUE)ESP_LOGW(TAG,"Tap queue full; gesture discarded");
                    }
                });
            }
            if(count<FrameCapacity)break;
        }
        if(!enabled)continue;
#if AURORA_TAP_STREAM
        fifo_log.pump();
#endif
        int32_t expected=0,got=0;
        const int rate=stream.check_rate(level_read_us,&expected,&got);
        if(rate<0){
            ++rate_failures;detector.reset();
            ESP_LOGW(TAG,"IMU rate check failed: %ld frames, expected %ld; detector held",long(got),long(expected));
        }
#if AURORA_TAP_STREAM
        if(rate)ESP_LOGI("tap_stream","TAP_FIFO rate frames=%ld expected=%ld ok=%d",long(got),long(expected),rate>0);
#endif
        const uint32_t now_report=millis();
        if(now_report-report_at>=10000){
            ESP_LOGI(TAG,"IMU samples=%lu max_step_mg=%d overflows=%lu rate_failures=%lu verified=%d",static_cast<unsigned long>(samples),
                int(max_step*1000),static_cast<unsigned long>(overflows),static_cast<unsigned long>(rate_failures),stream.verified());
            const auto d=detector.snapshot();
            ESP_LOGI(TAG,"DETECT impulses=%lu taps=%lu events=%lu/%lu state=%u%u%u pending=%u gaps=%lu resets=%lu peak_rms_mg=%d",
                (unsigned long)d.impulses,(unsigned long)d.accepted_taps,(unsigned long)d.double_events,(unsigned long)d.triple_events,
                d.settling,d.blocked,d.in_window,d.taps,(unsigned long)d.max_sample_gap_ms,(unsigned long)d.resets,int(d.peak_rms_g*1000));
            // One count per TapRejectReason, in enum order from index 1.
            char reasons[160];unsigned written=0;
            for(unsigned k=1;k<unsigned(TapRejectReason::Count)&&written<sizeof reasons;++k)
                written+=unsigned(snprintf(reasons+written,sizeof reasons-written,"%s%lu",k>1?",":"",(unsigned long)d.rejected[k]));
            ESP_LOGI(TAG,"REJECT %s last=%u interval=%lu last_rms_mg=%d along=%d%% axial=%d%% smooth_mg=%d lead_mg=%d",reasons,
                unsigned(d.last_reject),(unsigned long)d.last_tap_interval_ms,int(d.last_rms_g*1000),int(d.last_along*100),
                int(d.last_axial*100),int(d.last_smooth_g*1000),int(d.last_lead_g*1000));
            samples=0;max_step=0;report_at=now_report;
        }
        vTaskDelay(pdMS_TO_TICKS(PollMs));
    }
}
}
bool sleep_tap_start(){
    if(started)return true;
    events=xQueueCreate(4,sizeof(Event));if(!events)return false;
    if(xTaskCreatePinnedToCore(worker,"sleep_taps",4096,nullptr,2,nullptr,0)!=pdPASS){
        vQueueDelete(events);events=nullptr;return false;
    }
    started=true;return true;
}
void sleep_tap_arm(bool enabled){
    if(!started)return;
    if(stopping.load(std::memory_order_acquire))enabled=false;
    const uint32_t old=token.load(std::memory_order_relaxed);
    if(bool(old&1U)==enabled)return;
    token.store(((old+2U)&~1U)|(enabled?1U:0U),std::memory_order_release);
    // Late worker events remain harmless because their session token differs.
    Event discarded{};while(xQueueReceive(events,&discarded,0)==pdTRUE){}
}
void sleep_tap_set_playing(bool value){playing.store(value,std::memory_order_relaxed);}
TapGesture sleep_tap_take(uint32_t now_ms){
    if(!started)return TapGesture::None;
    Event event{};
    while(xQueueReceive(events,&event,0)==pdTRUE){
        const uint32_t current=token.load(std::memory_order_acquire);
        if((current&1U)&&event.token==current&&now_ms-event.completed_ms<=250U)
            return event.gesture;
    }
    return TapGesture::None;
}
bool sleep_tap_stop_and_wait(uint32_t timeout_ms){
    if(!started)return true;
    stopping.store(true,std::memory_order_release);sleep_tap_arm(false);
    const uint32_t until=millis()+timeout_ms;
    // Wait for worker acknowledgement, not merely an earlier idle snapshot.
    while(!quiesced.load(std::memory_order_acquire)){
        if(int32_t(millis()-until)>=0)return false;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
}
}
