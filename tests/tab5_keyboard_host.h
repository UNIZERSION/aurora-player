#pragma once
#include "tab5_keyboard_protocol.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

typedef int esp_err_t;
typedef int BaseType_t;
typedef void *TaskHandle_t;
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef struct { tab5_keyboard_event_t values[64]; unsigned count,head; } test_queue;
typedef test_queue *QueueHandle_t;
enum { ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_STATE=-2,ESP_ERR_NO_MEM=-3,
    GPIO_NUM_0=0,GPIO_NUM_1=1,I2C_ADDR_BIT_LEN_7=7,I2C_CLK_SRC_DEFAULT=0,
    GPIO_MODE_INPUT=1,GPIO_PULLUP_ENABLE=1,GPIO_PULLDOWN_DISABLE=0,GPIO_INTR_NEGEDGE=2,
    pdTRUE=1,pdFALSE=0,pdPASS=1,BSP_I2C_NUM=1 };
typedef struct { int i2c_port,sda_io_num,scl_io_num,clk_source,glitch_ignore_cnt; struct {bool enable_internal_pullup;} flags; } i2c_master_bus_config_t;
typedef struct { int dev_addr_length,device_address,scl_speed_hz; } i2c_device_config_t;
typedef struct { uint64_t pin_bit_mask; int mode,pull_up_en,pull_down_en,intr_type; } gpio_config_t;
#define IRAM_ATTR
#define pdMS_TO_TICKS(x) (x)
#define portYIELD_FROM_ISR() ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t,const uint8_t*,size_t,uint8_t*,size_t,int);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t,const uint8_t*,size_t,int);
esp_err_t i2c_master_probe(i2c_master_bus_handle_t,int,int);
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t*,i2c_master_bus_handle_t*);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t,const i2c_device_config_t*,i2c_master_dev_handle_t*);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t);
esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t);
int64_t esp_timer_get_time(void);
QueueHandle_t xQueueCreate(unsigned,size_t);
void vQueueDelete(QueueHandle_t);
BaseType_t xQueueReset(QueueHandle_t);
BaseType_t xQueueSend(QueueHandle_t,const void*,int);
BaseType_t xQueueReceive(QueueHandle_t,void*,int);
void vTaskDelay(unsigned);
unsigned ulTaskNotifyTake(BaseType_t,unsigned);
void vTaskNotifyGiveFromISR(TaskHandle_t,BaseType_t*);
BaseType_t xTaskCreate(void(*)(void*),const char*,unsigned,void*,unsigned,TaskHandle_t*);
esp_err_t gpio_config(const gpio_config_t*);
esp_err_t gpio_install_isr_service(int);
esp_err_t gpio_isr_handler_add(int,void(*)(void*),void*);
