#ifdef TAB5_KEYBOARD_HOST_TEST
#include "tab5_keyboard_host.h"
#else
#include "tab5_keyboard.h"
#include "bsp/m5stack_tab5.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#endif
#include <stdatomic.h>

/* The internal touch/codec/RTC bus uses BSP_I2C_NUM on GPIO31/32. This
 * external controller never resets or reconfigures the internal BSP bus. */
_Static_assert(BSP_I2C_NUM == 0 || BSP_I2C_NUM == 1, "Expected one of two P4 I2C controllers");
/* 50 ms: a transfer that times out after it started leaves IDF 5.5.2's
 * i2c_master read state stale (see connect_keyboard). */
enum { KB_ADDR = 0x6d, KB_TIMEOUT_MS = 50, KB_IRQ = 50,
       REG_INT_CFG = 0x00, REG_INT_STATUS = 0x01, REG_COUNT = 0x02,
       REG_MODE = 0x10, REG_HID = 0x30, REG_VERSION = 0xfe, REG_ADDRESS = 0xff };
static const char *TAG = "tab5_keyboard";
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t device;
static QueueHandle_t events;
static TaskHandle_t worker;
static atomic_bool connected;
static tab5_keyboard_protocol_t protocol;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static esp_err_t read_reg(uint8_t reg, uint8_t *value, size_t size)
{
    return i2c_master_transmit_receive(device, &reg, 1, value, size, KB_TIMEOUT_MS);
}
static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t bytes[] = { reg, value };
    return i2c_master_transmit(device, bytes, sizeof(bytes), KB_TIMEOUT_MS);
}
static void reset_input(void)
{
    tab5_keyboard_protocol_reset(&protocol);
    xQueueReset(events);
}
static void disconnect(void)
{
    bool was_connected = atomic_exchange(&connected, false);
    reset_input();
    if (was_connected) ESP_LOGI(TAG, "A164 disconnected; pending keys cleared");
}
static bool connect_keyboard(void)
{
    /* Detect with an ordinary register read, never i2c_master_probe(). In IDF
     * 5.5.2 a probe keeps the read state of an earlier failed read and points
     * the ISR at its own 2-entry stack array: the next completion interrupt
     * then stores through a saved register (Store access fault at
     * i2c_master.c:752). An absent keyboard simply NACKs this read. */
    uint8_t identity[2], mode;
    if (read_reg(REG_VERSION, identity, sizeof(identity)) != ESP_OK || identity[1] != KB_ADDR ||
        identity[0] == 0xff || read_reg(REG_MODE, &mode, 1) != ESP_OK || mode > 2) return false;
    /* Volatile settings only: never touch IAP/reset0xFD or flash address0xFF.
     * HID retains firmware Aa/Sym handling and provides release events. */
    if (write_reg(REG_MODE, 1) != ESP_OK || write_reg(REG_INT_CFG, 2) != ESP_OK ||
        write_reg(REG_COUNT, 0) != ESP_OK || write_reg(REG_INT_STATUS, 0) != ESP_OK ||
        read_reg(REG_MODE, &mode, 1) != ESP_OK || mode != 1) return false;
    reset_input();
    atomic_store(&connected, true);
    ESP_LOGI(TAG, "A164 connected, firmware=%u, HID, SDA0/SCL1", identity[0]);
    return true;
}
static bool enqueue(const tab5_keyboard_event_t *event)
{
    if (xQueueSend(events, event, 0) == pdTRUE) return true;
    /* Drop the backlog rather than replay old controls later. Suppress held
     * repeat; the following physical key can be handled normally. */
    reset_input();
    return false;
}
static bool drain(void)
{
    uint8_t count = 0;
    if (read_reg(REG_COUNT, &count, 1) != ESP_OK || count > 32) return false;
    for (unsigned i = 0; i < count; ++i) {
        uint8_t report[2];
        if (read_reg(REG_HID, report, sizeof(report)) != ESP_OK) return false;
        tab5_keyboard_event_t event;
        if (tab5_keyboard_protocol_hid(&protocol, report[0], report[1], now_ms(), &event))
            (void)enqueue(&event);
    }
    /* A racing arrival after this count is covered by the periodic fallback
     * which checks COUNT regardless of interrupt state. No lost-edge hang. */
    return write_reg(REG_INT_STATUS, 0) == ESP_OK;
}
static void keyboard_worker(void *unused)
{
    (void)unused;
    uint32_t next_probe = 0, next_health = 0;
    for (;;) {
        uint32_t now = now_ms();
        if (!atomic_load(&connected)) {
            if ((int32_t)(now - next_probe) >= 0) {
                if (connect_keyboard()) next_health = now + 1000;
                next_probe = now_ms() + 2000;
            }
            /* Ignore noisy unplugged IRQs until the bounded probe deadline. */
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        /* IRQ wakes promptly. A 20ms fallback also advances host repeat and
         * reads the FIFO if an interrupt edge was missed or not installed. */
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        bool ok = drain();
        now = now_ms();
        if (ok && (int32_t)(now - next_health) >= 0) {
            uint8_t mode;
            ok = read_reg(REG_MODE, &mode, 1) == ESP_OK && mode == 1;
            next_health = now + 1000;
        }
        if (!ok) {
            disconnect();
            next_probe = now + 2000;
            continue;
        }
        tab5_keyboard_event_t event;
        if (tab5_keyboard_protocol_repeat(&protocol, now, &event)) (void)enqueue(&event);
        vTaskDelay(pdMS_TO_TICKS(1)); /* Bound IRQ storms; let idle/audio run. */
    }
}
static void IRAM_ATTR keyboard_irq(void *unused)
{
    (void)unused;
    if (!worker) return;
    BaseType_t wake = pdFALSE;
    vTaskNotifyGiveFromISR(worker, &wake);
    if (wake) portYIELD_FROM_ISR();
}
esp_err_t tab5_keyboard_init(void)
{
    if (worker) return ESP_OK;
    const i2c_master_bus_config_t cfg = {
        .i2c_port = 1 - BSP_I2C_NUM, .sda_io_num = GPIO_NUM_0, .scl_io_num = GPIO_NUM_1,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &bus);
    if (err != ESP_OK) return err;
    const i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = KB_ADDR, .scl_speed_hz = 400000,
    };
    err = i2c_master_bus_add_device(bus, &dev, &device);
    if (err != ESP_OK) goto fail;
    events = xQueueCreate(64, sizeof(tab5_keyboard_event_t));
    if (!events) { err = ESP_ERR_NO_MEM; goto fail; }
    if (xTaskCreate(keyboard_worker, "tab5_keyboard", 3072, NULL, 2, &worker) != pdPASS) {
        err = ESP_ERR_NO_MEM; goto fail;
    }
    const gpio_config_t irq = {
        .pin_bit_mask = 1ULL << KB_IRQ, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    err = gpio_config(&irq);
    if (err == ESP_OK) {
        err = gpio_install_isr_service(0);
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE)
            err = gpio_isr_handler_add(KB_IRQ, keyboard_irq, NULL);
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "IRQ unavailable; 20ms polling fallback");
    return ESP_OK;
fail:
    if (events) { vQueueDelete(events); events = NULL; }
    if (device) { i2c_master_bus_rm_device(device); device = NULL; }
    if (bus) { i2c_del_master_bus(bus); bus = NULL; }
    return err;
}
bool tab5_keyboard_read_event(tab5_keyboard_event_t *event)
{
    return events && event && atomic_load(&connected) && xQueueReceive(events, event, 0) == pdTRUE;
}
bool tab5_keyboard_connected(void) { return atomic_load(&connected); }
