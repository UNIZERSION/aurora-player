#ifdef TAB5_DISPLAY_HOST_TEST
#include "tab5_display_host.h"
#else
#include "tab5_hal.h"
#include <string.h>
#include <stdlib.h>
#include "bsp/m5stack_tab5.h"
#include "bsp/touch.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lvgl_port.h"
#include "esp_lvgl_port_touch.h"
#include "esp_timer.h"
#include "hal/axi_icm_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#endif

/* BSP's stock avoid-tear mode disables rotation. Keep LVGL's efficient partial
 * rendering, but rotate into a hidden native framebuffer, then publish once per
 * complete LVGL refresh. Never expose an unfinished settings/menu repaint. */
#define NATIVE_W 720
#define NATIVE_H 1280
#define FRAME_BYTES (NATIVE_W * NATIVE_H * 2)
#define DRAW_PIXELS (1280 * 480)
#define MAX_DAMAGE 64

static const char *TAG = "tab5_display";
static esp_lcd_panel_handle_t panel;
static esp_lcd_panel_io_handle_t panel_io;
static lv_display_t *screen;
static lv_indev_read_cb_t original_touch_read;
static lv_indev_t *touch_input;

static bool asleep, wake_requested, consume_touch;
static ppa_client_handle_t ppa;
static uint8_t *frames[3];
static unsigned latest=0;
static uint32_t retired_until[3];
static bool retired[3];
static lv_area_t stale[3][MAX_DAMAGE];
static unsigned stale_count[3];
static unsigned back = 1; /* IDF starts scanout using fb0. */
static SemaphoreHandle_t vsync;
static uint32_t vsync_sequence;
static lv_area_t damage[MAX_DAMAGE];
static unsigned damage_count;
static bool failed, pending;
static uint32_t published_sequence;
static int64_t published_at;
static uint32_t presents, last_log;
static uint64_t copied_pixels;
static uint64_t rotation_us, repair_us, fence_us, publish_us;
static uint32_t flush_max_us;

typedef struct {
    lv_area_t area;
    const uint8_t *pixels;
    bool last;
} present_job_t;
static QueueHandle_t presentation_queue;
static SemaphoreHandle_t presentation_done;
/* Only the LVGL task accesses this flag. The worker owns all native-frame
 * state until its completion semaphore is consumed. No LVGL lock in worker. */
static bool transfer_pending;

static bool IRAM_ATTR on_refresh(esp_lcd_panel_handle_t handle,
                                esp_lcd_dpi_panel_event_data_t *event,
                                void *ctx)
{
    (void)handle; (void)event; (void)ctx;
    __atomic_add_fetch(&vsync_sequence, 1, __ATOMIC_RELEASE);
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(vsync, &wake);
    return wake == pdTRUE;
}

static void add_damage(lv_area_t *list,unsigned *count,lv_area_t rect)
{
    for(unsigned i=0;i<*count;i++) {
        lv_area_t *old=&list[i];
        bool adjacent=rect.x1<=old->x2+1&&rect.x2>=old->x1-1&&rect.y1<=old->y2+1&&rect.y2>=old->y1-1;
        if(adjacent) {
            if(rect.x1<old->x1)old->x1=rect.x1;
            if(rect.y1<old->y1)old->y1=rect.y1;
            if(rect.x2>old->x2)old->x2=rect.x2;
            if(rect.y2>old->y2)old->y2=rect.y2;
            return;
        }
    }
    if(*count==MAX_DAMAGE){list[0]=(lv_area_t){0,0,NATIVE_W-1,NATIVE_H-1};*count=1;}
    else list[(*count)++]=rect;
}
static void remember_damage(lv_area_t rect){add_damage(damage,&damage_count,rect);}

