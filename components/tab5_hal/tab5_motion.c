/* BMI270 accelerometer FIFO adapter. Register definitions and sequencing:
 * Bosch BST-BMI270-DS000-08 sections 4.4, 4.6, 4.7 (FIFO), 5.2 and 6.5.
 * https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi270-ds000.pdf
 *
 * The existing espressif/bmi270 1.1.0 component supplies the Bosch config
 * blob. Its high-level driver uses unbounded I2C waits and starts gyro/temp,
 * so this adapter uses bounded transfers and powers only the accelerometer.
 */
#ifdef TAB5_MOTION_HOST_TEST
#include "tab5_motion_host.h"
#else
#include "tab5_hal.h"
#include "bsp/m5stack_tab5.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#include <string.h>

enum {
    MOTION_ADDR = 0x68,
    /* Shared-bus acquisition also uses this bound. Touch/codec/INA bursts
     * can exceed 10 ms without a sensor fault; 25 ms accommodates contention
     * while the worker still rejects any resulting incomplete tap sequence.
     * 200 ms: on hardware 25 ms often expired while waiting for the bus lock
     * (unlock redraw, SD wallpaper load), so each lock timeout restarted the
     * sensor. Such a timeout touches no transfer state. */
    MOTION_TIMEOUT_MS = 200,
    CONFIG_BYTES = 8192,
    CONFIG_CHUNK = 64,
    REG_CHIP_ID = 0x00,
    REG_ERR = 0x02,
    REG_INTERNAL_STATUS = 0x21,
    REG_FIFO_LENGTH = 0x24,
    REG_FIFO_DATA = 0x26,
    REG_ACC_CONF = 0x40,
    REG_ACC_RANGE = 0x41,
    REG_FIFO_DOWNS = 0x45,
    REG_FIFO_CONFIG_0 = 0x48,
    REG_FIFO_CONFIG_1 = 0x49,
    REG_INIT_CTRL = 0x59,
    REG_INIT_ADDR = 0x5b,
    REG_INIT_DATA = 0x5e,
    REG_PWR_CONF = 0x7c,
    REG_PWR_CTRL = 0x7d,
    REG_CMD = 0x7e,
    ACC_ONLY = 0x04,
    ACC_CONFIG = 0xaa, /* Performance, normal bandwidth, 400 Hz. */
    ACC_RANGE = 0x02,  /* +/-8 g; 4096 LSB/g. */
    CMD_FIFO_FLUSH = 0xb0,
    FIFO_DOWNS = 0x88,     /* Reset value: filtered accelerometer data at the full ODR. */
    FIFO_CONFIG_0 = 0x01,  /* Stop on full: no old frame is overwritten; no time frame. */
    FIFO_CONFIG_1 = 0x40,  /* Accelerometer only, headerless: 6-byte x/y/z frames. */
    FIFO_FRAME = 6,
    /* 2048-byte FIFO, about 0.85 s at 400 Hz. The worker drains it every
     * ~20 ms (backlogs of ~160 frames seen during screen-off bus contention),
     * so 256 frames (0.64 s) means the reader stalled. That is reported as a
     * discontinuity before a frame can be lost unseen: with stop-on-full the
     * sensor may stop once the free space is below its largest frame (at
     * least ~338 frames stored), and a later drain pass removes at most 40
     * frames before the next fill-level check, so any loss still shows as a
     * backlog of 298 frames or more. */
    FIFO_BACKLOG_LIMIT = 1536,
    FIFO_MAX_READ = 40,    /* 240 bytes: about 6 ms of shared-bus time per transfer. */
};

/* Exported const symbol in the pinned managed component. No copied blob and
 * no writes to managed sources. Its 8192-byte contract is checked by the HAL
 * host test against the dependency's actual definition before release. */
extern const uint8_t bmi270_config_file[CONFIG_BYTES];

static const char *TAG = "tab5_motion";
static i2c_master_dev_handle_t motion_device;
static bool motion_identified;
static bool motion_ready;
static bool motion_enabled;

static esp_err_t motion_read_register(uint8_t reg, uint8_t *data, size_t size)
{
    if (!motion_device) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(motion_device, &reg, 1, data, size, MOTION_TIMEOUT_MS);
}

static esp_err_t motion_write_register(uint8_t reg, const uint8_t *data, size_t size)
{
    if (!motion_device) return ESP_ERR_INVALID_STATE;
    if (!data || !size || size > CONFIG_CHUNK) return ESP_ERR_INVALID_ARG;
    uint8_t packet[CONFIG_CHUNK + 1];
    packet[0] = reg;
    memcpy(packet + 1, data, size);
    esp_err_t result = i2c_master_transmit(motion_device, packet, size + 1, MOTION_TIMEOUT_MS);
    /* Normal-mode inter-write minimum is 2 us. APS transitions additionally
     * wait a whole millisecond below before any further register access. */
    if (result == ESP_OK) esp_rom_delay_us(2);
    return result;
}

static esp_err_t motion_write_byte(uint8_t reg, uint8_t value)
{
    return motion_write_register(reg, &value, 1);
}

