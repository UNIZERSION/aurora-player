#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
BaseType_t xTaskCreatePinnedToCore(void(*fn)(void*),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *task,int core);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t task);
