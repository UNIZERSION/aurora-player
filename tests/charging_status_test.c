#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
typedef void *i2c_master_dev_handle_t;
typedef void *esp_io_expander_handle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG -2
#define ESP_ERR_INVALID_STATE -3
#define ESP_ERR_TIMEOUT -4
#define ESP_RETURN_ON_ERROR(expr,tag,...) do { int result_=(expr); (void)(tag); if(result_) return result_; } while(0)
static const char *TAG __attribute__((unused))="host battery";
static i2c_master_dev_handle_t battery=(void*)0x41;
static bool battery_ready=true;
static uint16_t voltage_raw=6160;
static int16_t shunt_raw=1000;
static int voltage_error,current_error,pin_error;
static bool pin_high=true;
static unsigned voltage_reads,current_reads,pin_reads;
static int64_t now_us;
static inline __attribute__((unused)) int64_t esp_timer_get_time(void){return now_us;}
static int reg_read(i2c_master_dev_handle_t device,uint8_t reg,uint8_t *data,size_t size){
 assert(device==battery&&size==2);uint16_t raw;
 if(reg==0x02){++voltage_reads;if(voltage_error)return voltage_error;raw=voltage_raw;}
 else {assert(reg==0x01);++current_reads;if(current_error)return current_error;raw=(uint16_t)shunt_raw;}
 data[0]=(uint8_t)(raw>>8);data[1]=(uint8_t)raw;return ESP_OK;
}
static inline __attribute__((unused)) esp_io_expander_handle_t bsp_io_expander1_init(void){return (void*)0x44;}
static inline __attribute__((unused)) int esp_io_expander_get_level(esp_io_expander_handle_t device,uint32_t mask,uint32_t *value){
 assert(device==(void*)0x44&&mask==(1U<<6));++pin_reads;if(pin_error)return pin_error;
 *value=pin_high?mask:0;return ESP_OK;
}

#include "battery_production.h"

static bool disconnect_regression(void){
 tab5_battery_status_t status;
 assert(tab5_battery_status_read(&status)==ESP_OK);
 assert(status.millivolts==7700&&status.milliamps==-500&&status.present);
 printf("Disconnect: raw=+1000, pack=-500mA, voltage=7700mV, CHG_STAT=HIGH; charging=%d (expected 0), known=%d\n",status.charging,status.charging_known);
 return !status.charging;
}

