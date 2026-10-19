#pragma once
#include "FreeRTOS.h"
typedef struct host_art_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned count,unsigned size);
BaseType_t xQueueSend(QueueHandle_t queue,const void *item,TickType_t ticks);
BaseType_t xQueueReceive(QueueHandle_t queue,void *item,TickType_t ticks);
BaseType_t xQueuePeek(QueueHandle_t queue,void *item,TickType_t ticks);
BaseType_t xQueueOverwrite(QueueHandle_t queue,const void *item);
void vQueueDelete(QueueHandle_t queue);
