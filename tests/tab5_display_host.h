#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_TIMEOUT -2
#define ESP_ERR_NO_MEM -3
#define ESP_ERR_INVALID_STATE -4
#define ESP_CACHE_MSYNC_FLAG_DIR_M2C 4
#define PPA_SRM_ROTATION_ANGLE_0 0
#define LV_INDEV_STATE_PRESSED 1
#define LV_INDEV_STATE_RELEASED 0
#define ESP_RETURN_ON_ERROR(expr,tag,...) do { int result_=(expr); (void)(tag); if(result_) return result_; } while(0)
#define ESP_LOGI(tag,...) ((void)(tag))
#define ESP_LOGE(tag,...) ((void)(tag))
#define ESP_LOGW(tag,...) ((void)(tag))
#define IRAM_ATTR
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 1
#define ESP_CACHE_MSYNC_FLAG_UNALIGNED 2
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_DMA 2
#define LV_DISPLAY_ROTATION_90 1
#define LV_DISPLAY_RENDER_MODE_PARTIAL 0
#define LV_COLOR_FORMAT_RGB565 1
#define LV_EVENT_INVALIDATE_AREA 1
#define LV_EVENT_REFR_REQUEST 2
#define LVGL_PORT_EVENT_DISPLAY 1
#define PPA_OPERATION_SRM 1
#define PPA_TRANS_MODE_BLOCKING 1
#define PPA_SRM_COLOR_MODE_RGB565 1
#define PPA_SRM_ROTATION_ANGLE_90 1
#define BSP_FEATURE_LCD 1
#define BSP_TAB5_BOARD_VERSION_ST7121 1
#define BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS 1000
#define pdFALSE 0
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
typedef int BaseType_t;
typedef void *SemaphoreHandle_t;
typedef void *QueueHandle_t;
typedef void *esp_lcd_panel_handle_t;
typedef void *esp_lcd_panel_io_handle_t;
typedef void *esp_lcd_touch_handle_t;
typedef void *ppa_client_handle_t;
typedef void *i2c_master_bus_handle_t;
typedef void *esp_io_expander_handle_t;
typedef int bsp_tab5_board_version_t;
typedef struct {int unused;} esp_lcd_dpi_panel_event_data_t;
typedef struct {int unused;} lv_event_t;
typedef struct {int x1,y1,x2,y2;} lv_area_t;
typedef struct {bool last,flushing; int ready;} lv_display_t;
typedef struct {int task_priority,task_stack,task_affinity,task_max_sleep_ms,timer_period_ms;} lvgl_port_cfg_t;
typedef struct {struct {int lane_bit_rate_mbps;} dsi_bus;} bsp_display_config_t;
typedef struct {int oper_type;} ppa_client_config_t;
typedef struct {int unused;} bsp_touch_config_t;
typedef struct {lv_display_t *disp; esp_lcd_touch_handle_t handle;} lvgl_port_touch_cfg_t;
typedef struct {bool (*on_refresh_done)(esp_lcd_panel_handle_t,esp_lcd_dpi_panel_event_data_t*,void*);} esp_lcd_dpi_panel_event_callbacks_t;
typedef struct {const void *buffer; unsigned pic_w,pic_h,block_w,block_h; unsigned block_offset_x,block_offset_y; int srm_cm;} test_ppa_in;
typedef struct {void *buffer; unsigned buffer_size,pic_w,pic_h,block_offset_x,block_offset_y; int srm_cm;} test_ppa_out;
typedef struct {test_ppa_in in; test_ppa_out out; int rotation_angle; float scale_x,scale_y;int mode;} ppa_srm_oper_config_t;
static int lv_area_get_width(const lv_area_t*r){return r->x2-r->x1+1;}
static int lv_area_get_height(const lv_area_t*r){return r->y2-r->y1+1;}
static int lv_area_get_size(const lv_area_t*r){return lv_area_get_width(r)*lv_area_get_height(r);}
static uint64_t fake_time;
static unsigned publications,edges_after_publish;
static uint64_t present_sync_pixels;
static int present_first_row,present_last_row;
static bool stop_vsync;
static bool ppa_error_next, publish_error_next, queue_error_next;
static void (*host_progress)(void);
typedef struct {bool vsync;unsigned count;} host_semaphore_t;
static host_semaphore_t host_semaphores[4];
static unsigned host_semaphore_count;
typedef struct {unsigned size;bool queued;uint8_t data[128];} host_queue_t;
static host_queue_t host_queue;
static unsigned host_worker_calls;
static unsigned startup_i2c_failures,startup_expander_failures,startup_feature_failures;
static unsigned startup_expander_creates,startup_feature_calls,startup_bus_resets;
static unsigned startup_lvgl_calls,startup_panel_calls,startup_delays,startup_delay_ms;
/* IDF ESP32-P4 ICM fields: DW-GDMA master 0 is bits 12..15, memory
 * master 1 is bits 16..19. Retain the other register bits as sentinels. */
