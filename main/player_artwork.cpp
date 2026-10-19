#include "player_artwork.h"
#include "player_artwork_decode.h"
#include "player_ui.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <new>
#include <initializer_list>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace {
constexpr size_t VISIBLE=8;
constexpr int64_t NEGATIVE_TTL_US=30000000, VIEWPORT_DEBOUNCE_US=180000;
struct Request { uint32_t generation;char path[512]; };
struct SharedImage {
    std::atomic<unsigned> references{1};player_artwork_image_t *image;
    explicit SharedImage(player_artwork_image_t *p):image(p) {}
};
struct Result { uint32_t generation;SharedImage *image; };
struct Viewport { uint32_t generation;int64_t ready_at;size_t count;char paths[VISIBLE][512]; };
struct ThumbnailResult { uint32_t generation;char path[512];player_artwork_image_t *image; };
QueueHandle_t requests=nullptr,results=nullptr,viewports=nullptr,thumbnails=nullptr;
std::atomic<uint32_t> newest{0},newest_viewport{0},finished_viewport{0},stopped_generation{0};
player_artwork_io_allowed_cb io_allowed=nullptr;void *io_context=nullptr;
SharedImage *displayed=nullptr;uint32_t displayed_generation=0;
// Controller-thread state; worker receives independent queue copies.
char desired_path[512]{},displayed_path[512]{};
Viewport submitted{};int64_t submitted_at=0;
Result deferred{};bool has_deferred=false;int64_t retry_main_at=0;
struct Work { uint32_t generation,viewport;int64_t last_yield; };
// Four recent main312x312 images, shared immutably with the displayed result.
// Background thumbnails may borrow hits but never promote/evict these covers.
// Eviction drops one reference, never pixels borrowed by LVGL. No195KB clones.
struct CacheEntry { char path[512]{};uint32_t used{};SharedImage *image{}; };
struct NegativeEntry { char path[512]{};int64_t expires{}; };
CacheEntry cache[4];NegativeEntry negative[32];uint32_t cache_age=0,negative_next=0;