#ifndef EXPECT_OLD_REGRESSION
static unsigned samples;
static tab5_battery_status_t read_status(void){
 tab5_battery_status_t status;memset(&status,0xA5,sizeof status);
 assert(tab5_battery_status_read(&status)==ESP_OK);assert(pin_reads==0);++samples;return status;
}
static tab5_battery_status_t sample(int current,int64_t time,bool expected,bool known){
 shunt_raw=(int16_t)(-current*2);now_us=time;tab5_battery_status_t status=read_status();
 if(status.charging!=expected||status.charging_known!=known){
  fprintf(stderr,"FAIL sample %u current=%d time=%lld charging=%d/%d known=%d/%d\n",samples,current,(long long)time,status.charging,expected,status.charging_known,known);
  assert(!"Charging state mismatch");
 }
 if(known&&!status.sample_age_ms)assert(status.milliamps==current&&status.present);
 return status;
}
static void reset(void){
 voltage_error=current_error=pin_error=0;pin_high=true;now_us=0;voltage_raw=6160;shunt_raw=1000;
 battery_ready=false;tab5_battery_status_t status;memset(&status,0xA5,sizeof status);
 assert(tab5_battery_status_read(&status)==ESP_ERR_INVALID_STATE);
 assert(!status.charging&&!status.charging_known&&!status.present&&status.estimated_percent==-1&&status.raw_estimated_percent==-1);
 battery_ready=true;pin_reads=voltage_reads=current_reads=0;
}
static void confirmed_charge(int64_t start){
 sample(100,start,false,true);sample(100,start+250000,false,true);
 sample(100,start+500000,false,true);sample(100,start+750000,true,true);
}
static void timing_and_hysteresis(void){
 reset();sample(20,0,false,true);sample(20,250000,false,true);sample(20,500000,false,true);
 sample(20,749999,false,true);sample(20,750000,true,true);
 sample(19,750001,true,true);sample(6,750002,true,true);sample(5,750003,true,true);
 for(int i=1;i<12;++i)sample(5,750003+i*250000,true,true);
 sample(5,3750002,true,true);sample(5,3750003,false,true);
 reset();confirmed_charge(0);sample(-20,1000000,true,true);
 sample(-19,1250000,true,true); // near-zero must not count as strong discharge.
 for(int i=0;i<4;++i)sample(-20,1500000+i*250000,true,true);
 sample(-20,2499999,true,true);sample(-20,2500000,false,true);
 sample(20,2750000,false,true);sample(19,3000000,false,true);
 sample(20,3250000,false,true);sample(20,3500000,false,true);
 sample(20,3750000,false,true);sample(20,4000000,true,true);
 reset();for(int current=-19;current<=19;++current)sample(current,(current+19)*250000LL,false,true);
 reset();int64_t large=INT64_C(1)<<48;confirmed_charge(large);
 for(int i=0;i<4;++i)sample(-202,large+1000000+i*250000,true,true);
 sample(-202,large+2000000,false,true);
 puts("PASS: current sign/thresholds, 750ms ON, 1s persistent discharge OFF, 3s zero-current OFF; interrupted confirmations and long uptime.");
}
static void load_pulses_and_stalls(void){
 reset();confirmed_charge(0);int64_t time=750000;
 // Six minutes of zero/negative pulses up to 750ms, interleaved with charging.
 for(unsigned cycle=0;cycle<480;++cycle){
  unsigned count=1+cycle%3;
  for(unsigned j=0;j<count;++j){time+=250000;sample(cycle%2?-202:0,time,true,true);}
  time+=250000;sample(100,time,true,true);
 }
 // Repeated 900ms UI stalls used to reset the charging flag immediately.
 for(unsigned i=0;i<20;++i){time+=900000;sample(100,time,true,true);}
 // 1.5s gap retains confirmed state, but must restart pending timers.
 time+=1500000;sample(-202,time,true,true);
 for(unsigned i=0;i<3;++i){time+=250000;sample(-202,time,true,true);}
 time+=250000;sample(-202,time,false,true);
 reset();sample(100,0,false,true);sample(100,1500000,false,true);
 sample(100,1750000,false,true);sample(100,2000000,false,true);sample(100,2250000,true,true);
 sample(100,4250001,false,true); // >2s old state expires.
 sample(100,4500001,false,true);sample(100,4750001,false,true);sample(100,5000001,true,true);
 sample(100,10,false,true);sample(100,250010,false,true);sample(100,500010,false,true);sample(100,750010,true,true);
 sample(100,-1,false,true);sample(100,0,false,true);
 for(unsigned i=0;i<64;++i)sample(100,0,false,true);
 sample(100,250000,false,true);sample(100,500000,false,true);sample(100,750000,true,true);
 sample(-202,1000000,true,true);
 for(unsigned i=0;i<64;++i)sample(-202,1000000,true,true);
 for(unsigned i=0;i<3;++i)sample(-202,1250000+i*250000,true,true);
 sample(-202,2000000,false,true);
 assert(voltage_reads==current_reads);
 puts("PASS: six-minute load-pulse simulation, 900ms/1.5s scheduling gaps without blinking, gap expiry, clock rollback and duplicate timestamp protection.");
}
static void transient_faults(void){
 reset();confirmed_charge(0);voltage_error=ESP_ERR_TIMEOUT;
 tab5_battery_status_t status=sample(100,1000000,true,true);
 assert(status.sample_age_ms==250&&status.millivolts==7700&&status.milliamps==100);
 status=sample(100,1750000,true,true);assert(status.sample_age_ms==1000);
 now_us=1750001;assert(tab5_battery_status_read(&status)==ESP_ERR_TIMEOUT);
 assert(!status.charging&&!status.charging_known&&!status.present&&status.estimated_percent==-1);
 voltage_error=0;confirmed_charge(2000000);
 current_error=ESP_ERR_TIMEOUT;status=sample(100,3000000,true,true);assert(status.sample_age_ms==250);
 status=sample(100,3750000,true,true);assert(status.sample_age_ms==1000);
 status=sample(100,3750001,false,false);assert(status.present&&status.estimated_percent>=0);
 current_error=0;confirmed_charge(4000000);
 voltage_raw=0;status=sample(100,5000000,false,false);assert(!status.present&&status.estimated_percent==-1);
 voltage_raw=6160;confirmed_charge(5250000);
 battery_ready=false;assert(tab5_battery_status_read(&status)==ESP_ERR_INVALID_STATE);
 assert(!status.charging&&!status.charging_known&&!status.present);
 // I/O failures cannot borrow time from an unconfirmed ON sequence.
 reset();sample(100,0,false,true);current_error=ESP_FAIL;
 status=sample(100,250000,false,true);assert(status.sample_age_ms==250);
 current_error=0;confirmed_charge(500000);
 assert(tab5_battery_status_read(NULL)==ESP_ERR_INVALID_ARG);
 puts("PASS: one-second timestamped last-sample retention during read faults, unavailable after timeout, no fabricated currents, missing-pack reset and recovery confirmation.");
}
static void voltage_filter(void){
 reset();voltage_raw=6080;tab5_battery_status_t status=sample(-202,0,false,true);
 assert(status.millivolts==7600&&status.raw_estimated_percent==62&&status.estimated_percent==62);
 int minimum=100,maximum=0,raw_min=100,raw_max=0;
 // 5 minutes: normal voltage ripple plus one large load spike every 7 polls.
 for(int i=1;i<=1200;++i){
  voltage_raw=(uint16_t)(i%7==0?5800:6080+(i%5-2)*4);
  status=sample(-202,i*250000LL,false,true);
  if(status.estimated_percent<minimum)minimum=status.estimated_percent;
  if(status.estimated_percent>maximum)maximum=status.estimated_percent;
  if(status.raw_estimated_percent<raw_min)raw_min=status.raw_estimated_percent;
  if(status.raw_estimated_percent>raw_max)raw_max=status.raw_estimated_percent;
  assert(status.sample_age_ms==0&&status.millivolts==(uint16_t)(voltage_raw*5/4));
 }
 assert(maximum-minimum<=1&&raw_max-raw_min>=20);
 printf("PASS: voltage ripple/load spikes: raw=%d..%d%%; displayed=%d..%d%%; raw mV/mA preserved.\n",raw_min,raw_max,minimum,maximum);
 // Persistent voltage change must converge smoothly; cannot freeze forever.
 voltage_raw=5760;int previous=status.estimated_percent;int64_t last_change=0;
 for(int i=1201;i<=1800;++i){
  int64_t time=i*250000LL;status=sample(-202,time,false,true);
  assert(status.estimated_percent<=previous&&previous-status.estimated_percent<=1);
  if(status.estimated_percent!=previous){assert(!last_change||time-last_change>=5000000);last_change=time;}
  previous=status.estimated_percent;
 }
 assert(status.raw_estimated_percent==37&&status.estimated_percent>=37&&status.estimated_percent<=39);
 printf("PASS: sustained 7600->7200mV change converged to %d%% (raw %d%%), <=1 point/5s, no frame-sized jump.\n",status.estimated_percent,status.raw_estimated_percent);
 // Charging cannot cause an immediate voltage-driven 20-point jump either.
 voltage_raw=6240;int64_t start=450250000;previous=status.estimated_percent;last_change=0;
 for(int i=0;i<2000;++i){
  int64_t time=start+i*250000LL;shunt_raw=-200;now_us=time;status=read_status();
  assert(status.estimated_percent>=previous&&status.estimated_percent-previous<=1);
  if(status.estimated_percent!=previous){assert(!last_change||time-last_change>=10000000);last_change=time;}
  previous=status.estimated_percent;
 }
 assert(status.estimated_percent>=73&&status.estimated_percent<=75&&status.raw_estimated_percent==75);
 puts("PASS: charging rise converges without jumping; <=1 point/10s; filtered value remains explicitly a voltage estimate.");
 // Missing battery resets the estimator, unlike a short communication fault.
 voltage_raw=0;sample(-202,1000000000,false,false);
 voltage_raw=6560;status=sample(0,1000250000,false,true);assert(status.estimated_percent==100);
 reset();voltage_raw=5280;status=sample(0,0,false,true);assert(status.estimated_percent==0);
 reset();voltage_raw=6720;status=sample(0,0,false,true);assert(status.estimated_percent==100&&status.millivolts==8400);
}
static void raw_bounds(void){
 reset();shunt_raw=INT16_MIN;now_us=0;tab5_battery_status_t status=read_status();
 assert(status.milliamps==16384&&!status.charging&&status.charging_known);
 now_us=250000;read_status();now_us=500000;read_status();now_us=750000;status=read_status();assert(status.charging);
 shunt_raw=INT16_MAX;
 for(int i=0;i<4;++i){now_us=1000000+i*250000;status=read_status();assert(status.milliamps==-16383&&status.charging);}
 now_us=2000000;status=read_status();assert(!status.charging);
 for(int raw=-1;raw<=1;++raw){shunt_raw=(int16_t)raw;status=read_status();assert(status.milliamps==0&&!status.charging);}
 reset();voltage_raw=4000;status=sample(0,0,false,true);assert(status.millivolts==5000&&status.estimated_percent==0);
 reset();voltage_raw=7200;status=sample(0,0,false,true);assert(status.millivolts==9000&&status.estimated_percent==100);
 const uint16_t absent[]={0,3999,7201,65535};
 for(unsigned i=0;i<sizeof absent/sizeof absent[0];++i){
  voltage_raw=absent[i];unsigned before=current_reads;status=sample(0,250000+i,false,false);
  assert(!status.present&&status.estimated_percent==-1&&status.raw_estimated_percent==-1&&current_reads==before);
 }
 assert(pin_reads==0);
 puts("PASS: signed register extrema, half-mA noise, valid voltage bounds, absent pack and no CHG_STAT fallback.");
}
#endif

int main(void){
 if(!disconnect_regression())return 1;
#ifndef EXPECT_OLD_REGRESSION
 timing_and_hysteresis();load_pulses_and_stalls();transient_faults();voltage_filter();raw_bounds();
 printf("PASS: %u actual-production sensor/time samples. No hardware accessed.\n",samples);
#endif
 return 0;
}