static struct {
    union {struct {uint32_t other_low:16,reg_gdma_mst2_awqos:4,other_high:12;};uint32_t val;} mst_awqos_reg0;
    union {struct {uint32_t other_low:16,reg_gdma_mst2_arqos:4,other_high:12;};uint32_t val;} mst_arqos_reg0;
} AXI_ICM;
static unsigned scanout_qos_calls;
static void axi_icm_ll_set_dw_gdma_qos_arbiter_prio(uint32_t port,uint32_t write_priority,uint32_t read_priority){
    assert(port<=1&&write_priority<16&&read_priority<16);
    const unsigned shift=port?16:12;const uint32_t mask=15u<<shift;
    AXI_ICM.mst_awqos_reg0.val=(AXI_ICM.mst_awqos_reg0.val&~mask)|(write_priority<<shift);
    AXI_ICM.mst_arqos_reg0.val=(AXI_ICM.mst_arqos_reg0.val&~mask)|(read_priority<<shift);
    ++scanout_qos_calls;
}
static bool startup_bus_ready,startup_expander_cached,startup_reset_failure;
static uint16_t *scanning, *published_frame;
static uint32_t *sequence_ptr;
static int64_t esp_timer_get_time(void){return (int64_t)fake_time;}
static int xSemaphoreTake(void*s,unsigned ticks){host_semaphore_t *sem=s;
    if(sem->vsync){fake_time+=(uint64_t)ticks*1000;
      if(!stop_vsync){++*sequence_ptr; ++edges_after_publish; scanning=published_frame;} return !stop_vsync;}
    if(!sem->count&&ticks){assert(host_progress);host_progress();}
    if(!sem->count)return pdFALSE;--sem->count;return pdTRUE;}
static int xSemaphoreGive(void*s){host_semaphore_t *sem=s;assert(!sem->count);++sem->count;return pdTRUE;}
static void xSemaphoreGiveFromISR(void*s,BaseType_t*w){(void)s;*w=0;}
static void*xSemaphoreCreateBinary(void){assert(host_semaphore_count<4);host_semaphore_t *s=&host_semaphores[host_semaphore_count];s->vsync=host_semaphore_count++==0;return s;}
static void vSemaphoreDelete(void*s){(void)s;}
static QueueHandle_t xQueueCreate(unsigned n,unsigned size){assert(n==1&&size<=sizeof(host_queue.data));host_queue.size=size;return &host_queue;}
static int xQueueSend(void*q,const void*data,unsigned ticks){(void)ticks;host_queue_t *queue=q;
    if(queue_error_next){queue_error_next=false;return pdFALSE;}
    assert(!queue->queued);memcpy(queue->data,data,queue->size);queue->queued=true;return pdTRUE;}
static int xQueueReceive(void*q,void*data,unsigned ticks){(void)ticks;host_queue_t *queue=q;
    if(!queue->queued)return pdFALSE;memcpy(data,queue->data,queue->size);queue->queued=false;return pdTRUE;}
static void vQueueDelete(void*q){(void)q;}
static int xTaskCreatePinnedToCore(void(*fn)(void*),const char*name,unsigned stack,void*arg,unsigned priority,void*handle,int core){
    (void)fn;(void)name;(void)arg;(void)handle;assert(stack>=4096&&priority==5&&core==0);return pdPASS;}
