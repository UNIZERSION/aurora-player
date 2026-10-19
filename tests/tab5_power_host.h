#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
typedef int esp_err_t;
typedef struct io_model *esp_io_expander_handle_t;
struct io_model {
 esp_err_t (*read_direction_reg)(esp_io_expander_handle_t,uint32_t*);
 esp_err_t (*read_output_reg)(esp_io_expander_handle_t,uint32_t*);
 esp_err_t (*read_highz_reg)(esp_io_expander_handle_t,uint32_t*);
 esp_err_t (*read_pullup_en_reg)(esp_io_expander_handle_t,uint32_t*);
 esp_err_t (*read_pullup_sel_reg)(esp_io_expander_handle_t,uint32_t*);
};
typedef void *i2c_master_dev_handle_t;
typedef struct {int dev_addr_length,device_address,scl_speed_hz;} i2c_device_config_t;
#define I2C_ADDR_BIT_LEN_7 7
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_TIMEOUT -2
#define ESP_ERR_INVALID_STATE -3
#define ESP_ERR_INVALID_RESPONSE -4
#define ESP_ERR_NOT_SUPPORTED -5
#define IO_EXPANDER_PIN_NUM_4 (1U << 4)
#define IO_EXPANDER_OUTPUT 1
#define IO_EXPANDER_PULL_NONE 0
#define IO_EXPANDER_OUTPUT_MODE_OPEN_DRAIN 1
#define IO_EXPANDER_OUTPUT_MODE_PUSH_PULL 0
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_RETURN_ON_ERROR(expr,tag,...) do { int result_=(expr); (void)(tag); if(result_) return result_; } while(0)
static void mock_log(const char *tag,const char *format,...){(void)tag;(void)format;}
#define ESP_LOGI(tag,...) mock_log(tag,__VA_ARGS__)
#define ESP_LOGE(tag,...) mock_log(tag,__VA_ARGS__)
static const char *esp_err_to_name(int err){(void)err;return "error";}