static void motion_delay_ms(unsigned ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

static esp_err_t motion_fail(esp_err_t error)
{
    motion_ready = motion_enabled = false;
    /* Best effort off, but preserve the original failure. One bounded write
     * only; repeated missing-sensor retries belong to the worker's backoff. */
    if (motion_device && motion_identified) (void)motion_write_byte(REG_PWR_CTRL, 0);
    return error;
}

static esp_err_t motion_read_fail(esp_err_t error)
{
    /* IDF also returns TIMEOUT when another device holds the shared bus.
     * No valid sample is returned. The worker discards its gesture, disables
     * the accelerometer and backs off. Preserve the loaded configuration for
     * that transient case: enable validates it before accepting more data.
     * A failed disable or any other sensor/read error still requires reset. */
    return error == ESP_ERR_TIMEOUT ? error : motion_fail(error);
}

esp_err_t tab5_motion_init(void)
{
    if (motion_ready) return ESP_OK;
    motion_enabled = false;
    motion_identified = false;
    esp_err_t result = bsp_i2c_init();
    if (result != ESP_OK) return result;
    if (!motion_device) {
        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = MOTION_ADDR,
            .scl_speed_hz = 400000,
        };
        result = i2c_master_bus_add_device(bsp_i2c_get_handle(), &config, &motion_device);
        if (result != ESP_OK) return result;
    }
    uint8_t value;
    result = motion_read_register(REG_CHIP_ID, &value, 1);
    if (result != ESP_OK) return motion_fail(result);
    /* Do not send any writes if another device occupies this address. */
    if (value != 0x24) return ESP_ERR_NOT_FOUND;
    motion_identified = true;
    result = motion_write_byte(REG_CMD, 0xb6);
    if (result != ESP_OK) return motion_fail(result);
    motion_delay_ms(10);
    result = motion_write_byte(REG_PWR_CONF, 0); /* Disable advanced power save. */
    if (result != ESP_OK) return motion_fail(result);
    motion_delay_ms(1); /* Datasheet minimum 450 us. */
    result = motion_write_byte(REG_INIT_CTRL, 0);
    if (result != ESP_OK) return motion_fail(result);

    const int64_t deadline = esp_timer_get_time() + INT64_C(1500000);
    for (unsigned offset = 0; offset < CONFIG_BYTES; offset += CONFIG_CHUNK) {
        if (esp_timer_get_time() >= deadline) return motion_fail(ESP_ERR_TIMEOUT);
        const unsigned word = offset / 2;
        const uint8_t address[] = {(uint8_t)(word & 0x0f), (uint8_t)(word >> 4)};
        result = motion_write_register(REG_INIT_ADDR, address, sizeof(address));
        if (result != ESP_OK) return motion_fail(result);
        result = motion_write_register(REG_INIT_DATA, bmi270_config_file + offset, CONFIG_CHUNK);
        if (result != ESP_OK) return motion_fail(result);
        /* Each transfer releases IDF's shared bus semaphore; this additional
         * yield avoids starving the touch/codec tasks during one-time upload. */
        if ((offset / CONFIG_CHUNK) % 8 == 7) motion_delay_ms(1);
    }
    result = motion_write_byte(REG_INIT_CTRL, 1);
    if (result != ESP_OK) return motion_fail(result);
    motion_delay_ms(20);
    result = motion_read_register(REG_INTERNAL_STATUS, &value, 1);
    if (result != ESP_OK) return motion_fail(result);
    if ((value & 0x0f) != 1) return motion_fail(ESP_ERR_INVALID_RESPONSE);
    result = motion_write_byte(REG_ACC_CONF, ACC_CONFIG);
    if (result != ESP_OK) return motion_fail(result);
    result = motion_write_byte(REG_ACC_RANGE, ACC_RANGE);
    if (result != ESP_OK) return motion_fail(result);
    result = motion_write_byte(REG_PWR_CTRL, 0);
    if (result != ESP_OK) return motion_fail(result);
    result = motion_write_byte(REG_PWR_CONF, 1); /* Standby with every sensor off. */
    if (result != ESP_OK) return motion_fail(result);
    motion_delay_ms(1);
    motion_ready = true;
    ESP_LOGI(TAG, "BMI270 ready: 400 Hz +/-8 g, accelerometer only, standby");
    return ESP_OK;
}