static esp_err_t esp_cache_msync(void*p,size_t n,unsigned flags){(void)p;(void)n;(void)flags;return 0;}
static esp_err_t esp_lcd_panel_draw_bitmap(void*p,int x,int y,int w,int h,const void*data){
    (void)p;assert(x==0&&y>=0&&w==720&&h<=1280&&h>y);
    if(publish_error_next){publish_error_next=false;return ESP_FAIL;}
    // Match IDF DPI: a driver-owned framebuffer pointer flips the whole image;
    // Y bounds only select the cache rows. They do not crop/offset scanout.
    present_sync_pixels+=(uint64_t)(h-y)*720;present_first_row=y;present_last_row=h-1;
    ++publications;edges_after_publish=0;published_frame=(uint16_t*)data;return 0;}
#define esp_err_to_name(e) "test"
static bool lv_display_flush_is_last(lv_display_t*d){return d->last;}
static void lv_display_flush_ready(lv_display_t*d){d->flushing=false;++d->ready;}
static esp_err_t ppa_do_scale_rotate_mirror(void*p,const ppa_srm_oper_config_t*o){
    (void)p;assert(o->out.buffer != scanning); assert(o->rotation_angle==0||o->rotation_angle==PPA_SRM_ROTATION_ANGLE_90);
    if(ppa_error_next){ppa_error_next=false;return ESP_FAIL;}
    fake_time+=20000;
    const uint16_t*src=o->in.buffer;uint16_t*dst=o->out.buffer;
    for(unsigned y=0;y<o->in.block_h;y++) for(unsigned x=0;x<o->in.block_w;x++)
      if(o->rotation_angle==PPA_SRM_ROTATION_ANGLE_90) dst[(o->out.block_offset_y+o->in.block_w-1-x)*o->out.pic_w+o->out.block_offset_x+y]=src[y*o->in.pic_w+x];
      else dst[(o->out.block_offset_y+y)*o->out.pic_w+o->out.block_offset_x+x]=src[(o->in.block_offset_y+y)*o->in.pic_w+o->in.block_offset_x+x];
    return 0;}
static void*heap_caps_aligned_alloc(unsigned a,size_t n,unsigned c){(void)a;(void)c;return malloc(n);}
static int lvgl_port_init(const lvgl_port_cfg_t*p){(void)p;++startup_lvgl_calls;return 0;}
static bool lvgl_port_lock(unsigned t){(void)t;return true;}
static void lvgl_port_unlock(void){}
static void lvgl_port_task_wake(int event,void*p){(void)event;(void)p;}
static int bsp_i2c_init(void){if(startup_i2c_failures){--startup_i2c_failures;return ESP_FAIL;}startup_bus_ready=true;return ESP_OK;}
static i2c_master_bus_handle_t bsp_i2c_get_handle(void){return startup_bus_ready?(void*)2:NULL;}
static esp_io_expander_handle_t bsp_io_expander_init(void){
    assert(startup_bus_ready);
    if(startup_expander_cached)return(void*)3;
    ++startup_expander_creates;
    // Model actual BSP/PI4 constructor: failed reset frees the temporary device
    // and never assigns BSP's cached handle. A successful one is then reused.
    if(startup_expander_failures){--startup_expander_failures;return NULL;}
    startup_expander_cached=true;return(void*)3;}
static int bsp_feature_enable(int f,bool v){assert(f==BSP_FEATURE_LCD&&v);++startup_feature_calls;
    assert(startup_expander_cached&&bsp_io_expander_init());
    if(startup_feature_failures){--startup_feature_failures;return ESP_FAIL;}return ESP_OK;}
static int i2c_master_bus_reset(i2c_master_bus_handle_t bus){assert(bus&&startup_bus_ready);++startup_bus_resets;return startup_reset_failure?ESP_FAIL:ESP_OK;}
static void vTaskDelay(unsigned ms){assert(ms==50||ms==100);++startup_delays;startup_delay_ms+=ms;fake_time+=(uint64_t)ms*1000;}
static int bsp_display_new(const bsp_display_config_t*c,void**p,void**io){(void)c;
    assert(scanout_qos_calls==1&&AXI_ICM.mst_arqos_reg0.reg_gdma_mst2_arqos==15);
    ++startup_panel_calls;*p=(void*)1;*io=(void*)1;return 0;}
