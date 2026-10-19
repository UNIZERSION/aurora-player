#ifdef TAB5_POWER_HOST_TEST
#include "tab5_power_host.h"
#else
#include "tab5_hal.h"
#include "bsp/m5stack_tab5.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

/* Match bsp_generate_poweroff_signal in M5Stack's Tab5 UserDemo:
 * PI4IOE 0x44 pin 4, three HIGH 100 ms / LOW 100 ms cycles, ending LOW.
 * https://github.com/m5stack/M5Tab5-UserDemo/blob/main/platforms/tab5/components/m5stack_tab5/m5stack_tab5.c
 * Caller has already stopped audio/storage work and blanked the LCD.
 * Do not replace a failed physical cutoff with CPU sleep or a restart.
 */
#define POWER_OFF_PIN IO_EXPANDER_PIN_NUM_4
static const char *TAG = "tab5_power";
static const uint8_t power_registers[] = {0x03, 0x05, 0x07, 0x0b, 0x0d};
enum {POWER_DIR, POWER_OUT, POWER_HIGHZ, POWER_PULL_EN, POWER_PULL_SEL, POWER_REG_COUNT};
typedef struct {uint8_t value[POWER_REG_COUNT];} power_register_state_t;

esp_err_t tab5_rtc_release_irq(void)
{
    /* RTC /IRQ and the power-off transistor share PMS150G's trigger line.
     * RX8130 retains alarm settings across MCU resets. Aurora uses no RTC
     * wake alarms: disable UIE/TIE/AIE like M5Unified RX8130::begin(), but
     * preserve every unrelated control bit, clock/calendar and calibration.
     * RX8130 manual sections 13.2 and 14: flags are write-zero-to-clear.
     */
    i2c_master_dev_handle_t device = NULL;
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x32, .scl_speed_hz = 400000,
    };
    esp_err_t result = i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &device);
    if (result != ESP_OK) return result;
    uint8_t before[4], after[4];
    const uint8_t start = 0x1c;
    result = i2c_master_transmit_receive(device, &start, 1, before, sizeof(before), 100);
    if (result != ESP_OK) goto finished;
    ESP_LOGI(TAG, "POWER_RTC before ext=%02x flags=%02x ctrl0=%02x ctrl1=%02x",
             (unsigned)before[0], (unsigned)before[1], (unsigned)before[2], (unsigned)before[3]);
    const uint8_t disable[] = {0x1e, (uint8_t)(before[2] & ~0x38U)};
    result = i2c_master_transmit(device, disable, sizeof(disable), 100);
    if (result != ESP_OK) goto finished;
    /* Clear UF/TF/AF only. Ones preserve VBLF/RSF/VLF; reserved bit6 is 0.
     * No read-modify-write: a flag raised after the read must be retained. */
    const uint8_t clear[] = {0x1d, 0x87};
    result = i2c_master_transmit(device, clear, sizeof(clear), 100);
    if (result != ESP_OK) goto finished;
    result = i2c_master_transmit_receive(device, &start, 1, after, sizeof(after), 100);
    if (result != ESP_OK) goto finished;
    ESP_LOGI(TAG, "POWER_RTC after ext=%02x flags=%02x ctrl0=%02x ctrl1=%02x",
             (unsigned)after[0], (unsigned)after[1], (unsigned)after[2], (unsigned)after[3]);
    if (after[0] != before[0] || after[2] != disable[1] || after[3] != before[3]) {
        ESP_LOGE(TAG, "RTC IRQ release readback mismatch; no power pulse sent");
        result = ESP_ERR_INVALID_RESPONSE;
    }
    /* Flags may change autonomously even with their IRQ outputs disabled;
     * do not mistake a newly set UF/TF for a failed release. */
finished:
    if (result != ESP_OK) ESP_LOGE(TAG, "RTC IRQ release failed: %s", esp_err_to_name(result));
    const esp_err_t removed = i2c_master_bus_rm_device(device);
    if (removed != ESP_OK) ESP_LOGE(TAG, "RTC handle cleanup failed: %s", esp_err_to_name(removed));
    return result != ESP_OK ? result : removed;
}

static esp_err_t power_read_registers(i2c_master_dev_handle_t device,
                                     power_register_state_t *state, const char *phase)
{
    for (unsigned i = 0; i < POWER_REG_COUNT; ++i) {
        esp_err_t err = i2c_master_transmit_receive(device, &power_registers[i], 1,
                                                   &state->value[i], 1, 100);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Power readback %s reg=%02x failed: %s", phase,
                     (unsigned)power_registers[i], esp_err_to_name(err));
            return err;
        }
    }
    ESP_LOGI(TAG, "POWER_IO %s dir=%02x out=%02x highz=%02x pull_en=%02x pull_sel=%02x",
             phase, (unsigned)state->value[POWER_DIR], (unsigned)state->value[POWER_OUT],
             (unsigned)state->value[POWER_HIGHZ], (unsigned)state->value[POWER_PULL_EN],
             (unsigned)state->value[POWER_PULL_SEL]);
    return ESP_OK;
}