SharedImage *retain(SharedImage *p) { if(p)p->references.fetch_add(1,std::memory_order_relaxed);return p; }
void release(SharedImage *p) {
    if(p&&p->references.fetch_sub(1,std::memory_order_acq_rel)==1) {
        player_artwork_image_free(p->image);delete p;
    }
}
bool current(const Work &work) {
    return newest.load(std::memory_order_acquire)==work.generation&&
        (!work.viewport||newest_viewport.load(std::memory_order_acquire)==work.viewport);
}
bool proceed(void *opaque,bool will_read_sd) {
    auto &work=*static_cast<Work *>(opaque);
    while(current(work)) {
        if(!will_read_sd||!io_allowed||io_allowed(io_context)) {
            if(esp_timer_get_time()-work.last_yield>=4000) {
                vTaskDelay(1);work.last_yield=esp_timer_get_time();
            }
            return current(work);
        }
        vTaskDelay(pdMS_TO_TICKS(20));work.last_yield=esp_timer_get_time();
    }
    return false;
}
void clear_cache() {
    for(auto &entry:cache){release(entry.image);entry={};}
    for(auto &entry:negative)entry={};
}
SharedImage *load_cached(const char *path,Work &work,bool promote) {
    if(!current(work))return nullptr;
    for(auto &entry:cache)if(entry.image&&!strcmp(entry.path,path)) {
        if(promote)entry.used=++cache_age;
        return retain(entry.image);
    }
    const int64_t now=esp_timer_get_time();
    for(auto &entry:negative)if(entry.expires>now&&!strcmp(entry.path,path))return nullptr;
    // Gate every cache miss, including decoder implementations which may open
    // a descriptor before their first read. RAM hits never wait on audio I/O.
    if(!proceed(&work,true))return nullptr;
    auto *decoded=player_artwork_load(path,proceed,&work);
    if(!current(work)){player_artwork_image_free(decoded);return nullptr;}
    if(!decoded) {
        // NULL can also mean a transient error. Cancelled work isn't cached.
        auto &entry=negative[negative_next++%32];strcpy(entry.path,path);entry.expires=esp_timer_get_time()+NEGATIVE_TTL_US;
        return nullptr;
    }
    auto *image=new(std::nothrow) SharedImage(decoded);
    if(!image){player_artwork_image_free(decoded);return nullptr;}
    if(!promote)return image; // Resize background misses, then release full pixels.
    CacheEntry *entry=&cache[0];for(auto &candidate:cache)if(candidate.used<entry->used)entry=&candidate;
    release(entry->image);entry->image=retain(image);strcpy(entry->path,path);entry->used=++cache_age;
    return image;
}
player_artwork_image_t *small_image(const SharedImage *source) {
    if(!source)return nullptr;
    const auto &input=source->image->image;
    if(input.header.cf!=LV_COLOR_FORMAT_RGB565||!input.header.w||!input.header.h||!input.data)return nullptr;
    auto *out=static_cast<player_artwork_image_t *>(calloc(1,sizeof(player_artwork_image_t)));
    if(!out)return nullptr;
#ifdef ESP_PLATFORM
    out->allocation=heap_caps_malloc(56*56*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
    out->allocation=malloc(56*56*2);
#endif
    if(!out->allocation){player_artwork_image_free(out);return nullptr;}
    out->image.header.magic=LV_IMAGE_HEADER_MAGIC;out->image.header.cf=LV_COLOR_FORMAT_RGB565;
    out->image.header.w=56;out->image.header.h=56;out->image.header.stride=112;
    out->image.data_size=56*56*2;out->image.data=static_cast<uint8_t *>(out->allocation);
    auto *pixels=static_cast<uint16_t *>(out->allocation);
    const unsigned stride=input.header.stride?input.header.stride:input.header.w*2;
    for(unsigned y=0;y<56;++y) {
        const auto *row=reinterpret_cast<const uint16_t *>(input.data+(y*input.header.h/56)*stride);
        for(unsigned x=0;x<56;++x)pixels[y*56+x]=row[x*input.header.w/56];
    }
    return out;
}
void discard_thumbnails() {
    ThumbnailResult old{};while(xQueueReceive(thumbnails,&old,0)==pdTRUE)player_artwork_image_free(old.image);
}
void main_request(const Request &request,Viewport &viewport,size_t &index) {
    if(!request.path[0]) {
        // Dequeued only after the previous loader returned and closed its files.
        clear_cache();viewport={};index=0;Viewport ignored{};
        while(xQueueReceive(viewports,&ignored,0)==pdTRUE){}
        discard_thumbnails();Result old{};
        while(xQueueReceive(results,&old,0)==pdTRUE)release(old.image);
        stopped_generation.store(request.generation,std::memory_order_release);return;
    }
    if(newest.load(std::memory_order_acquire)!=request.generation)return;
    Work work{request.generation,0,esp_timer_get_time()};const int64_t start=work.last_yield;
    auto *image=load_cached(request.path,work,true);
    if(!current(work)){release(image);return;}
    Result old{},result{request.generation,image};
    if(xQueueReceive(results,&old,0)==pdTRUE)release(old.image);
    if(xQueueSend(results,&result,0)!=pdTRUE)release(image);
    ESP_LOGI("artwork","request=%lu %s in %lld ms",static_cast<unsigned long>(request.generation),image?"cover ready":"no supported cover",static_cast<long long>((esp_timer_get_time()-start)/1000));
}
void worker(void *) {
    Request request{};Viewport viewport{};size_t index=0;uint32_t handled_main=0;
    for(;;) {
        // Main cover/barrier always wins at the next bounded continuation.
        if(xQueueReceive(requests,&request,0)==pdTRUE){main_request(request,viewport,index);handled_main=request.generation;continue;}
        Viewport next{};
        if(xQueueReceive(viewports,&next,0)==pdTRUE){viewport=next;index=0;discard_thumbnails();}
        if(viewport.generation!=newest_viewport.load(std::memory_order_acquire))index=viewport.count;
        if(index==viewport.count&&viewport.generation==newest_viewport.load(std::memory_order_acquire))
            finished_viewport.store(viewport.generation,std::memory_order_release);
        if(index<viewport.count&&handled_main==newest.load(std::memory_order_acquire)&&esp_timer_get_time()>=viewport.ready_at) {
            Work work{handled_main,viewport.generation,esp_timer_get_time()};
            auto *image=load_cached(viewport.paths[index],work,false);
            if(current(work)) {
                ThumbnailResult result{};result.generation=viewport.generation;strcpy(result.path,viewport.paths[index]);
                result.image=small_image(image);
                // Allocation failure is not evidence of no cover.
                if((!image||result.image)&&xQueueSend(thumbnails,&result,0)==pdTRUE)result.image=nullptr;
                player_artwork_image_free(result.image);++index;
            }
            release(image);continue;
        }
        if(xQueueReceive(requests,&request,pdMS_TO_TICKS(20))==pdTRUE){main_request(request,viewport,index);handled_main=request.generation;}
    }
}
}