static esp_err_t rotate_piece(const lv_area_t *area, const uint8_t *pixels)
{
    const uint32_t width = lv_area_get_width(area);
    const uint32_t height = lv_area_get_height(area);
    /* Same CCW90 transform as esp_lvgl_port 2.8's PPA rotation: screen90
     * logical coordinates become native (y, logical_width - 1 - x). */
    lv_area_t native = {area->y1, NATIVE_H - 1 - area->x2,
                        area->y2, NATIVE_H - 1 - area->x1};
    const ppa_srm_oper_config_t operation = {
        .in = {.buffer = pixels, .pic_w = width, .pic_h = height,
               .block_w = width, .block_h = height, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .out = {.buffer = frames[back], .buffer_size = FRAME_BYTES,
                .pic_w = NATIVE_W, .pic_h = NATIVE_H,
                .block_offset_x = native.x1, .block_offset_y = native.y1,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
        .scale_x = 1.0f, .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    const int64_t started = esp_timer_get_time();
    esp_err_t err = ppa_do_scale_rotate_mirror(ppa, &operation);
    rotation_us += esp_timer_get_time() - started;
    if (err == ESP_OK) remember_damage(native);
    return err;
}

/* Three physical framebuffers: render into a free one while a complete
 * predecessor waits for scanout. Two VSYNC edges still fence retirement,
 * because IDF 5.5 handles DPI VSYNC and DMA restart in separate interrupts. */
static esp_err_t repair_piece(unsigned candidate, lv_area_t rect)
{
    const lv_area_t *r=&rect;
    const ppa_srm_oper_config_t repair={
        .in={.buffer=frames[latest],.pic_w=NATIVE_W,.pic_h=NATIVE_H,
             .block_w=lv_area_get_width(r),.block_h=lv_area_get_height(r),
             .block_offset_x=r->x1,.block_offset_y=r->y1,.srm_cm=PPA_SRM_COLOR_MODE_RGB565},
        .out={.buffer=frames[candidate],.buffer_size=FRAME_BYTES,.pic_w=NATIVE_W,.pic_h=NATIVE_H,
              .block_offset_x=r->x1,.block_offset_y=r->y1,.srm_cm=PPA_SRM_COLOR_MODE_RGB565},
        .rotation_angle=PPA_SRM_ROTATION_ANGLE_0,.scale_x=1.f,.scale_y=1.f,.mode=PPA_TRANS_MODE_BLOCKING,
    };
    const int64_t started = esp_timer_get_time();
    esp_err_t err = ppa_do_scale_rotate_mirror(ppa,&repair);
    repair_us += esp_timer_get_time() - started;
    ESP_RETURN_ON_ERROR(err,TAG,"repair retired buffer");
    copied_pixels+=(uint64_t)lv_area_get_size(r);
    return ESP_OK;
}

static esp_err_t finish_present(const lv_area_t *overwrite)
{
    if(!pending)return ESP_OK;
    const int64_t wait_started = esp_timer_get_time();
    unsigned candidate=3;
    for(;;) {
        uint32_t now=__atomic_load_n(&vsync_sequence,__ATOMIC_ACQUIRE);
        for(unsigned i=0;i<3;i++)if(i!=latest&&(!retired[i]||(int32_t)(now-retired_until[i])>=0)) {
            if(candidate==3||stale_count[i]<stale_count[candidate])candidate=i;
        }
        if(candidate<3)break;
        if(esp_timer_get_time()-published_at>=150000)return ESP_ERR_TIMEOUT;
        (void)xSemaphoreTake(vsync,pdMS_TO_TICKS(5));
    }
    fence_us += esp_timer_get_time() - wait_started;
    for(unsigned i=0;i<stale_count[candidate];i++) {
        const lv_area_t *r=&stale[candidate][i];
        // Only omit pixels the very next rotation will certainly replace.
        // Damage history may be a bounding box, so never treat it as exact
        // write coverage. Subtract ONE actual incoming rectangle (<=4 pieces).
        if(!overwrite || overwrite->x2<r->x1 || overwrite->x1>r->x2 ||
           overwrite->y2<r->y1 || overwrite->y1>r->y2) {
            ESP_RETURN_ON_ERROR(repair_piece(candidate,*r),TAG,"repair full");
            continue;
        }
        int left=overwrite->x1>r->x1?overwrite->x1:r->x1;
        int right=overwrite->x2<r->x2?overwrite->x2:r->x2;
        int top=overwrite->y1>r->y1?overwrite->y1:r->y1;
        int bottom=overwrite->y2<r->y2?overwrite->y2:r->y2;
        if(r->y1<top) ESP_RETURN_ON_ERROR(repair_piece(candidate,(lv_area_t){r->x1,r->y1,r->x2,top-1}),TAG,"repair top");
        if(bottom<r->y2) ESP_RETURN_ON_ERROR(repair_piece(candidate,(lv_area_t){r->x1,bottom+1,r->x2,r->y2}),TAG,"repair bottom");
        if(r->x1<left) ESP_RETURN_ON_ERROR(repair_piece(candidate,(lv_area_t){r->x1,top,left-1,bottom}),TAG,"repair left");
        if(right<r->x2) ESP_RETURN_ON_ERROR(repair_piece(candidate,(lv_area_t){right+1,top,r->x2,bottom}),TAG,"repair right");
    }
    stale_count[candidate]=0;retired[candidate]=false;back=candidate;damage_count=0;pending=false;
    return ESP_OK;
}
static esp_err_t present(void)
{
    // The IDF DPI direct-framebuffer path flips the complete framebuffer, but
    // uses these Y bounds for cache writeback. All updates/repairs here are PPA
    // DMA writes; no CPU dirty pixels exist outside the actual damaged rows.
    // Avoid sweeping 1.84 MB of cache addresses for every knob/visual update.
    int first_row = 0, last_row = NATIVE_H - 1;
    if(damage_count) {
        first_row = damage[0].y1; last_row = damage[0].y2;
        for(unsigned i=1;i<damage_count;i++) {
            if(damage[i].y1<first_row)first_row=damage[i].y1;
            if(damage[i].y2>last_row)last_row=damage[i].y2;
        }
    }
    const int64_t started = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(panel,0,first_row,NATIVE_W,last_row+1,frames[back]),TAG,"publish frame");
    publish_us += esp_timer_get_time() - started;
    published_sequence=__atomic_load_n(&vsync_sequence,__ATOMIC_ACQUIRE);published_at=esp_timer_get_time();
    // The old published frame may have started scanning immediately before the
    // new pointer was submitted. It cannot be reused until this fence expires.
    retired[latest]=true;retired_until[latest]=published_sequence+2;latest=back;
    for(unsigned i=0;i<3;i++)if(i!=latest)for(unsigned j=0;j<damage_count;j++)add_damage(stale[i],&stale_count[i],damage[j]);
    pending=true;++presents;
    uint32_t now=(uint32_t)(published_at/1000);
    if(now-last_log>=10000){
        ESP_LOGI(TAG,"PRESENT frames=%lu copied_pixels=%llu vsync=%lu rotate_us=%llu repair_us=%llu fence_us=%llu publish_us=%llu flush_max_us=%lu",
            (unsigned long)presents,(unsigned long long)copied_pixels,(unsigned long)published_sequence,
            (unsigned long long)rotation_us,(unsigned long long)repair_us,
            (unsigned long long)fence_us,(unsigned long long)publish_us,(unsigned long)flush_max_us);
        last_log=now;rotation_us=repair_us=fence_us=publish_us=0;flush_max_us=0;
    }
    return ESP_OK;
}

static void process_presentation(const present_job_t *job)
{
    const int64_t started = esp_timer_get_time();
    const lv_area_t *area = &job->area;
    if (!failed) {
        const lv_area_t overwrite={area->y1,NATIVE_H-1-area->x2,area->y2,NATIVE_H-1-area->x1};
        esp_err_t err = finish_present(&overwrite);
        if (err == ESP_OK) err = rotate_piece(area, job->pixels);
        if (err == ESP_OK && job->last) err = present();
        if (err != ESP_OK) {
            /* Preserve the last image if synchronization fails; never write into
             * a buffer that may still be scanned out, nor hang the LVGL task. */
            failed = true;
            ESP_LOGE(TAG, "Display presentation stopped: %s", esp_err_to_name(err));
        }
    }
    const uint32_t elapsed = (uint32_t)(esp_timer_get_time()-started);
    if(elapsed>flush_max_us)flush_max_us=elapsed;
    /* Release even after a driver error: the failed state prevents all later
     * writes/publications, while LVGL remains able to process input. */
    xSemaphoreGive(presentation_done);
}

static void presentation_worker(void *arg)
{
    (void)arg;
    present_job_t job;
    for (;;) {
        if (xQueueReceive(presentation_queue, &job, portMAX_DELAY) == pdTRUE)
            process_presentation(&job);
    }
}

static void wait_for_flush(lv_display_t *display)
{
    (void)display;
    if (transfer_pending) {
        /* Never return while DMA can still read the source buffer. LVGL calls
         * this only after drawing into its OTHER buffer, so CPU and PPA overlap.
         * A custom wait callback also avoids LVGL's default busy-spin loop. */
        xSemaphoreTake(presentation_done, portMAX_DELAY);
        transfer_pending = false;
    }
}

static void drain_presentation(void)
{
    if (transfer_pending) {
        wait_for_flush(screen);
        /* Explicit drains (sleep/snapshot) occur outside LVGL's normal wait,
         * whose caller would otherwise clear this flag for us. */
        lv_display_flush_ready(screen);
    }
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    wait_for_flush(display);
    const present_job_t job = {.area = *area, .pixels = pixels,
                               .last = lv_display_flush_is_last(display)};
    transfer_pending = true;
    if (xQueueSend(presentation_queue, &job, 0) != pdTRUE) {
        /* One outstanding flush is enforced by LVGL and wait_for_flush.
         * A rejected enqueue must never leave it waiting on a nonexistent job. */
        transfer_pending = false;
        failed = true;
        ESP_LOGE(TAG, "Display presentation queue rejected a frame");
        lv_display_flush_ready(display);
    }
}

static void wake_display(lv_event_t *event)
{
    (void)event;
    lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, NULL);
}

// Runs on the LVGL task. The complete wake gesture is consumed until release.
static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    original_touch_read(indev, data);
    if (asleep && data->state == LV_INDEV_STATE_PRESSED) {wake_requested = true; consume_touch = true;}
    if (consume_touch) {if (data->state == LV_INDEV_STATE_RELEASED) consume_touch = false;data->state = LV_INDEV_STATE_RELEASED;}
}
bool tab5_display_wake_requested(void) {return wake_requested;}
bool tab5_display_is_sleeping(void) {return asleep;}
esp_err_t tab5_display_reveal(uint8_t brightness)
{
    if (!screen || asleep || failed) return ESP_ERR_INVALID_STATE;
    /* With LVGL locked, replace any invisible initialization frame with the
     * final SD logo (or the original missing-file fallback). Completing PPA
     * alone is insufficient: the DPI pointer change takes effect at VSYNC. */
    drain_presentation();
    if (failed) return ESP_ERR_INVALID_STATE;
    lv_refr_now(screen);
    drain_presentation();
    if (failed || !presents) return ESP_ERR_INVALID_STATE;
    const int64_t started = esp_timer_get_time();
    while ((int32_t)(__atomic_load_n(&vsync_sequence, __ATOMIC_ACQUIRE) - published_sequence) < 2) {
        if (esp_timer_get_time() - started >= 150000) return ESP_ERR_TIMEOUT;
        (void)xSemaphoreTake(vsync, pdMS_TO_TICKS(5));
    }
    ESP_RETURN_ON_ERROR(bsp_display_brightness_set(brightness > 100 ? 100 : brightness), TAG, "first frame backlight");
    lv_display_trigger_activity(screen);
    return ESP_OK;
}
esp_err_t tab5_display_sleep(bool sleep, uint8_t brightness)
{
    /* Also the shutdown barrier. Called with LVGL locked, so no new refresh
     * can enqueue while we drain. The worker deliberately never takes that lock. */
    drain_presentation();
    if (sleep == asleep) return ESP_OK;
    // Fully extinguish LED backlight. Keep integrated touch/display controller
    // powered: panel sleep on ST712x also prevents touch wake. UI/FFT work is
    // suspended separately; idle scanout is hardware DMA, not CPU rendering.
    if(!sleep) {
        lv_timer_resume(lv_display_get_refr_timer(screen));
        lv_refr_now(screen); // Restore a complete UI frame before lighting it.
        drain_presentation();
        if (failed) {
            lv_timer_pause(lv_display_get_refr_timer(screen));
            return ESP_ERR_INVALID_STATE;
        }
    }
    esp_err_t err=bsp_display_brightness_set(sleep ? 0 : brightness);
    if(err!=ESP_OK) {
        if(!sleep)lv_timer_pause(lv_display_get_refr_timer(screen));
        return err;
    }
    if(sleep)lv_timer_pause(lv_display_get_refr_timer(screen));
    asleep=sleep;wake_requested=false;
    if(!sleep)lv_display_trigger_activity(screen);
    return ESP_OK;
}