static int esp_lcd_dpi_panel_get_frame_buffer(void*p,int n,void**a,void**b,void**c){(void)p;assert(n==3);*a=calloc(720*1280,2);*b=calloc(720*1280,2);*c=calloc(720*1280,2);return 0;}
static int ppa_register_client(const ppa_client_config_t*c,void**p){(void)c;*p=(void*)1;return 0;}
static int esp_lcd_dpi_panel_register_event_callbacks(void*p,const esp_lcd_dpi_panel_event_callbacks_t*c,void*u){(void)p;(void)c;(void)u;return 0;}
static lv_display_t*lv_display_create(int w,int h){(void)w;(void)h;static lv_display_t d;return &d;}
static void lv_display_delete(lv_display_t*d){(void)d;}
static void lv_display_set_color_format(lv_display_t*d,int c){(void)d;(void)c;}
static void lv_display_set_buffers(lv_display_t*d,void*a,void*b,size_t n,int m){(void)d;(void)a;(void)b;(void)n;(void)m;}
static void lv_display_set_flush_cb(lv_display_t*d,void(*cb)(lv_display_t*,const lv_area_t*,uint8_t*)){(void)d;(void)cb;}
static void lv_display_set_flush_wait_cb(lv_display_t*d,void(*cb)(lv_display_t*)){(void)d;(void)cb;}
static void lv_display_set_rotation(lv_display_t*d,int r){(void)d;(void)r;}
static void lv_display_add_event_cb(lv_display_t*d,void(*cb)(lv_event_t*),int e,void*u){(void)d;(void)cb;(void)e;(void)u;}
static int display_on=1;
static int esp_lcd_panel_disp_on_off(void*p,bool v){(void)p;display_on=v;return 0;}
static int bsp_touch_new(const bsp_touch_config_t*c,void**p){(void)c;*p=(void*)1;return 0;}
static void*lvgl_port_add_touch(const lvgl_port_touch_cfg_t*c){(void)c;return(void*)1;}

typedef struct {int unused;} lv_indev_t;
typedef struct {int state;} lv_indev_data_t;
typedef void (*lv_indev_read_cb_t)(lv_indev_t*,lv_indev_data_t*);
static int raw_touch,backlight=70;
static void host_touch(lv_indev_t*i,lv_indev_data_t*d){(void)i;d->state=raw_touch;}
static lv_indev_read_cb_t lv_indev_get_read_cb(lv_indev_t*i){(void)i;return host_touch;}
static void lv_indev_set_read_cb(lv_indev_t*i,lv_indev_read_cb_t cb){(void)i;(void)cb;}
typedef struct {int paused;} lv_timer_t;
static lv_timer_t refresh_timer;
static lv_timer_t *lv_display_get_refr_timer(lv_display_t*d){(void)d;return &refresh_timer;}
static void lv_timer_pause(lv_timer_t*t){t->paused=1;}
static void lv_timer_resume(lv_timer_t*t){t->paused=0;}
#define LV_INDEV_MODE_TIMER 0
static int touch_mode=-1,touch_period=0;
static int esp_lcd_touch_register_interrupt_callback(void*t,void*cb){(void)t;assert(!cb);return 0;}
static void lv_indev_set_mode(lv_indev_t*i,int mode){(void)i;touch_mode=mode;}
static lv_timer_t *lv_indev_get_read_timer(lv_indev_t*i){(void)i;static lv_timer_t t;return &t;}
static void lv_timer_set_period(lv_timer_t*t,int period){(void)t;touch_period=period;}
static bool brightness_error, require_visible_scanout;
static int bsp_display_brightness_set(int n){
    /* Sleep/shutdown and wake must finish queued rendering before switching
     * the backlight, including the final refresh triggered on wake. */
    assert(!host_queue.queued&&!host_semaphores[1].count);
    if(n&&require_visible_scanout)assert(scanning==published_frame&&edges_after_publish>=2);
    if(brightness_error)return ESP_FAIL;backlight=n;return 0;}
static void lv_display_trigger_activity(lv_display_t*d){(void)d;}
static void (*host_refresh)(void);
static void lv_refr_now(void*p){(void)p;if(host_refresh)host_refresh();}
