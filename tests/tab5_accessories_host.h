#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
typedef void *esp_io_expander_handle_t;
typedef void *SemaphoreHandle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE -2
#define ESP_ERR_TIMEOUT -3
#define ESP_ERR_NO_MEM -4
#define IO_EXPANDER_PIN_NUM_1 (1U<<1)
#define IO_EXPANDER_PIN_NUM_6 (1U<<6)
#define IO_EXPANDER_PIN_NUM_7 (1U<<7)
#define IO_EXPANDER_INPUT 0
#define IO_EXPANDER_OUTPUT 1
#define IO_EXPANDER_OUTPUT_MODE_PUSH_PULL 0
#define IO_EXPANDER_PULL_NONE 0
#define IO_EXPANDER_PULL_DOWN 2
#define pdMS_TO_TICKS(x) (x)
#define pdPASS 1
#define ESP_RETURN_ON_ERROR(expr,tag,...) do {int e_=(expr);(void)(tag);if(e_)return e_;}while(0)
static unsigned out[2],dir[2],mode[2],pull[2],input_level,calls,fail_at,task_count;
static bool fail_read,locked;
static void *bsp_io_expander_init(void){return (void*)0x43;}
static void *bsp_io_expander1_init(void){return (void*)0x44;}
static int record(void*h,unsigned mask){assert(h==(void*)0x43||h==(void*)0x44);assert(mask==(1U<<1)||mask==(1U<<6)||mask==(1U<<7));return ++calls==fail_at?ESP_FAIL:ESP_OK;}
static int index_of(void*h){return h==(void*)0x44;}
static esp_err_t esp_io_expander_set_level(void*h,unsigned mask,unsigned value){int e=record(h,mask);if(!e){if(value)out[index_of(h)]|=mask;else out[index_of(h)]&=~mask;}return e;}
static esp_err_t esp_io_expander_set_dir(void*h,unsigned mask,unsigned value){int e=record(h,mask);if(!e){if(value)dir[index_of(h)]|=mask;else dir[index_of(h)]&=~mask;}return e;}
static esp_err_t esp_io_expander_set_output_mode(void*h,unsigned mask,unsigned value){int e=record(h,mask);if(!e){if(value)mode[index_of(h)]|=mask;else mode[index_of(h)]&=~mask;}return e;}
static esp_err_t esp_io_expander_set_pullupdown(void*h,unsigned mask,unsigned value){int e=record(h,mask);assert(value==0);if(!e)pull[index_of(h)]&=~mask;return e;}
static esp_err_t esp_io_expander_get_level(void*h,unsigned mask,uint32_t*value){assert(h==(void*)0x43&&mask==(1U<<7));*value=input_level&mask;return fail_read?ESP_FAIL:ESP_OK;}
static SemaphoreHandle_t xSemaphoreCreateMutex(void){return (void*)1;}
static int xSemaphoreTake(void*h,unsigned ticks){assert(h&&(ticks==100||ticks==20)&&!locked);locked=true;return 1;}
static void xSemaphoreGive(void*h){assert(h&&locked);locked=false;}
static void vTaskDelay(unsigned ticks){assert(ticks==30);}
static int xTaskCreate(void(*f)(void*),const char*n,unsigned stack,void*a,unsigned priority,void*t){assert(f&&n&&stack>=2048&&!a&&priority==3&&!t);task_count++;return pdPASS;}