esp_err_t tab5_display_snapshot_rgb565(uint16_t *out, size_t pixels)
{
    if (!out || pixels < NATIVE_W*NATIVE_H || asleep) return ESP_ERR_INVALID_STATE;
    drain_presentation();
    if (failed) return ESP_ERR_INVALID_STATE;
    lv_refr_now(screen);
    drain_presentation();
    if (failed) return ESP_ERR_INVALID_STATE;
    const uint16_t *src=(const uint16_t *)frames[latest];
    ESP_RETURN_ON_ERROR(esp_cache_msync((void*)src,FRAME_BYTES,ESP_CACHE_MSYNC_FLAG_DIR_M2C),TAG,"snapshot cache");
    // Tiled transpose preserves native orientation and cache locality.
    for(int ty=0;ty<720;ty+=16)for(int tx=0;tx<1280;tx+=16)
        for(int y=ty;y<ty+16 && y<720;y++)for(int x=tx;x<tx+16;x++)out[y*1280+x]=src[(1279-x)*720+y];
    return ESP_OK;
}

static uint32_t panel_lane_rate(void)
{
#ifdef TAB5_DISPLAY_HOST_TEST
    return 965;
#else
    /* BSP1.3.2 keeps its board-detection helper private, but requires the caller
     * to choose ST7121's965Mbps instead of the generic1000Mbps. The documented
     * ST712x touch firmware byte at register0 is1 for ST7121 and3 for ST7123.
     * Only probe/read here; BSP retains all reset and panel-init sequences. */
    if (bsp_feature_enable(BSP_FEATURE_TOUCH, true) != ESP_OK) return 1000;
    vTaskDelay(pdMS_TO_TICKS(500));
    if (i2c_master_probe(bsp_i2c_get_handle(), 0x55, 100) != ESP_OK) return 1000;
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x55,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t touch_id;
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &config, &touch_id) != ESP_OK) return 1000;
    const uint8_t reg[] = {0, 0};
    uint8_t fw = 0;
    const esp_err_t err = i2c_master_transmit_receive(touch_id, reg, 2, &fw, 1, 100);
    i2c_master_bus_rm_device(touch_id);
    return err == ESP_OK && fw == 1 ? 965 : 1000;