esp_err_t tab5_motion_set_enabled(bool enabled)
{
    if (!enabled) {
        motion_enabled = false; /* Fail closed even if the power-down write fails. */
        if (!motion_device || !motion_identified) return ESP_OK;
        /* APS may already be active. Repeated disable stays safe without an
         * unnecessary sensor restart or an unbounded suspend-mode access. */
        esp_err_t result = motion_write_byte(REG_PWR_CTRL, 0);
        motion_delay_ms(1);
        if (result != ESP_OK) { motion_ready = false; return result; }
        result = motion_write_byte(REG_PWR_CONF, 1);
        motion_delay_ms(1);
        if (result != ESP_OK) motion_ready = false;
        return result;
    }
    if (motion_enabled) return ESP_OK;
    esp_err_t result = tab5_motion_init();
    if (result != ESP_OK) return result;
    result = motion_write_byte(REG_PWR_CONF, 0);
    if (result != ESP_OK) return motion_fail(result);
    motion_delay_ms(1);
    /* Verify retained configuration before allowing samples after standby. */
    uint8_t config[2], status;
    result = motion_read_register(REG_INTERNAL_STATUS, &status, 1);
    if (result != ESP_OK) return motion_fail(result);
    result = motion_read_register(REG_ACC_CONF, config, sizeof(config));
    if (result != ESP_OK) return motion_fail(result);
    if ((status & 0x0f) != 1 || config[0] != ACC_CONFIG || config[1] != ACC_RANGE)
        return motion_fail(ESP_ERR_INVALID_RESPONSE);
    /* FIFO before the accelerometer starts: no frame of another layout. */
    const uint8_t fifo_config[2] = {FIFO_CONFIG_0, FIFO_CONFIG_1};
    result = motion_write_byte(REG_FIFO_DOWNS, FIFO_DOWNS);
    if (result == ESP_OK) result = motion_write_register(REG_FIFO_CONFIG_0, fifo_config, sizeof(fifo_config));
    if (result != ESP_OK) return motion_fail(result);
    uint8_t fifo[5]; /* FIFO_DOWNS, FIFO_WTM_0/1, FIFO_CONFIG_0/1 */
    result = motion_read_register(REG_FIFO_DOWNS, fifo, sizeof(fifo));
    if (result != ESP_OK) return motion_fail(result);
    if (fifo[0] != FIFO_DOWNS || fifo[3] != FIFO_CONFIG_0 || fifo[4] != FIFO_CONFIG_1)
        return motion_fail(ESP_ERR_INVALID_RESPONSE);
    result = motion_write_byte(REG_PWR_CTRL, ACC_ONLY);
    if (result != ESP_OK) return motion_fail(result);
    motion_delay_ms(20);
    uint8_t power;
    result = motion_read_register(REG_PWR_CTRL, &power, 1);
    if (result != ESP_OK) return motion_fail(result);
    if (power != ACC_ONLY) return motion_fail(ESP_ERR_INVALID_RESPONSE);
    /* Start-up frames, and anything retained from an earlier session, go. */
    result = motion_write_byte(REG_CMD, CMD_FIFO_FLUSH);
    if (result != ESP_OK) return motion_fail(result);
    motion_enabled = true;
    return ESP_OK;
}

esp_err_t tab5_motion_read_fifo(tab5_motion_sample_t *samples, size_t capacity, size_t *count, bool *overflow)
{
    if (count) *count = 0;
    if (overflow) *overflow = false;
    if (!samples || !capacity || !count || !overflow) return ESP_ERR_INVALID_ARG;
    if (!motion_ready || !motion_enabled) return ESP_ERR_INVALID_STATE;
    uint8_t status;
    esp_err_t result = motion_read_register(REG_ERR, &status, 1);
    if (result != ESP_OK) return motion_read_fail(result);
    /* ERR_REG fatal/internal bits are never usable motion observations. */
    if (status & 0x1f) return motion_fail(ESP_ERR_INVALID_RESPONSE);
    uint8_t length[2];
    result = motion_read_register(REG_FIFO_LENGTH, length, sizeof(length));
    if (result != ESP_OK) return motion_read_fail(result);
    const unsigned bytes = (unsigned)length[0] | ((unsigned)(length[1] & 0x3f) << 8);
    if ((status & 0x40) || bytes >= FIFO_BACKLOG_LIMIT) {
        /* fifo_err (cleared by this read) or a backlog that may already have
         * lost frames: the stream has, or may have, a hole. */
        *overflow = true;
        result = motion_write_byte(REG_CMD, CMD_FIFO_FLUSH);
        return result == ESP_OK ? ESP_OK : motion_read_fail(result);
    }
    size_t frames = bytes / FIFO_FRAME;
    if (frames > capacity) frames = capacity;
    if (frames > FIFO_MAX_READ) frames = FIFO_MAX_READ;
    if (!frames) return ESP_OK;
    uint8_t data[FIFO_MAX_READ * FIFO_FRAME];
    /* Whole frames only: a partly read frame would be delivered again. */
    result = motion_read_register(REG_FIFO_DATA, data, frames * FIFO_FRAME);
    if (result != ESP_OK) return motion_read_fail(result);
    for (size_t i = 0; i < frames; ++i) {
        const uint8_t *frame = data + i * FIFO_FRAME;
        samples[i].x = (int16_t)((uint16_t)frame[0] | ((uint16_t)frame[1] << 8));
        samples[i].y = (int16_t)((uint16_t)frame[2] | ((uint16_t)frame[3] << 8));
        samples[i].z = (int16_t)((uint16_t)frame[4] | ((uint16_t)frame[5] << 8));
    }
    *count = frames;
    return ESP_OK;
}
