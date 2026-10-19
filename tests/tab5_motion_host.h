#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
typedef int esp_err_t;
typedef unsigned TickType_t;
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef struct { int dev_addr_length,device_address,scl_speed_hz; } i2c_device_config_t;
enum { ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_STATE=-2, ESP_ERR_INVALID_ARG=-3,
    ESP_ERR_NOT_FOUND=-4, ESP_ERR_TIMEOUT=-5, ESP_ERR_INVALID_RESPONSE=-6, I2C_ADDR_BIT_LEN_7=7 };
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)0)
esp_err_t bsp_i2c_init(void);
i2c_master_bus_handle_t bsp_i2c_get_handle(void);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t,const i2c_device_config_t*,i2c_master_dev_handle_t*);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t,const uint8_t*,size_t,uint8_t*,size_t,int);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t,const uint8_t*,size_t,int);
void esp_rom_delay_us(unsigned);
void vTaskDelay(TickType_t);
int64_t esp_timer_get_time(void);
#define TAB5_MOTION_RATE_HZ 400
#define TAB5_MOTION_LSB_PER_G 4096
typedef struct { int16_t x, y, z; } tab5_motion_sample_t;
