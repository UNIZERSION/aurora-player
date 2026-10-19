/* Runs the production worker on a real host thread with mock RTOS queues.
 * The mock loader keeps an actual FILE open until an injected I/O stall ends.
 * Decoder format tests live separately in ui_preview.cpp. */
#include "player_artwork.h"
#include "player_artwork_decode.h"
#include "player_ui.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_timer.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <deque>
#include <map>
#include <string>
#include <functional>

struct host_art_queue {
    std::mutex mutex;std::condition_variable changed;std::deque<std::vector<unsigned char>> items;unsigned capacity,size;
    host_art_queue(unsigned count,unsigned bytes):capacity(count),size(bytes) {}
};
QueueHandle_t main_results=nullptr;unsigned queues_created=0;
std::function<void()> after_peek;
QueueHandle_t xQueueCreate(unsigned count,unsigned size) { assert(count>0&&count<=8);auto *q=new host_art_queue(count,size);if(++queues_created==2)main_results=q;return q; }
BaseType_t xQueueSend(QueueHandle_t q,const void *item,TickType_t) {
    std::lock_guard<std::mutex> guard(q->mutex);if(q->items.size()==q->capacity)return pdFALSE;
    q->items.emplace_back(q->size);memcpy(q->items.back().data(),item,q->size);q->changed.notify_all();return pdTRUE;
}
BaseType_t xQueueOverwrite(QueueHandle_t q,const void *item) {
    std::lock_guard<std::mutex> guard(q->mutex);assert(q->capacity==1);q->items.clear();q->items.emplace_back(q->size);memcpy(q->items.back().data(),item,q->size);q->changed.notify_all();return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q,void *item,TickType_t ticks) {
    std::unique_lock<std::mutex> guard(q->mutex);
    if(ticks==portMAX_DELAY)q->changed.wait(guard,[&]{return !q->items.empty();});
    else if(!q->changed.wait_for(guard,std::chrono::milliseconds(ticks),[&]{return !q->items.empty();}))return pdFALSE;
    memcpy(item,q->items.front().data(),q->size);q->items.pop_front();return pdTRUE;
}
BaseType_t xQueuePeek(QueueHandle_t q,void *item,TickType_t) {
    {std::lock_guard<std::mutex> guard(q->mutex);if(q->items.empty())return pdFALSE;memcpy(item,q->items.front().data(),q->size);}
    if(after_peek){auto callback=std::move(after_peek);after_peek=nullptr;callback();}
    return pdTRUE;
}
uint32_t queued_generation() {
    std::lock_guard<std::mutex> guard(main_results->mutex);if(main_results->items.empty())return 0;
    uint32_t generation=0;memcpy(&generation,main_results->items.front().data(),sizeof(generation));return generation;
}
void vQueueDelete(QueueHandle_t queue) { delete queue; }
std::atomic<bool> worker_enabled{false},stall_io{false},file_open{false},io_allowed{true};
std::atomic<unsigned> loads{0},closed{0},ui_updates{0};
std::atomic<unsigned> allocated_images{0},freed_images{0},clone_calls{0},thumbnail_updates{0};
std::atomic<int64_t> clock_offset{0};
std::atomic<const lv_image_dsc_t *> ui_artwork{nullptr};
std::mutex order_mutex;std::vector<std::string> load_order;
std::map<std::string,bool> received_thumbnails;
bool strict_routing=false;std::string ui_path;
BaseType_t xTaskCreatePinnedToCore(void(*fn)(void*),const char *,unsigned,void *arg,unsigned,TaskHandle_t *,int) {
    std::thread([=]{while(!worker_enabled.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }fn(arg);}).detach();return pdPASS;
}
void vTaskDelay(TickType_t ticks) { std::this_thread::sleep_for(std::chrono::milliseconds(ticks?ticks:1)); }
int64_t esp_timer_get_time(void) {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()+clock_offset.load();
}
bool allow_io(void *) { return io_allowed.load(); }
static uint16_t seed(const char *path){uint16_t value=0;while(*path)value=uint16_t(value*31+static_cast<unsigned char>(*path++));return value;}
player_artwork_image_t *player_artwork_load(const char *path,player_artwork_continue_cb proceed,void *context) {
    ++loads;
    {std::lock_guard<std::mutex> guard(order_mutex);load_order.emplace_back(path);}
    if(!proceed(context,true)) { return nullptr; }
    FILE *file=fopen(path,"rb");assert(file);file_open=true;
    while(stall_io.load()) { vTaskDelay(1); }
    const bool keep=proceed(context,true);
    fclose(file);file_open=false;++closed;
    if(!keep) { return nullptr; }
    if(strstr(path,".missing"))return nullptr;
    auto *image=static_cast<player_artwork_image_t *>(calloc(1,sizeof(player_artwork_image_t)));
    image->allocation=malloc(312*312*2);assert(image->allocation);
    image->image.header.magic=LV_IMAGE_HEADER_MAGIC;image->image.header.cf=LV_COLOR_FORMAT_RGB565;
    image->image.header.w=312;image->image.header.h=312;image->image.header.stride=624;
    image->image.data=static_cast<uint8_t *>(image->allocation);image->image.data_size=312*312*2;
    for(unsigned i=0;i<312*312;i++)static_cast<uint16_t *>(image->allocation)[i]=uint16_t(seed(path)+i);
    ++allocated_images;return image;
}
void player_artwork_image_free(player_artwork_image_t *image) { if(!image)return;assert(&image->image!=ui_artwork.load());if(image->image.header.w==312)++freed_images;free(image->allocation);free(image); }
player_artwork_image_t *player_artwork_image_clone(const player_artwork_image_t *) { ++clone_calls;assert(false&&"Worker should share immutable images, not clone195KB");return nullptr; }
extern "C" bool player_ui_accepts_artwork(const char *path) {return !strict_routing||!path||!*path||ui_path==path;}
extern "C" void player_ui_set_artwork(const lv_image_dsc_t *image) {
    if(strict_routing&&image)assert(reinterpret_cast<const uint16_t *>(image->data)[0]==seed(ui_path.c_str()));
    ui_artwork=image;++ui_updates;
}
extern "C" void player_ui_set_thumbnail(const char *path,const lv_image_dsc_t *image) {
    if(image){assert(image->header.w==56&&image->header.h==56&&image->header.stride==112);const auto *pixels=reinterpret_cast<const uint16_t *>(image->data);
      for(unsigned y=0;y<56;y++)for(unsigned x=0;x<56;x++)assert(pixels[y*56+x]==uint16_t(seed(path)+(y*312/56)*312+x*312/56));}
    received_thumbnails[path]=image!=nullptr;++thumbnail_updates;
}
template<class Predicate> void await(Predicate predicate) {
    const int64_t deadline=esp_timer_get_time()+2000000;
    while(!predicate()) { assert(esp_timer_get_time()<deadline);vTaskDelay(1); }
}
int main(int argc,char **argv) {
    assert(argc==2);FILE *f=fopen(argv[1],"wb");assert(f);fputs("artwork input",f);fclose(f);
    assert(player_artwork_stop_and_wait(0)); // Never initialized.
    assert(player_artwork_init(allow_io,nullptr));
    assert(player_artwork_request(argv[1])); // Pending before worker starts.
    assert(!player_artwork_stop_and_wait(5));
    worker_enabled=true;
    assert(player_artwork_stop_and_wait(2000));assert(loads==0&&!file_open);
    assert(player_artwork_stop_and_wait(2000)); // Idle barrier acknowledgement.

    stall_io=true;assert(player_artwork_request(argv[1]));await([]{return file_open.load();});
    assert(player_artwork_request(argv[1])); // Queued replacement behind active I/O.
    const unsigned before=loads.load(),before_close=closed.load(),before_ui=ui_updates.load();
    assert(!player_artwork_stop_and_wait(5));assert(file_open&&closed==before_close);
    assert(ui_updates==before_ui); // Waiting never needs the UI/LVGL thread.
    stall_io=false;
    assert(player_artwork_stop_and_wait(2000));assert(!file_open&&closed==before_close+1);
    vTaskDelay(20);assert(loads==before); // Queued replacement was cancelled.
    player_artwork_poll();assert(ui_artwork==nullptr);

    // Cancellation while the worker is blocked by the audio-buffer gate.
    io_allowed=false;assert(player_artwork_request(argv[1]));vTaskDelay(30);assert(loads==before);
    assert(player_artwork_stop_and_wait(2000));assert(!file_open);
    io_allowed=true;

    // A failed/cancelled shutdown can resume normal loading later.
    const unsigned prior_close=closed.load();assert(player_artwork_request(argv[1]));
    await([&]{return closed>prior_close;});assert(player_artwork_stop_and_wait(2000));
    player_artwork_poll();assert(!file_open);
    // Completed covers are reused without reopening/decoding the track.
    unsigned started=loads.load(),display_before=ui_updates.load();assert(player_artwork_request(argv[1]));
    await([&]{player_artwork_poll();return loads>started&&closed>prior_close+1&&ui_updates>display_before;});
    started=loads.load();display_before=ui_updates.load();assert(player_artwork_request(argv[1]));
    await([&]{player_artwork_poll();return ui_updates>display_before;});assert(loads==started);
    assert(player_artwork_stop_and_wait(2000)); // Explicit library update clears cache.
    assert(player_artwork_request(argv[1]));await([&]{return loads>started;});assert(player_artwork_stop_and_wait(2000));
    player_artwork_poll();

    std::vector<std::string> paths;
    for(unsigned i=0;i<9;i++)paths.push_back(std::string(argv[1])+".thumb"+std::to_string(i));
    const std::string missing=std::string(argv[1])+".missing";
    paths.push_back(missing);
    for(unsigned i=0;i<4;i++)paths.push_back(std::string(argv[1])+".main"+std::to_string(i));
    for(const auto &path:paths){FILE *file=fopen(path.c_str(),"wb");assert(file);fputs("fixture",file);fclose(file);}
    const char *batch[9];for(unsigned i=0;i<9;i++)batch[i]=paths[i].c_str();
    assert(!player_artwork_request_thumbnails(batch,9));
    const std::string too_long(512,'x');const char *invalid[]={too_long.c_str()};
    assert(!player_artwork_request_thumbnails(invalid,1));
    assert(!player_artwork_request_thumbnails(nullptr,1));
    assert(player_artwork_request_thumbnails(nullptr,0));

    // A cached positive image is immediately reusable while audio forbids SD.
    display_before=ui_updates;assert(player_artwork_request(argv[1]));
    await([&]{player_artwork_poll();return ui_updates>display_before&&ui_artwork.load();});
    const lv_image_dsc_t *same_image=ui_artwork.load();started=loads;const unsigned allocated_before=allocated_images;
    io_allowed=false;display_before=ui_updates;assert(player_artwork_request(argv[1]));
    await([&]{player_artwork_poll();return ui_updates>display_before;});
    assert(ui_artwork==same_image&&loads==started&&allocated_images==allocated_before&&clone_calls==0);
    io_allowed=true;

    // Missing-art results are cached, still delivered with a closed audio gate,
    // and retried after expiry rather than permanently hiding transient errors.
    display_before=ui_updates;assert(player_artwork_request(missing.c_str()));
    await([&]{player_artwork_poll();return ui_updates>display_before&&ui_artwork==nullptr&&loads>started;});
    started=loads;io_allowed=false;display_before=ui_updates;assert(player_artwork_request(missing.c_str()));
    await([&]{player_artwork_poll();return ui_updates>display_before;});assert(loads==started);
    const char *missing_batch[]={missing.c_str()};received_thumbnails.clear();
    assert(player_artwork_request_thumbnails(missing_batch,1));
    await([&]{player_artwork_poll();return received_thumbnails.count(missing);});
    assert(!received_thumbnails[missing]&&loads==started);
    io_allowed=true;clock_offset.fetch_add(31000000);display_before=ui_updates;
    assert(player_artwork_request(missing.c_str()));
    await([&]{player_artwork_poll();return loads>started&&ui_updates>display_before;});
    puts("PASS: immutable positive cache hits bypass closed audio gate without SD, allocation or clone; negative hits, thumbnail NULL and30s expiry.");

    assert(player_artwork_stop_and_wait(2000));player_artwork_poll();received_thumbnails.clear();
    display_before=ui_updates;assert(player_artwork_request(argv[1]));
    await([&]{player_artwork_poll();return ui_updates>display_before&&ui_artwork.load();});
    const lv_image_dsc_t *pinned=ui_artwork.load();const auto *pinned_pixels=reinterpret_cast<const uint16_t *>(pinned->data);
    started=loads;assert(player_artwork_request_thumbnails(batch,8));vTaskDelay(80);
    assert(player_artwork_request_thumbnails(batch,8));assert(loads==started);
    await([&]{assert(player_artwork_request_thumbnails(batch,8));player_artwork_poll();return received_thumbnails.size()==8;});
    assert(loads==started+8&&ui_artwork==pinned&&clone_calls==0);
    assert(pinned_pixels[0]==seed(argv[1])&&pinned_pixels[312*312-1]==uint16_t(seed(argv[1])+312*312-1));
    assert(allocated_images-freed_images==1); // Background misses retain only thumbnails.
    // Evict the borrowed main through newer MAIN requests, without UI polling.
    // Queue replacements release their own refs; the old visible image survives.
    for(unsigned i=0;i<5;i++) {
        const uint32_t prior=queued_generation();assert(player_artwork_request(paths[i].c_str()));
        await([&]{return queued_generation()!=0&&queued_generation()!=prior;});
    }
    assert(ui_artwork==pinned&&allocated_images-freed_images==5);
    assert(pinned_pixels[0]==seed(argv[1])&&pinned_pixels[312*312-1]==uint16_t(seed(argv[1])+312*312-1));
    puts("PASS: repeated viewport snapshots finish8 exactly sampled56x56 thumbnails;180ms debounce; background releases full pixels; borrowed main survives main-LRU eviction without copies.");

    // Eight background decodes must not displace the four recent main covers.
    assert(player_artwork_stop_and_wait(2000));player_artwork_poll();received_thumbnails.clear();
    const lv_image_dsc_t *recent[4]{};
    for(unsigned i=0;i<4;i++) {
        display_before=ui_updates;assert(player_artwork_request(paths[10+i].c_str()));
        await([&]{player_artwork_poll();return ui_updates>display_before&&ui_artwork.load();});recent[i]=ui_artwork;
    }
    started=loads;assert(player_artwork_request_thumbnails(batch,8));
    await([&]{player_artwork_poll();return received_thumbnails.size()==8;});assert(loads==started+8);
    started=loads;const unsigned main_allocated=allocated_images;io_allowed=false;
    for(unsigned i=0;i<4;i++) {
        display_before=ui_updates;assert(player_artwork_request(paths[10+i].c_str()));
        await([&]{player_artwork_poll();return ui_updates>display_before&&ui_artwork.load();});assert(ui_artwork==recent[i]);
    }
    assert(loads==started&&allocated_images==main_allocated);
    // A BG cache hit must not make the oldest MAIN cover more recent.
    const char *cached_thumb[]={paths[10].c_str()};received_thumbnails.clear();
    assert(player_artwork_request_thumbnails(cached_thumb,1));
    await([&]{player_artwork_poll();return received_thumbnails.count(paths[10]);});assert(loads==started);
    io_allowed=true;display_before=ui_updates;assert(player_artwork_request(paths[8].c_str()));
    await([&]{player_artwork_poll();return ui_updates>display_before&&ui_artwork.load();});assert(loads==started+1);
    io_allowed=false;started=loads;
    for(unsigned i=1;i<4;i++) {
        display_before=ui_updates;assert(player_artwork_request(paths[10+i].c_str()));
        await([&]{player_artwork_poll();return ui_updates>display_before&&ui_artwork.load();});assert(ui_artwork==recent[i]);
    }
    assert(loads==started);assert(player_artwork_request(paths[10].c_str()));vTaskDelay(30);
    assert(!queued_generation()&&loads==started); // Oldest main miss waits for audio gate.
    assert(player_artwork_stop_and_wait(2000));player_artwork_poll();io_allowed=true;
    puts("PASS:4main then8background then4closed-audio RAM hits retain exact pointers without IO/allocation; BG hits preserve main LRU order.");

    // A changed viewport cancels a pending debounce before any SD read.
    assert(player_artwork_stop_and_wait(2000));player_artwork_poll();received_thumbnails.clear();started=loads;
    assert(player_artwork_request_thumbnails(batch,1));vTaskDelay(50);
    assert(player_artwork_request_thumbnails(batch+1,1));vTaskDelay(100);assert(loads==started);
    await([&]{player_artwork_poll();return received_thumbnails.count(paths[1]);});
    assert(loads==started+1&&!received_thumbnails.count(paths[0]));

    // A long audio/SD wait must not restart an unchanged viewport every10s.
    assert(player_artwork_stop_and_wait(2000));player_artwork_poll();received_thumbnails.clear();
    started=loads;stall_io=true;assert(player_artwork_request_thumbnails(batch+2,1));await([]{return file_open.load();});
    clock_offset.fetch_add(15000000);assert(player_artwork_request_thumbnails(batch+2,1));stall_io=false;
    await([&]{player_artwork_poll();return received_thumbnails.count(paths[2]);});assert(loads==started+1);

    // Current-cover request interrupts a background read, closes its descriptor,
    // runs before the next background path, and then resumes the visible batch.
    assert(player_artwork_stop_and_wait(2000));player_artwork_poll();received_thumbnails.clear();
    size_t order_start;{std::lock_guard<std::mutex> guard(order_mutex);order_start=load_order.size();}
    stall_io=true;assert(player_artwork_request_thumbnails(batch,2));await([]{return file_open.load();});
    assert(player_artwork_request(argv[1]));stall_io=false;
    await([&]{player_artwork_poll();return received_thumbnails.size()==2&&ui_artwork.load();});
    {std::lock_guard<std::mutex> guard(order_mutex);assert(load_order[order_start]==paths[0]&&load_order[order_start+1]==argv[1]);}
    assert(reinterpret_cast<const uint16_t *>(ui_artwork.load()->data)[0]==seed(argv[1]));

    // Background misses respect the audio reserve too, and shutdown's barrier
    // includes those descriptors, not only the selected-track request channel.
    assert(player_artwork_stop_and_wait(2000));player_artwork_poll();received_thumbnails.clear();
    started=loads;io_allowed=false;assert(player_artwork_request_thumbnails(batch+8,1));vTaskDelay(240);
    assert(loads==started&&player_artwork_stop_and_wait(2000));io_allowed=true;
    stall_io=true;assert(player_artwork_request_thumbnails(batch+8,1));await([]{return file_open.load();});
    assert(!player_artwork_stop_and_wait(5)&&file_open);stall_io=false;
    assert(player_artwork_stop_and_wait(2000));player_artwork_poll();
    assert(!file_open&&received_thumbnails.empty()&&allocated_images==freed_images&&clone_calls==0);
    puts("PASS: viewport replacement drops stale work; current cover preempts background; background audio gate and close-before-shutdown ACK; shared image references all released.");

    // Request and UI state can arrive in either order. Current results retain
    // ownership in the queue until the UI accepts that exact track path.
    strict_routing=true;ui_path=paths[0];assert(player_artwork_request(paths[0].c_str()));
    await([&]{player_artwork_poll();return ui_artwork.load();});
    const auto *art_a=ui_artwork.load();display_before=ui_updates;
    assert(player_artwork_request(paths[1].c_str()));await([]{return queued_generation()!=0;});
    for(unsigned i=0;i<4;i++){player_artwork_poll();assert(ui_updates==display_before&&ui_artwork==art_a&&queued_generation());}
    ui_path=paths[1];player_artwork_poll();assert(ui_artwork&&ui_artwork!=art_a&&!queued_generation());
    assert(!player_artwork_needs_retry());
    assert(player_artwork_request(paths[1].c_str()));await([]{return queued_generation()!=0;});
    ui_path=paths[0];display_before=ui_updates;player_artwork_poll();assert(ui_updates==display_before&&queued_generation());
    assert(player_artwork_request(paths[0].c_str()));
    await([&]{player_artwork_poll();return ui_updates>display_before&&ui_artwork.load();});
    assert(reinterpret_cast<const uint16_t *>(ui_artwork.load()->data)[0]==seed(paths[0].c_str()));

    // NULL results must also wait for their track key. Start retry time only
    // when delivered, clear it on request/positive result/stop.
    display_before=ui_updates;assert(player_artwork_request(missing.c_str()));await([]{return queued_generation()!=0;});
    player_artwork_poll();assert(ui_updates==display_before&&!player_artwork_needs_retry());
    clock_offset.fetch_add(31000000);assert(!player_artwork_needs_retry());
    ui_path=missing;player_artwork_poll();assert(ui_artwork==nullptr&&!player_artwork_needs_retry());
    clock_offset.fetch_add(29000000);assert(!player_artwork_needs_retry());
    clock_offset.fetch_add(1000001);assert(player_artwork_needs_retry());
    assert(player_artwork_request(missing.c_str()));assert(!player_artwork_needs_retry());
    await([]{return queued_generation()!=0;});player_artwork_poll();clock_offset.fetch_add(31000000);assert(player_artwork_needs_retry());
    assert(player_artwork_stop_and_wait(2000));player_artwork_poll();assert(!player_artwork_needs_retry());

    // Force the subtle producer interleaving: peek stale A, producer replaces
    // queue with B, receive B while UI still A. Deferred reference must survive
    // both eventual acceptance and superseding cancellation, without leaks.
    for(unsigned cancel=0;cancel<2;cancel++) {
        assert(player_artwork_stop_and_wait(2000));player_artwork_poll();ui_path=paths[7];
        assert(player_artwork_request(paths[7].c_str()));await([]{return queued_generation()!=0;});
        const uint32_t old_generation=queued_generation();stall_io=true;
        assert(player_artwork_request(paths[8].c_str()));await([]{return file_open.load();});
        after_peek=[&]{stall_io=false;await([&]{return queued_generation()!=0&&queued_generation()!=old_generation;});};
        display_before=ui_updates;player_artwork_poll();assert(ui_updates==display_before&&!queued_generation());
        if(cancel) {
            ui_path=paths[0];assert(player_artwork_request(paths[0].c_str()));
            await([&]{player_artwork_poll();return ui_updates>display_before&&ui_artwork.load();});
        } else {
            ui_path=paths[8];player_artwork_poll();assert(ui_artwork.load()&&ui_updates>display_before);
        }
    }
    assert(player_artwork_stop_and_wait(2000));player_artwork_poll();
    assert(!player_artwork_needs_retry()&&allocated_images==freed_images);
    puts("PASS: current covers/NULL wait for exact UI path in both event orders; stale supersession and forced peek/receive replacement retain correct ownership; retry deadline starts only on accepted NULL and resets; all references released.");
    for(const auto &path:paths)remove(path.c_str());
    remove(argv[1]);
    puts("PASS: production artwork stop barrier; idle/pending/active I/O; timeout keeps FD open; close before ACK; audio gate; cancelled replacement; restart; no UI lock dependency.");
    return 0;
}
