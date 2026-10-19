#ifdef TAB5_ACCESSORIES_HOST_TEST
#include "tab5_accessories_host.h"
#else
#include "tab5_hal.h"
#include "bsp/m5stack_tab5.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

// Tab5 schematic / official M5Tab5-UserDemo bsp_headphone_detect:
// 0x43 P7 HIGH means a jack is inserted; P1 controls only the NS4150B amp.
// Codec/DAC remains enabled so the headphones retain both audio channels.
static const char *TAG="tab5_accessories";
static esp_io_expander_handle_t audio_io;
static SemaphoreHandle_t gate_lock;
static bool gate_requested, jack_known, jack_inserted, gate_initialized;
static unsigned removed_samples;
static int amp_applied=-1;

static esp_err_t apply_gate(void) {
    const bool enabled=gate_requested && jack_known && !jack_inserted;
    if(amp_applied==(int)enabled)return ESP_OK;
    esp_err_t err=esp_io_expander_set_level(audio_io,IO_EXPANDER_PIN_NUM_1,enabled);
    if(err==ESP_OK)amp_applied=enabled;
    return err;
}
esp_err_t tab5_speaker_gate(bool active) {
    if(!gate_initialized)return ESP_ERR_INVALID_STATE;
    if(!xSemaphoreTake(gate_lock,pdMS_TO_TICKS(100)))return ESP_ERR_TIMEOUT;
    gate_requested=active;
    esp_err_t err=apply_gate();
    xSemaphoreGive(gate_lock);
    return err;
}
// BSP's one-time codec init enables its speaker. Call immediately afterwards,
// before opening/unmuting the codec, to restore our independent amplifier gate.
esp_err_t tab5_speaker_gate_restore(void) {
    if(!gate_initialized)return ESP_ERR_INVALID_STATE;
    if(!xSemaphoreTake(gate_lock,pdMS_TO_TICKS(100)))return ESP_ERR_TIMEOUT;
    amp_applied=-1;esp_err_t err=apply_gate();xSemaphoreGive(gate_lock);return err;
}
esp_err_t tab5_headphones_poll(void) {
    if(!gate_initialized)return ESP_ERR_INVALID_STATE;
    if(!xSemaphoreTake(gate_lock,pdMS_TO_TICKS(100)))return ESP_ERR_TIMEOUT;
    uint32_t level=0;esp_err_t err=esp_io_expander_get_level(audio_io,IO_EXPANDER_PIN_NUM_7,&level);
    if(err==ESP_OK){
        const bool inserted=(level&IO_EXPANDER_PIN_NUM_7)!=0;
        // Cut the speaker on the first insertion sample; require two clean
        // removal samples before restoring it. Read failures never unmute it.
        if(inserted){jack_inserted=true;jack_known=true;removed_samples=0;}
        else if(++removed_samples>=2){jack_inserted=false;jack_known=true;removed_samples=2;}
    }else{jack_known=false;removed_samples=0;}
    esp_err_t gate=apply_gate();xSemaphoreGive(gate_lock);return err==ESP_OK?gate:err;
}
bool tab5_headphones_status(bool *inserted) {
    if(!gate_initialized||!inserted)return false;
    if(!xSemaphoreTake(gate_lock,pdMS_TO_TICKS(20)))return false;
    *inserted=jack_inserted;bool known=jack_known;xSemaphoreGive(gate_lock);return known;
}
static void jack_worker(void *unused) {
    (void)unused;
    for(;;){tab5_headphones_poll();vTaskDelay(pdMS_TO_TICKS(30));}
}
esp_err_t tab5_headphones_init(void) {
    if(gate_initialized)return ESP_OK;
    audio_io=bsp_io_expander_init();if(!audio_io)return ESP_FAIL;
    gate_lock=xSemaphoreCreateMutex();if(!gate_lock)return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(audio_io,IO_EXPANDER_PIN_NUM_7,IO_EXPANDER_INPUT),TAG,"jack input");
    // Match M5Unified's HP_DET setup. The generic expander reset enables a
    // pull-down on every pin, loading the jack's external detection circuit.
    ESP_RETURN_ON_ERROR(esp_io_expander_set_pullupdown(audio_io,IO_EXPANDER_PIN_NUM_7,IO_EXPANDER_PULL_NONE),TAG,"jack external bias");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_output_mode(audio_io,IO_EXPANDER_PIN_NUM_7,IO_EXPANDER_OUTPUT_MODE_PUSH_PULL),TAG,"jack input buffer");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(audio_io,IO_EXPANDER_PIN_NUM_1,IO_EXPANDER_OUTPUT),TAG,"speaker direction");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(audio_io,IO_EXPANDER_PIN_NUM_1,0),TAG,"speaker quiet");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_output_mode(audio_io,IO_EXPANDER_PIN_NUM_1,IO_EXPANDER_OUTPUT_MODE_PUSH_PULL),TAG,"speaker gate");
    gate_initialized=true;tab5_headphones_poll();
    if(xTaskCreate(jack_worker,"jack_detect",3072,NULL,3,NULL)!=pdPASS)return ESP_ERR_NO_MEM;
    return ESP_OK;
}
esp_err_t tab5_charger_init(void) {
    esp_io_expander_handle_t io=bsp_io_expander1_init();if(!io)return ESP_FAIL;
    // Same CHG_EN=HIGH as M5Unified. Change only the charger pins, retaining
    // WLAN, USB, shutdown and quick-charge configuration owned by other code.
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(io,IO_EXPANDER_PIN_NUM_6,IO_EXPANDER_INPUT),TAG,"charge status input");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_pullupdown(io,IO_EXPANDER_PIN_NUM_6,IO_EXPANDER_PULL_NONE),TAG,"charge external bias");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_output_mode(io,IO_EXPANDER_PIN_NUM_6,IO_EXPANDER_OUTPUT_MODE_PUSH_PULL),TAG,"charge input buffer");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(io,IO_EXPANDER_PIN_NUM_7,1),TAG,"charge enable latch");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(io,IO_EXPANDER_PIN_NUM_7,IO_EXPANDER_OUTPUT),TAG,"charge enable direction");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_output_mode(io,IO_EXPANDER_PIN_NUM_7,IO_EXPANDER_OUTPUT_MODE_PUSH_PULL),TAG,"charge enable output");
    return ESP_OK;
}
