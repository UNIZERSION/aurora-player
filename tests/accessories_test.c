#undef NDEBUG /* zig -O1/-O2 predefines NDEBUG; keep assert() checks active */
#define TAB5_ACCESSORIES_HOST_TEST
#include "../components/tab5_hal/tab5_accessories.c"
static void reset(void){memset(out,0x55,sizeof out);memset(dir,0xaa,sizeof dir);memset(mode,0xff,sizeof mode);memset(pull,0xff,sizeof pull);calls=fail_at=task_count=input_level=removed_samples=0;fail_read=locked=gate_requested=jack_known=jack_inserted=gate_initialized=false;gate_lock=audio_io=NULL;amp_applied=-1;}
static bool speaker(void){return (out[0]&(1U<<1))!=0;}
int main(void){
 reset();assert(tab5_charger_init()==ESP_OK);assert(out[1]&(1U<<7));assert(dir[1]&(1U<<7));assert(!(dir[1]&(1U<<6)));assert(!(pull[1]&(1U<<6)));assert(!(mode[1]&(1U<<7)));
 assert((out[1]&0x7f)==(0x55&0x7f));assert((dir[1]&0x3f)==(0xaa&0x3f));assert(out[0]==0x55555555); // Other rails and quick-charge bit untouched.
 for(unsigned i=1;i<=6;i++){reset();fail_at=i;assert(tab5_charger_init()==ESP_FAIL&&calls==i);}
 reset();assert(tab5_headphones_init()==ESP_OK&&task_count==1);assert(!speaker());assert(tab5_speaker_gate(true)==ESP_OK);assert(!speaker());assert(tab5_headphones_poll()==ESP_OK&&speaker());bool inserted;assert(tab5_headphones_status(&inserted)&&!inserted);
 input_level=1U<<7;assert(tab5_headphones_poll()==ESP_OK&&!speaker());assert(tab5_headphones_status(&inserted)&&inserted);assert(tab5_speaker_gate(true)==ESP_OK&&!speaker());
 // BSP initialization is unable to leave the amplifier enabled with a jack.
 out[0]|=1U<<1;assert(tab5_speaker_gate_restore()==ESP_OK&&!speaker());
 input_level=0;assert(tab5_headphones_poll()==ESP_OK&&!speaker());assert(tab5_headphones_poll()==ESP_OK&&speaker());
 assert(tab5_speaker_gate(false)==ESP_OK&&!speaker());assert(tab5_headphones_poll()==ESP_OK&&!speaker()); // Mute or BT route wins.
 assert(tab5_speaker_gate(true)==ESP_OK&&speaker());fail_read=true;assert(tab5_headphones_poll()==ESP_FAIL&&!speaker());assert(!tab5_headphones_status(&inserted));fail_read=false;assert(tab5_headphones_poll()==ESP_OK&&!speaker());assert(tab5_headphones_poll()==ESP_OK&&speaker());
 unsigned before=calls;for(int i=0;i<100;i++)assert(tab5_headphones_poll()==ESP_OK);assert(calls==before); // No redundant speaker I2C writes.
 assert((out[0]&~(1U<<1))==(0x55555555U&~(1U<<1)));assert(tab5_headphones_init()==ESP_OK&&task_count==1);
 puts("PASS: production jack insertion/removal, mute/BT gate, codec init, I2C fault recovery, preserved rails and charger enable");return 0;
}
