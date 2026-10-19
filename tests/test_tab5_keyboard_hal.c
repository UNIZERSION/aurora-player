#define TAB5_KEYBOARD_HOST_TEST
#include "../components/tab5_hal/tab5_keyboard.c"
#include <stdio.h>
#include <setjmp.h>

static test_queue queue;
static unsigned clock_ms, writes, probes, last_probe, mode_reg, pending, cursor;
static uint8_t reports[40][2];
static bool present, fail_read, scenario;
static jmp_buf stop_worker;
static bool is_present(void) { return scenario ? clock_ms>=500 && clock_ms<2100 : present; }
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t d,const uint8_t *reg,size_t n,uint8_t *out,size_t len,int timeout)
{
    assert(d&&n==1&&timeout<=50);
    if(*reg==0xfe){ /* The VERSION read is the connection attempt (no i2c_master_probe). */
        if(scenario && probes) assert(clock_ms-last_probe>=2000);
        last_probe=clock_ms;++probes;
    }
    if(!is_present()||fail_read)return ESP_FAIL;
    if(*reg==0xfe){ assert(len==2);out[0]=1;out[1]=0x6d; }
    else if(*reg==0x10){assert(len==1);*out=(uint8_t)mode_reg;}
    else if(*reg==0x02){assert(len==1);*out=(uint8_t)(pending-cursor);}
    else if(*reg==0x30){assert(len==2);if(cursor<pending){memcpy(out,reports[cursor++],2);}else{out[0]=out[1]=0xff;}}
    else assert(!"Unexpected register read");
    return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t d,const uint8_t *bytes,size_t n,int timeout)
{
    assert(d&&n==2&&timeout<=50);if(!is_present())return ESP_FAIL;++writes;
    if(bytes[0]==0x10){assert(bytes[1]==1);mode_reg=1;}
    else if(bytes[0]==0){assert(bytes[1]==2);}
    else if(bytes[0]==2){assert(bytes[1]==0);pending=cursor=0;}
    else if(bytes[0]==1){assert(bytes[1]==0);}
    else assert(!"Must never write address, firmware/IAP or brightness");
    return ESP_OK;
}
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *c,i2c_master_bus_handle_t *b)
{assert(c->i2c_port==0&&c->sda_io_num==0&&c->scl_io_num==1);*b=(void*)1;return ESP_OK;}
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t b,const i2c_device_config_t*c,i2c_master_dev_handle_t*d)
{assert(b&&c->device_address==0x6d&&c->scl_speed_hz==400000);*d=(void*)2;return ESP_OK;}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t d){(void)d;return ESP_OK;}
esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t b){(void)b;return ESP_OK;}
int64_t esp_timer_get_time(void){return (int64_t)clock_ms*1000;}
QueueHandle_t xQueueCreate(unsigned n,size_t size){assert(n==64&&size==sizeof(tab5_keyboard_event_t));memset(&queue,0,sizeof(queue));return &queue;}
void vQueueDelete(QueueHandle_t q){(void)q;}
BaseType_t xQueueReset(QueueHandle_t q){q->count=q->head=0;return pdTRUE;}
BaseType_t xQueueSend(QueueHandle_t q,const void *p,int timeout){assert(timeout==0);if(q->count==64)return pdFALSE;q->values[(q->head+q->count++)%64]=*(const tab5_keyboard_event_t*)p;return pdTRUE;}
BaseType_t xQueueReceive(QueueHandle_t q,void*p,int timeout){assert(timeout==0);if(!q->count)return pdFALSE;*(tab5_keyboard_event_t*)p=q->values[q->head++%64];--q->count;return pdTRUE;}
void vTaskDelay(unsigned ms){clock_ms+=ms;if(scenario&&clock_ms>=2200)longjmp(stop_worker,1);}
unsigned ulTaskNotifyTake(BaseType_t clear,unsigned ms){assert(clear==pdTRUE);clock_ms+=ms;return 0;}
void vTaskNotifyGiveFromISR(TaskHandle_t task,BaseType_t*wake){assert(task);*wake=pdFALSE;}
BaseType_t xTaskCreate(void(*fn)(void*),const char*n,unsigned stack,void*a,unsigned priority,TaskHandle_t*out)
{(void)fn;(void)n;(void)a;assert(stack<=4096&&priority<=2);*out=(void*)3;return pdPASS;}
esp_err_t gpio_config(const gpio_config_t*c){assert(c->pin_bit_mask==(1ULL<<50)&&c->mode==GPIO_MODE_INPUT);return ESP_OK;}
esp_err_t gpio_install_isr_service(int flags){assert(flags==0);return ESP_ERR_INVALID_STATE;}
esp_err_t gpio_isr_handler_add(int pin,void(*fn)(void*),void*a){(void)fn;(void)a;assert(pin==50);return ESP_OK;}

int main(void)
{
    assert(tab5_keyboard_init()==ESP_OK&&tab5_keyboard_init()==ESP_OK);
    assert(!connect_keyboard()&&!tab5_keyboard_connected());
    present=true;assert(connect_keyboard()&&tab5_keyboard_connected()&&writes==4);
    reports[0][0]=0;reports[0][1]=4;reports[1][0]=0;reports[1][1]=0;pending=2;
    assert(drain());tab5_keyboard_event_t event;
    assert(tab5_keyboard_read_event(&event)&&event.key=='a'&&!event.repeat);
    assert(!tab5_keyboard_read_event(&event)&&!protocol.repeating);
    reports[0][0]=2;reports[0][1]=5;pending=1;cursor=0;assert(drain());
    fail_read=true;assert(!drain());disconnect();
    assert(!tab5_keyboard_connected()&&!tab5_keyboard_read_event(&event)&&queue.count==0&&!protocol.repeating);
    fail_read=false;assert(connect_keyboard());
    event=(tab5_keyboard_event_t){.key='x'};
    for(unsigned i=0;i<64;++i)assert(enqueue(&event));
    assert(!enqueue(&event)&&queue.count==0&&!protocol.repeating);
    assert(enqueue(&event)&&tab5_keyboard_read_event(&event)&&event.key=='x');
    pending=33;cursor=0;assert(!drain());disconnect();
    pending=cursor=0;clock_ms=probes=last_probe=0;scenario=true;
    if(!setjmp(stop_worker))keyboard_worker(NULL);
    assert(probes==2&&!tab5_keyboard_connected()&&queue.count==0&&!protocol.repeating);
    puts("PASS: actual A164 HAL independent pins/bus, safe volatile registers, nonblocking queue, overflow, I2C error reset, absent/reconnect timing and unplug");
    return 0;
}