extern "C" bool player_artwork_init(player_artwork_io_allowed_cb callback,void *context) {
    if(requests)return true;
    io_allowed=callback;io_context=context;
    requests=xQueueCreate(1,sizeof(Request));results=xQueueCreate(1,sizeof(Result));
    viewports=xQueueCreate(1,sizeof(Viewport));thumbnails=xQueueCreate(VISIBLE,sizeof(ThumbnailResult));
    if(!requests||!results||!viewports||!thumbnails||xTaskCreatePinnedToCore(worker,"album_art",24576,nullptr,1,nullptr,0)!=pdPASS) {
        for(auto queue:{requests,results,viewports,thumbnails})if(queue)vQueueDelete(queue);
        requests=results=viewports=thumbnails=nullptr;return false;
    }
    return true;
}
extern "C" bool player_artwork_request(const char *path) {
    if(!requests||(path&&strlen(path)>=sizeof(Request::path)))return false;
    release(deferred.image);deferred={};has_deferred=false;retry_main_at=0;
    Request request{};request.generation=newest.fetch_add(1,std::memory_order_acq_rel)+1;
    desired_path[0]=0;if(path){strcpy(request.path,path);strcpy(desired_path,path);}
    if(!request.path[0])newest_viewport.fetch_add(1,std::memory_order_acq_rel);
    return xQueueOverwrite(requests,&request)==pdTRUE;
}
extern "C" bool player_artwork_request_thumbnails(const char *const *paths,size_t count) {
    if(!viewports||count>VISIBLE||(count&&!paths))return false;
    Viewport next{};
    for(size_t i=0;i<count;++i) {
        if(!paths[i]||!*paths[i]||strlen(paths[i])>=sizeof(next.paths[0]))return false;
        bool duplicate=false;for(size_t j=0;j<next.count;++j)if(!strcmp(next.paths[j],paths[i]))duplicate=true;
        if(!duplicate)strcpy(next.paths[next.count++],paths[i]);
    }
    const int64_t now=esp_timer_get_time();bool subset=next.count!=0;
    for(size_t i=0;i<next.count;++i){bool found=false;for(size_t j=0;j<submitted.count;++j)if(!strcmp(next.paths[i],submitted.paths[j]))found=true;if(!found)subset=false;}
    if(submitted.generation==newest_viewport.load(std::memory_order_acquire)&&
       ((!next.count&&!submitted.count)||(subset&&(finished_viewport.load(std::memory_order_acquire)!=submitted.generation||now-submitted_at<10000000))))return true;
    next.generation=newest_viewport.fetch_add(1,std::memory_order_acq_rel)+1;
    next.ready_at=now+VIEWPORT_DEBOUNCE_US;submitted=next;submitted_at=now;
    return xQueueOverwrite(viewports,&next)==pdTRUE;
}
extern "C" bool player_artwork_stop_and_wait(uint32_t timeout_ms) {
    release(deferred.image);deferred={};has_deferred=false;retry_main_at=0;
    if(!requests)return true;
    Request barrier{};barrier.generation=newest.fetch_add(1,std::memory_order_acq_rel)+1;
    newest_viewport.fetch_add(1,std::memory_order_acq_rel);desired_path[0]=0;
    if(xQueueOverwrite(requests,&barrier)!=pdTRUE)return false;
    const int64_t deadline=esp_timer_get_time()+int64_t(timeout_ms)*1000;
    for(;;) {
        if(stopped_generation.load(std::memory_order_acquire)==barrier.generation)return true;
        if(esp_timer_get_time()>=deadline)return false;
        vTaskDelay(1);
    }
}
extern "C" void player_artwork_poll(void) {
    const uint32_t generation=newest.load(std::memory_order_acquire);
    const bool accepted=player_ui_accepts_artwork(desired_path);
    Result result{};bool applied=false,received=false;
    if(has_deferred) {
        if(deferred.generation!=generation||accepted){result=deferred;deferred={};has_deferred=false;received=true;}
    } else if(results&&xQueuePeek(results,&result,0)==pdTRUE&&(result.generation!=generation||accepted)) {
        received=xQueueReceive(results,&result,0)==pdTRUE;
    }
    if(received) {
        if(result.generation!=generation)release(result.image);
        else if(!accepted) {
            // Producer may replace a stale result between peek and receive.
            // Keep its reference until the UI catches up; never assign B to A.
            deferred=result;has_deferred=true;
        } else {
            player_ui_set_artwork(result.image?&result.image->image->image:nullptr);
            release(displayed);displayed=result.image;strcpy(displayed_path,desired_path);applied=true;
            retry_main_at=!result.image&&desired_path[0]?esp_timer_get_time()+NEGATIVE_TTL_US:0;
        }
    }
    if(displayed_generation!=generation&&accepted) {
        if(!applied&&strcmp(displayed_path,desired_path)) {
            player_ui_set_artwork(nullptr);release(displayed);displayed=nullptr;displayed_path[0]=0;
        }
        displayed_generation=generation;
    }
    if(!thumbnails)return;
    // Bounded handoff under the UI lock: at most two6KB copies, no SD/decode.
    for(unsigned n=0;n<2;++n) {
        ThumbnailResult thumbnail{};if(xQueueReceive(thumbnails,&thumbnail,0)!=pdTRUE)break;
        if(thumbnail.generation==newest_viewport.load(std::memory_order_acquire))
            player_ui_set_thumbnail(thumbnail.path,thumbnail.image?&thumbnail.image->image:nullptr);
        player_artwork_image_free(thumbnail.image);
    }
}
extern "C" bool player_artwork_needs_retry(void) {
    return retry_main_at>0&&esp_timer_get_time()>=retry_main_at;
}
