#pragma once
#include "FreeRTOS.h"
typedef struct host_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned count,unsigned size);
BaseType_t xQueueSend(QueueHandle_t queue,const void *item,TickType_t ticks);
BaseType_t xQueueReceive(QueueHandle_t queue,void *item,TickType_t ticks);
void vQueueDelete(QueueHandle_t queue);
unsigned uxQueueMessagesWaiting(QueueHandle_t queue);