static esp_err_t power_check_cache(esp_io_expander_handle_t io,
                                   const power_register_state_t *physical)
{
    /* PI4IO getters use shadow registers. A prior out-of-band write/reset
     * must be detected BEFORE a read-modify-write could overwrite other rails.
     * These are the public driver callbacks, not a private layout cast. */
    esp_err_t (*readers[POWER_REG_COUNT])(esp_io_expander_handle_t, uint32_t *) = {
        io->read_direction_reg, io->read_output_reg, io->read_highz_reg,
        io->read_pullup_en_reg, io->read_pullup_sel_reg,
    };
    for (unsigned i = 0; i < POWER_REG_COUNT; ++i) {
        if (!readers[i]) return ESP_ERR_NOT_SUPPORTED;
        uint32_t cached = 0;
        esp_err_t err = readers[i](io, &cached);
        if (err != ESP_OK) return err;
        if ((uint8_t)cached != physical->value[i]) {
            ESP_LOGE(TAG, "Power cache mismatch reg=%02x cached=%02x physical=%02x; no pulse sent",
                     (unsigned)power_registers[i], (unsigned)(uint8_t)cached,
                     (unsigned)physical->value[i]);
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_OK;
}

static esp_err_t power_verify(i2c_master_dev_handle_t device,
                              const power_register_state_t *before, unsigned level,
                              const char *phase)
{
    power_register_state_t physical;
    ESP_RETURN_ON_ERROR(power_read_registers(device, &physical, phase), TAG, "power readback");
    const uint8_t others = (uint8_t)~POWER_OFF_PIN;
    for (unsigned i = 0; i < POWER_REG_COUNT; ++i) {
        if ((physical.value[i] & others) != (before->value[i] & others)) {
            ESP_LOGE(TAG, "Power readback changed another pin, reg=%02x before=%02x now=%02x",
                     (unsigned)power_registers[i], (unsigned)before->value[i], (unsigned)physical.value[i]);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (!(physical.value[POWER_DIR] & POWER_OFF_PIN) ||
        (physical.value[POWER_HIGHZ] & POWER_OFF_PIN) ||
        (physical.value[POWER_PULL_EN] & POWER_OFF_PIN) ||
        !!(physical.value[POWER_OUT] & POWER_OFF_PIN) != !!level) {
        ESP_LOGE(TAG, "Power pin readback does not match requested level=%u", level);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t tab5_power_off(void)
{
    esp_io_expander_handle_t io = bsp_io_expander1_init();
    if (!io) return ESP_FAIL;

    ESP_RETURN_ON_ERROR(tab5_rtc_release_irq(), TAG, "RTC IRQ must be released before power pulses");

    /* A second I2C handle is READ-ONLY and does not reset/reconfigure PI4IO.
     * All writes stay with the shared BSP driver so its cache stays coherent. */
    i2c_master_dev_handle_t device = NULL;
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x44, .scl_speed_hz = 400000,
    };
    esp_err_t result = i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &device);
    if (result != ESP_OK) return result;
    bool may_be_high = false;
    power_register_state_t before;
    result = power_read_registers(device, &before, "before");
    if (result != ESP_OK) goto finished;
    result = power_check_cache(io, &before);
    if (result != ESP_OK) goto finished;

    /* Disconnect pin 4 before configuring its latch; leave all other bits. */
    result = esp_io_expander_set_output_mode(io, POWER_OFF_PIN, IO_EXPANDER_OUTPUT_MODE_OPEN_DRAIN);
    if (result != ESP_OK) goto finished;
    result = esp_io_expander_set_pullupdown(io, POWER_OFF_PIN, IO_EXPANDER_PULL_NONE);
    if (result != ESP_OK) goto finished;
    result = esp_io_expander_set_dir(io, POWER_OFF_PIN, IO_EXPANDER_OUTPUT);
    if (result != ESP_OK) goto finished;
    may_be_high = true; /* Even a failed LOW transfer warrants cleanup. */
    result = esp_io_expander_set_level(io, POWER_OFF_PIN, 0);
    if (result != ESP_OK) goto finished;
    may_be_high = false;
    result = esp_io_expander_set_output_mode(io, POWER_OFF_PIN, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
    if (result != ESP_OK) goto finished;
    result = power_verify(device, &before, 0, "ready");
    if (result != ESP_OK) goto finished;

    ESP_LOGI(TAG, "Requesting hardware shutdown: PI4IO 0x44 pin4, UserDemo 3 x 100/100 ms");
    for (unsigned step = 0; step < 6; ++step) {
        const unsigned level = !(step & 1U);
        may_be_high = true;
        result = esp_io_expander_set_level(io, POWER_OFF_PIN, level);
        if (result != ESP_OK) goto finished;
        may_be_high = level != 0;
        result = power_verify(device, &before, level, level ? "pulse HIGH" : "pulse LOW");
        if (result != ESP_OK) goto finished;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    /* No more pulses, and no long HIGH that could look like another request.
     * A successful physical cutoff cannot execute the timeout path. */
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGE(TAG, "Hardware shutdown did not cut power after verified UserDemo pulses");
    result = ESP_ERR_TIMEOUT;

finished:
    if (may_be_high) {
        esp_err_t cleanup = esp_io_expander_set_level(io, POWER_OFF_PIN, 0);
        if (cleanup != ESP_OK) ESP_LOGE(TAG, "Power pin cleanup failed: %s", esp_err_to_name(cleanup));
    }
    const esp_err_t removed = i2c_master_bus_rm_device(device);
    if (removed != ESP_OK) ESP_LOGE(TAG, "Power readback handle cleanup failed: %s", esp_err_to_name(removed));
    return result; /* Preserve the first operational error, even if cleanup fails. */
}