#endif
}

static esp_err_t enable_lcd_power(void)
{
    esp_err_t err = ESP_FAIL;
    bool bus_reset_attempted = false;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        err = bsp_i2c_init();
        i2c_master_bus_handle_t bus = err == ESP_OK ? bsp_i2c_get_handle() : NULL;
        if (err == ESP_OK && !bus) err = ESP_ERR_INVALID_STATE;
        if (err == ESP_OK) {
            /* BSP caches the PI4 handle only after its complete constructor
             * succeeds. On reset failure the constructor frees its device and
             * leaves that cache NULL, so retrying is safe. feature_enable itself
             * does not guard NULL and otherwise masks the original failure. */
            err = bsp_io_expander_init() ? bsp_feature_enable(BSP_FEATURE_LCD, true) : ESP_FAIL;
        }
        if (err == ESP_OK) return ESP_OK;
        ESP_LOGW(TAG, "LCD power attempt %u/3 failed: %s", attempt + 1, esp_err_to_name(err));
        if (attempt == 2) break;
        /* Startup only, before touch/audio/RTC or LVGL exist. Reset the master
         * FSM once without deleting the bus or invalidating BSP device caches.
         * This API is not safe to use concurrently with live I2C transactions. */
        if (bus && !bus_reset_attempted) {
            bus_reset_attempted = true;
            esp_err_t reset_error = i2c_master_bus_reset(bus);
            if (reset_error != ESP_OK)
                ESP_LOGW(TAG, "Startup I2C bus recovery failed: %s", esp_err_to_name(reset_error));
        }
        vTaskDelay(pdMS_TO_TICKS(50 * (attempt + 1)));
    }
    return err;
}

