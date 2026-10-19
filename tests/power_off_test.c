#undef NDEBUG /* zig -O1/-O2 predefines NDEBUG; keep assert() checks active */
#define TAB5_POWER_HOST_TEST
#include "../components/tab5_hal/tab5_power.c"

static const uint8_t initial_regs[5]={0xa1,0xb9,0x46,0x7b,0x55};
static void reset_model(void){
 calls=fail_at=fail_also_at=elapsed=highs=delays=mutations=ignore_write_at=corrupt_read_at=removed=cut_after_delay=0;
 rtc_removed=rtc_raise_before_clear=0;missing_device=readback_active=rtc_active=false;
 memcpy(physical_regs,initial_regs,5);memcpy(cached_regs,initial_regs,5);
 for(unsigned i=0;i<sizeof(rtc_regs);i++)rtc_regs[i]=(uint8_t)(i*3);
 rtc_regs[0x1c]=0x93;rtc_regs[0x1d]=0xbf;rtc_regs[0x1e]=0x7f;rtc_regs[0x1f]=0x35;
 io_model=(struct io_model){cached_dir,cached_out,cached_highz,cached_pull,cached_sel};
}
static void check_other_pins(void){
 for(unsigned i=0;i<5;i++)assert((physical_regs[i]&(uint8_t)~IO_EXPANDER_PIN_NUM_4)==(initial_regs[i]&(uint8_t)~IO_EXPANDER_PIN_NUM_4));
}
static void check_rtc_preserved(const uint8_t *before){
 for(unsigned i=0;i<sizeof(rtc_regs);i++)if(i!=0x1d&&i!=0x1e)assert(rtc_regs[i]==before[i]);
 assert(rtc_regs[0x1e]==(uint8_t)(before[0x1e]&~0x38));
 assert(rtc_regs[0x1d]==(uint8_t)(before[0x1d]&0x87));
}
static void test_rtc_release(void){
 for(unsigned control=0;control<256;control++){
  reset_model();rtc_regs[0x1e]=(uint8_t)control;uint8_t before[sizeof(rtc_regs)];memcpy(before,rtc_regs,sizeof(before));
  assert(tab5_rtc_release_irq()==ESP_OK&&calls==6&&rtc_removed==1&&!rtc_active);check_rtc_preserved(before);
 }
 for(unsigned flags=0;flags<256;flags++){
  reset_model();rtc_regs[0x1d]=(uint8_t)(flags&0xbf);uint8_t before[sizeof(rtc_regs)];memcpy(before,rtc_regs,sizeof(before));
  assert(tab5_rtc_release_irq()==ESP_OK);check_rtc_preserved(before);
 }
 // A newly raised voltage-low flag between read and write is never lost.
 reset_model();rtc_regs[0x1d]=0;rtc_raise_before_clear=0x86;assert(tab5_rtc_release_irq()==ESP_OK&&rtc_regs[0x1d]==0x86);
 for(unsigned failure=1;failure<=6;failure++){
  reset_model();fail_at=failure;assert(tab5_rtc_release_irq()==-1000-(int)failure);
  assert(!rtc_active&&rtc_removed==(failure==1?0:1));
  for(unsigned i=failure;i<calls;i++)assert(operations[i].kind=='x');
 }
 reset_model();fail_at=3;fail_also_at=4;assert(tab5_rtc_release_irq()==-1003);
 reset_model();ignore_write_at=3;assert(tab5_rtc_release_irq()==ESP_ERR_INVALID_RESPONSE);
 reset_model();corrupt_read_at=5;assert(tab5_rtc_release_irq()==ESP_ERR_INVALID_RESPONSE);
 // The second call is idempotent and used again immediately before shutdown.
 reset_model();assert(tab5_rtc_release_irq()==ESP_OK);assert(tab5_rtc_release_irq()==ESP_OK&&rtc_removed==2);
}
int main(void){
 test_rtc_release();
 reset_model();assert(tab5_power_off()==ESP_ERR_TIMEOUT);
 const unsigned normal_calls=calls;assert(normal_calls==64&&removed==1&&rtc_removed==1&&!readback_active&&!rtc_active);
 assert(delays==7&&elapsed==2600&&highs==3&&mutations==11);
 // RTC release is first (6 calls). Every pulse then has five REAL PI4 reads.
 for(unsigned edge=0;edge<6;edge++){
  const unsigned at=27+edge*6;assert(operations[at].kind=='l'&&operations[at].value==!(edge&1));
  assert(operations[at].time==edge*100);
  for(unsigned r=1;r<=5;r++)assert(operations[at+r].kind=='r');
 }
 assert(!(physical_regs[1]&IO_EXPANDER_PIN_NUM_4));check_other_pins();

 // Every operational add/read/cache/write fault preserves its original error.
 // Extra calls may only be LOW deassertion and temporary-handle removal.
 for(unsigned failure=1;failure<normal_calls;failure++){
  reset_model();fail_at=failure;assert(tab5_power_off()==-1000-(int)failure);
  assert(!readback_active&&!rtc_active);assert(removed==(failure<=7?0:1));
  for(unsigned i=failure;i<calls;i++)assert(operations[i].kind=='x'||(operations[i].kind=='l'&&operations[i].value==0));
  check_other_pins();
 }
 // Cleanup failures must not hide a preceding transfer failure.
 reset_model();fail_at=28;fail_also_at=29;assert(tab5_power_off()==-1028&&removed==1);
 reset_model();fail_at=normal_calls;assert(tab5_power_off()==ESP_ERR_TIMEOUT&&removed==1);

 // A stale shadow register is detected before ANY mutation on every register.
 for(unsigned index=0;index<5;index++){
  reset_model();cached_regs[index]^=1;assert(tab5_power_off()==ESP_ERR_INVALID_STATE);
  assert(mutations==0&&delays==0&&removed==1);assert(!memcmp(physical_regs,initial_regs,5));
 }
 reset_model();io_model.read_output_reg=NULL;assert(tab5_power_off()==ESP_ERR_NOT_SUPPORTED&&mutations==0&&removed==1);

 // Accepted driver write is not proof of a physical edge. Readback catches it.
 reset_model();ignore_write_at=22;assert(tab5_power_off()==ESP_ERR_INVALID_RESPONSE&&delays==0&&removed==1);check_other_pins();
 reset_model();ignore_write_at=28;assert(tab5_power_off()==ESP_ERR_INVALID_RESPONSE&&delays==0&&removed==1);check_other_pins();
 // Any unexpected change to another pin is rejected on all five real registers.
 for(unsigned read=23;read<=27;read++){
  reset_model();corrupt_read_at=read;assert(tab5_power_off()==ESP_ERR_INVALID_RESPONSE);
  assert(delays==0&&removed==1&&mutations==5);check_other_pins();
 }
 reset_model();missing_device=true;assert(tab5_power_off()==ESP_FAIL&&calls==0);

 // Modeled hardware may cut power at any pulse: no error UI/return is possible.
 for(unsigned cut=1;cut<=6;cut++){
  reset_model();cut_after_delay=cut;
  if(setjmp(powered_off)==0){(void)tab5_power_off();assert(!"A physical cutoff must not return");}
  assert(elapsed==cut*100&&delays==cut&&removed==0);check_other_pins();
 }
 puts("PASS production RTC IRQ release: 256 controls + 256 flag states, time/calendar/calibration/VLF preserved, asynchronous flags, idempotence, all 6 failure points and readback errors.");
 puts("PASS production UserDemo power-off: RTC release before 3 HIGH100/LOW100 cycles, final LOW; real-register/cache checks, preserved other pins, 63 operational failure points, cleanup errors, absent device and six modeled cutoffs.");
 puts("Host tests validate control flow and I2C diagnostics only, not the physical power controller.");
 return 0;
}
