/* Win32-only test shim. This validates task/queue control flow, never timing
 * guarantees or the hardware I2S/SD drivers. Not compiled into firmware. */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
struct host_queue {CRITICAL_SECTION lock;CONDITION_VARIABLE changed;unsigned capacity,size,head,count;unsigned char *data;};
static _Thread_local int is_decoder;
static volatile LONG decoder_delay_ms;
void host_audio_delay_next_decode(unsigned ms){InterlockedExchange(&decoder_delay_ms,(LONG)ms);}
QueueHandle_t xQueueCreate(unsigned count,unsigned size)
{
    QueueHandle_t q=calloc(1,sizeof(*q));if(!q)return NULL;
    q->data=malloc(count*size);if(!q->data){free(q);return NULL;}
    q->capacity=count;q->size=size;InitializeCriticalSection(&q->lock);InitializeConditionVariable(&q->changed);return q;
}
static BaseType_t transfer(QueueHandle_t q,void *item,TickType_t ticks,int send)
{
    EnterCriticalSection(&q->lock);ULONGLONG start=GetTickCount64();
    while(send?q->count==q->capacity:q->count==0) {
        DWORD left=ticks==portMAX_DELAY?INFINITE:(DWORD)(GetTickCount64()-start>=ticks?0:ticks-(GetTickCount64()-start));
        if(!left || !SleepConditionVariableCS(&q->changed,&q->lock,left)){LeaveCriticalSection(&q->lock);return pdFALSE;}
    }
    if(send){memcpy(q->data+((q->head+q->count)%q->capacity)*q->size,item,q->size);q->count++;}
    else {memcpy(item,q->data+q->head*q->size,q->size);q->head=(q->head+1)%q->capacity;q->count--;}
    WakeAllConditionVariable(&q->changed);LeaveCriticalSection(&q->lock);return pdTRUE;
}
BaseType_t xQueueSend(QueueHandle_t q,const void *item,TickType_t ticks){return transfer(q,(void*)item,ticks,1);}
BaseType_t xQueueReceive(QueueHandle_t q,void *item,TickType_t ticks)
{
    BaseType_t result=transfer(q,item,ticks,0);
    if(result && is_decoder && q->size==sizeof(void*)) {
        LONG delay=InterlockedExchange(&decoder_delay_ms,0);if(delay>0)Sleep((DWORD)delay);
    }
    return result;
}
unsigned uxQueueMessagesWaiting(QueueHandle_t q)
{
    EnterCriticalSection(&q->lock);unsigned count=q->count;LeaveCriticalSection(&q->lock);return count;
}
void vQueueDelete(QueueHandle_t q){DeleteCriticalSection(&q->lock);free(q->data);free(q);}
SemaphoreHandle_t xSemaphoreCreateMutex(void){return CreateMutex(NULL,FALSE,NULL);}
BaseType_t xSemaphoreTake(SemaphoreHandle_t m,TickType_t ticks){return WaitForSingleObject(m,ticks==portMAX_DELAY?INFINITE:ticks)==WAIT_OBJECT_0;}
BaseType_t xSemaphoreGive(SemaphoreHandle_t m){return ReleaseMutex(m)!=0;}
void vSemaphoreDelete(SemaphoreHandle_t m){CloseHandle(m);}
struct task_start {void(*fn)(void*);void *arg;int decoder;};
static DWORD WINAPI task_entry(void *p){struct task_start start=*(struct task_start*)p;free(p);is_decoder=start.decoder;start.fn(start.arg);return 0;}
BaseType_t xTaskCreatePinnedToCore(void(*fn)(void*),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *task,int core)
{
    (void)stack;(void)priority;(void)core;
    struct task_start *start=malloc(sizeof(*start));if(!start)return pdFALSE;*start=(struct task_start){fn,arg,!strcmp(name,"audio_decode")};
    HANDLE handle=CreateThread(NULL,0,task_entry,start,0,NULL);if(!handle){free(start);return pdFALSE;}
    if(task)*task=handle;else CloseHandle(handle);return pdPASS;
}
void vTaskDelay(TickType_t ticks){Sleep(ticks);}
void vTaskDelete(TaskHandle_t task){TerminateThread(task,0);CloseHandle(task);}
int64_t esp_timer_get_time(void){LARGE_INTEGER counter,frequency;QueryPerformanceCounter(&counter);QueryPerformanceFrequency(&frequency);return counter.QuadPart*1000000/frequency.QuadPart;}