static void prioritize_scanout_reads(void)
{
    /* IDF's DPI framebuffer reader uses DW-GDMA memory master 1 (master 0
     * feeds the DSI peripheral). A missed PSRAM deadline can make the panel
     * flash blue. Give these continuous reads priority over bursty redraw/SD
     * traffic, without changing write priorities or other masters. Apply once
     * at startup, before BSP starts scanout and other DMA clients are active. */
    const uint32_t write_priority = AXI_ICM.mst_awqos_reg0.reg_gdma_mst2_awqos;
    const uint32_t previous_read_priority = AXI_ICM.mst_arqos_reg0.reg_gdma_mst2_arqos;
    axi_icm_ll_set_dw_gdma_qos_arbiter_prio(1, write_priority, 15);
    ESP_LOGI(TAG, "DSI memory QoS read=%lu->%lu write=%lu (other masters unchanged)",
             (unsigned long)previous_read_priority,
             (unsigned long)AXI_ICM.mst_arqos_reg0.reg_gdma_mst2_arqos,
             (unsigned long)AXI_ICM.mst_awqos_reg0.reg_gdma_mst2_awqos);
    (void)previous_read_priority; /* Also build when informational logs are disabled. */
}

esp_err_t tab5_display_atomic_start(lv_display_t **result, bool *touch_ready)
{
    *result = NULL;
    *touch_ready = false;
    const lvgl_port_cfg_t port = {
        /* LVGL9.3 TJPGD decoder_info reserves4KiB even when rejecting an RGB
         * source. The measured8KiB task crossed its guard during initial draw;
         *16KiB leaves another8KiB for the remaining draw/flush call chain. */
        .task_priority = 4, .task_stack = 16384, .task_affinity = 0,
        .task_max_sleep_ms = 100, .timer_period_ms = 5,
    };
    /* A transient first I2C transaction must not leave a permanent black boot
     * or leak duplicate LVGL tasks on retries. Recover this stage before any
     * display/task/buffer allocation, never rerun the complete display init. */
    ESP_RETURN_ON_ERROR(enable_lcd_power(), TAG, "LCD power");
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port), TAG, "LVGL task");
    /* Detect board before creating DSI, retaining BSP's proven initialization
     * and ST7121 FW1 timing/965 Mbps lane configuration. */
    bsp_display_config_t config = {.dsi_bus = {.lane_bit_rate_mbps = panel_lane_rate()}};
    esp_lcd_panel_io_handle_t io;
    prioritize_scanout_reads();
    ESP_RETURN_ON_ERROR(bsp_display_new(&config, &panel, &io), TAG, "BSP panel");
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(panel, 3,
                        (void **)&frames[0], (void **)&frames[1], (void **)&frames[2]), TAG, "three framebuffers");
    const ppa_client_config_t ppa_config = {.oper_type = PPA_OPERATION_SRM};
    ESP_RETURN_ON_ERROR(ppa_register_client(&ppa_config, &ppa), TAG, "PPA rotation");
    vsync = xSemaphoreCreateBinary();
    if (!vsync) return ESP_ERR_NO_MEM;
    const esp_lcd_dpi_panel_event_callbacks_t callbacks = {.on_refresh_done = on_refresh};
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_register_event_callbacks(panel, &callbacks, NULL), TAG, "VSYNC callback");
    const unsigned caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA;
    void *draw0 = heap_caps_aligned_alloc(64, DRAW_PIXELS * 2, caps);
    void *draw1 = heap_caps_aligned_alloc(64, DRAW_PIXELS * 2, caps);
    if (!draw0 || !draw1) { free(draw0); free(draw1); return ESP_ERR_NO_MEM; }
    if (!lvgl_port_lock(1000)) { free(draw0); free(draw1); return ESP_ERR_TIMEOUT; }
    lv_display_t *display = lv_display_create(NATIVE_W, NATIVE_H);
    if (!display) { lvgl_port_unlock(); free(draw0); free(draw1); return ESP_ERR_NO_MEM; }
    presentation_queue = xQueueCreate(1, sizeof(present_job_t));
    presentation_done = xSemaphoreCreateBinary();
    if (!presentation_queue || !presentation_done ||
        xTaskCreatePinnedToCore(presentation_worker, "tab5_present", 6144, NULL, 5, NULL, 0) != pdPASS) {
        if(presentation_queue)vQueueDelete(presentation_queue);
        if(presentation_done)vSemaphoreDelete(presentation_done);
        lv_display_delete(display);lvgl_port_unlock();free(draw0);free(draw1);
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw0, draw1, DRAW_PIXELS * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_flush_wait_cb(display, wait_for_flush);
    lv_display_set_rotation(display, LV_DISPLAY_ROTATION_90);
    lv_display_add_event_cb(display, wake_display, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(display, wake_display, LV_EVENT_REFR_REQUEST, NULL);
    lvgl_port_unlock();
    *result = display;
    screen=display;panel_io=io;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "LCD on");
    esp_lcd_touch_handle_t touch = NULL;
    const bsp_touch_config_t touch_config = {0};
    esp_err_t touch_error = bsp_touch_new(&touch_config, &touch);
    if (touch_error == ESP_OK) {
        const lvgl_port_touch_cfg_t touch_port = {.disp = display, .handle = touch};
        lv_indev_t *input = lvgl_port_add_touch(&touch_port);
        *touch_ready = input != NULL;
        if (input) {
            lvgl_port_lock(1000);
            original_touch_read=lv_indev_get_read_cb(input);lv_indev_set_read_cb(input,touch_read);touch_input=input;
            // One regular polling path, rather than IRQ + a second fallback
            // reader. Missed edges can no longer stop a drag or wake gesture.
            esp_lcd_touch_register_interrupt_callback(touch,NULL);
            lv_indev_set_mode(input,LV_INDEV_MODE_TIMER);
            lv_timer_set_period(lv_indev_get_read_timer(input),16);
            lvgl_port_unlock();
        }
    }
    ESP_LOGI(TAG, "Atomic landscape: 3 native framebuffers, partial PPA rotation, touch=%d", *touch_ready);
    return *touch_ready ? ESP_OK : (touch_error == ESP_OK ? ESP_FAIL : touch_error);
}