static unsigned calls,fail_at,fail_also_at,elapsed,highs,delays,mutations,ignore_write_at,corrupt_read_at,removed,cut_after_delay;
static uint8_t physical_regs[5],cached_regs[5];
static uint8_t rtc_regs[0x31],rtc_raise_before_clear;
static unsigned rtc_removed;
static bool missing_device,readback_active,rtc_active;
static jmp_buf powered_off;
typedef struct {char kind;unsigned value,time;} operation_t;
static operation_t operations[100];
static int record(char kind,unsigned value){
 assert(calls<100);operations[calls]=(operation_t){kind,value,elapsed};++calls;
 return calls==fail_at||calls==fail_also_at?-1000-(int)calls:ESP_OK;
}
static int cached_read(unsigned index,uint32_t *value){int err=record('c',index);if(err)return err;*value=cached_regs[index];return ESP_OK;}
static int cached_dir(esp_io_expander_handle_t io,uint32_t*v){(void)io;return cached_read(0,v);}
static int cached_out(esp_io_expander_handle_t io,uint32_t*v){(void)io;return cached_read(1,v);}
static int cached_highz(esp_io_expander_handle_t io,uint32_t*v){(void)io;return cached_read(2,v);}
static int cached_pull(esp_io_expander_handle_t io,uint32_t*v){(void)io;return cached_read(3,v);}
static int cached_sel(esp_io_expander_handle_t io,uint32_t*v){(void)io;return cached_read(4,v);}
static struct io_model io_model={cached_dir,cached_out,cached_highz,cached_pull,cached_sel};
static esp_io_expander_handle_t bsp_io_expander1_init(void){return missing_device?NULL:&io_model;}
static void *bsp_i2c_get_handle(void){return (void*)0x123;}
static int i2c_master_bus_add_device(void*bus,const i2c_device_config_t*cfg,i2c_master_dev_handle_t*out){
 assert(bus==(void*)0x123&&(cfg->device_address==0x44||cfg->device_address==0x32)&&cfg->scl_speed_hz==400000&&cfg->dev_addr_length==7);
 int err=record('a',cfg->device_address);if(err)return err;
 if(cfg->device_address==0x32){*out=(void*)0x332;rtc_active=true;}else{*out=(void*)0x444;readback_active=true;}return ESP_OK;
}
static int i2c_master_bus_rm_device(i2c_master_dev_handle_t device){
 if(device==(void*)0x332){assert(rtc_active);++rtc_removed;rtc_active=false;return record('x',0x32);}
 assert(device==(void*)0x444&&readback_active);++removed;readback_active=false;return record('x',0x44);
}
static int i2c_master_transmit_receive(i2c_master_dev_handle_t device,const uint8_t*reg,size_t reg_size,uint8_t*out,size_t size,int timeout){
 if(device==(void*)0x332){
  assert(rtc_active&&reg_size==1&&*reg==0x1c&&size==4&&timeout==100);
  int err=record('r',*reg);if(err)return err;memcpy(out,rtc_regs+*reg,size);if(corrupt_read_at==calls)out[2]^=8;return ESP_OK;
 }
 assert(device==(void*)0x444&&readback_active&&reg_size==1&&size==1&&timeout==100);
 const uint8_t addresses[]={3,5,7,11,13};unsigned index=0;while(index<5&&addresses[index]!=*reg)++index;assert(index<5);
 int err=record('r',*reg);if(err)return err;*out=physical_regs[index];if(corrupt_read_at==calls)*out^=1;return ESP_OK;
}
static int i2c_master_transmit(i2c_master_dev_handle_t device,const uint8_t*data,size_t size,int timeout){
 assert(device==(void*)0x332&&rtc_active&&size==2&&timeout==100&&(data[0]==0x1d||data[0]==0x1e));
 int err=record('w',((unsigned)data[0]<<8)|data[1]);if(err)return err;
 if(data[0]==0x1d)rtc_regs[0x1d]|=rtc_raise_before_clear;
 if(ignore_write_at!=calls){
  if(data[0]==0x1d)rtc_regs[0x1d]=(uint8_t)(rtc_regs[0x1d]&(data[1]|1));
  else rtc_regs[data[0]]=data[1];
 }
 return ESP_OK;
}
static int mutate(esp_io_expander_handle_t io,unsigned mask,char kind,unsigned value,unsigned index,unsigned bits){
 assert(io==&io_model&&mask==IO_EXPANDER_PIN_NUM_4);
 int err=record(kind,value);++mutations;if(err)return err;
 cached_regs[index]=(uint8_t)((cached_regs[index]&~mask)|(bits&mask));
 if(ignore_write_at!=calls)physical_regs[index]=cached_regs[index];
 return ESP_OK;
}
static esp_err_t esp_io_expander_set_output_mode(esp_io_expander_handle_t io,unsigned mask,unsigned mode){return mutate(io,mask,'m',mode,2,mode?mask:0);}
static esp_err_t esp_io_expander_set_pullupdown(esp_io_expander_handle_t io,unsigned mask,unsigned state){
 int err=mutate(io,mask,'p',state,3,0);if(!err){cached_regs[4]&=(uint8_t)~mask;if(ignore_write_at!=calls)physical_regs[4]=cached_regs[4];}return err;
}
static esp_err_t esp_io_expander_set_dir(esp_io_expander_handle_t io,unsigned mask,unsigned dir){assert(cached_regs[2]&mask);return mutate(io,mask,'d',dir,0,mask);}
static esp_err_t esp_io_expander_set_level(esp_io_expander_handle_t io,unsigned mask,unsigned level){assert(cached_regs[0]&mask);int err=mutate(io,mask,'l',level,1,level?mask:0);if(!err&&level)++highs;return err;}
static void vTaskDelay(unsigned ticks){
 assert(ticks==100||ticks==2000);
 if(ticks==2000)assert(!(physical_regs[1]&IO_EXPANDER_PIN_NUM_4)&&!(physical_regs[2]&IO_EXPANDER_PIN_NUM_4));
 elapsed+=ticks;++delays;if(cut_after_delay==delays)longjmp(powered_off,1);
}

