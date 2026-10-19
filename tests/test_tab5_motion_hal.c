#define TAB5_MOTION_HOST_TEST
#include "../components/tab5_hal/tab5_motion.c"
#include "../managed_components/espressif__bmi270/include_priv/bmi270_priv.h"
#include <math.h>
#include <stdio.h>

_Static_assert(sizeof(bmi270_config_file) == 8192, "Pinned BMI270 blob size changed");
/* Register addresses cross-checked against the pinned component's map. */
_Static_assert(REG_FIFO_LENGTH == BMI270_FIFO_LENGTH_0_REG && REG_FIFO_DATA == BMI270_FIFO_DATA_REG, "FIFO data map");
_Static_assert(REG_FIFO_DOWNS == BMI270_FIFO_DOWNS_REG && REG_FIFO_CONFIG_0 == BMI270_FIFO_CONFIG_0_REG &&
               REG_FIFO_CONFIG_1 == BMI270_FIFO_CONFIG_1_REG && REG_ACC_CONF == BMI270_ACC_CONF_REG &&
               REG_CMD == BMI270_CMD_REG, "FIFO config map");
static uint8_t registers[128], uploaded[8192];
/* Sensor FIFO model: whole 6-byte frames, oldest first. */
static tab5_motion_sample_t fifo[400];
static unsigned fifo_count, fifo_reads, flushes;
static unsigned upload_offset, uploaded_bytes, transfers, failed_transfer, bus_adds, writes, resets;
static unsigned transaction_us;
static bool fail_bus, fail_add, reject_config, corrupt_power, reject_fifo_config;
static int64_t clock_us;

esp_err_t bsp_i2c_init(void) { return fail_bus ? ESP_FAIL : ESP_OK; }
i2c_master_bus_handle_t bsp_i2c_get_handle(void) { return (void*)1; }
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,const i2c_device_config_t *cfg,i2c_master_dev_handle_t *dev)
{
    assert(bus==(void*)1 && cfg->device_address==0x68 && cfg->scl_speed_hz==400000);
    assert(cfg->dev_addr_length==I2C_ADDR_BIT_LEN_7);
    ++bus_adds;
    if(fail_add)return ESP_FAIL;
    *dev=(void*)2;return ESP_OK;
}
static bool fails(void) { clock_us+=transaction_us; return ++transfers==failed_transfer; }
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,const uint8_t *reg,size_t n,uint8_t *out,size_t len,int timeout)
{
    assert(dev==(void*)2 && n==1 && timeout==200);
    if(*reg==REG_FIFO_DATA) {
        /* Burst port: whole frames, never more than stored, bounded transfer. */
        assert(len%6==0 && len/6<=fifo_count && len<=240);
        if(fails())return ESP_ERR_TIMEOUT;
        const unsigned frames=(unsigned)(len/6);
        for(unsigned i=0;i<frames;++i) {
            const int16_t v[]={fifo[i].x,fifo[i].y,fifo[i].z};
            for(unsigned k=0;k<3;++k){out[i*6+k*2]=(uint8_t)v[k];out[i*6+k*2+1]=(uint8_t)((uint16_t)v[k]>>8);}
        }
        memmove(fifo,fifo+frames,(fifo_count-frames)*sizeof fifo[0]);fifo_count-=frames;++fifo_reads;
        return ESP_OK;
    }
    assert(*reg+len<=sizeof(registers));
    if(fails())return ESP_ERR_TIMEOUT;
    const unsigned bytes=fifo_count*6;
    registers[REG_FIFO_LENGTH]=(uint8_t)bytes;registers[REG_FIFO_LENGTH+1]=(uint8_t)(bytes>>8);
    memcpy(out,registers+*reg,len);
    /* ERR_REG fifo_err/aux_err are cleared when read (datasheet 5.2.2). */
    if(*reg<=REG_ERR && *reg+len>REG_ERR)registers[REG_ERR]&=(uint8_t)~0xc0;
    return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev,const uint8_t *data,size_t n,int timeout)
{
    assert(dev==(void*)2 && n>=2 && n<=65 && timeout==200);
    if(fails())return ESP_ERR_TIMEOUT;
    ++writes;
    const unsigned reg=data[0];
    switch(reg) {
    case REG_CMD:
        assert(n==2 && (data[1]==0xb6 || data[1]==0xb0));
        fifo_count=0;registers[REG_ERR]&=(uint8_t)~0x40;
        if(data[1]==0xb0){++flushes;break;}
        ++resets;
        memset(registers,0,sizeof(registers));registers[0]=0x24;
        registers[REG_FIFO_DOWNS]=0x88;registers[REG_FIFO_CONFIG_0]=0x02;registers[REG_FIFO_CONFIG_1]=0x10;
        uploaded_bytes=upload_offset=0;break;
    case REG_FIFO_DOWNS:
        assert(n==2 && data[1]==0x88);registers[reg]=data[1];break;
    case REG_FIFO_CONFIG_0:
        /* Stop-on-full, headerless, accelerometer-only frames. */
        assert(n==3 && data[1]==0x01 && data[2]==0x40);
        registers[reg]=data[1];registers[reg+1]=reject_fifo_config?0x50:data[2];break;
    case REG_INIT_ADDR:
        assert(n==3);upload_offset=((unsigned)data[2]<<4|data[1])*2;
        assert(upload_offset==uploaded_bytes);break;
    case REG_INIT_DATA:
        assert(n==65 && upload_offset+n-1<=sizeof(uploaded));
        memcpy(uploaded+upload_offset,data+1,n-1);uploaded_bytes+=n-1;break;
    case REG_INIT_CTRL:
        assert(n==2 && data[1]<=1);
        if(data[1]) {
            assert(uploaded_bytes==sizeof(uploaded));
            assert(!memcmp(uploaded,bmi270_config_file,sizeof(uploaded)));
            registers[REG_INTERNAL_STATUS]=reject_config?0:1;
        }
        registers[reg]=data[1];break;
    case REG_PWR_CTRL:
        assert(n==2 && (data[1]==0 || data[1]==4)); /* Gyro/temp/AUX never enabled. */
        registers[reg]=corrupt_power?0x0e:data[1];break;
    case REG_PWR_CONF:
        assert(n==2 && data[1]<=1);registers[reg]=data[1];break;
    case REG_ACC_CONF:
        assert(n==2 && data[1]==0xaa);registers[reg]=data[1];break;
    case REG_ACC_RANGE:
        assert(n==2 && data[1]==2);registers[reg]=data[1];break;
    default: assert(!"Must never alter GPIO, interrupts, RTC or another device");
    }
    return ESP_OK;
}
void esp_rom_delay_us(unsigned us) { clock_us+=us; }
void vTaskDelay(TickType_t ticks) { assert(ticks>0 && ticks<=20);clock_us+=(int64_t)ticks*1000; }
int64_t esp_timer_get_time(void) { return clock_us; }
static void reset_fixture(void)
{
    motion_device=NULL;motion_ready=motion_enabled=motion_identified=false;
    memset(registers,0,sizeof(registers));registers[0]=0x24;
    memset(uploaded,0,sizeof(uploaded));
    upload_offset=uploaded_bytes=transfers=failed_transfer=bus_adds=writes=resets=transaction_us=0;
    fifo_count=fifo_reads=flushes=0;
    fail_bus=fail_add=reject_config=corrupt_power=reject_fifo_config=false;clock_us=0;
}
/* The sensor stores frames only while the accelerometer runs into an
 * accelerometer-only headerless FIFO; with stop-on-full it then stops. */
static void sample(int16_t x,int16_t y,int16_t z)
{
    if(registers[REG_PWR_CTRL]!=4 || registers[REG_FIFO_CONFIG_1]!=0x40)return;
    if((fifo_count+1)*6>2048)return;
    fifo[fifo_count++]=(tab5_motion_sample_t){x,y,z};
}
static size_t got;
static bool lost;
static tab5_motion_sample_t out[64];
static esp_err_t drain(size_t capacity) { return tab5_motion_read_fifo(out,capacity,&got,&lost); }
int main(void)
{
    reset_fixture();
    assert(tab5_motion_init()==ESP_OK && motion_ready && !motion_enabled);
    assert(registers[REG_PWR_CTRL]==0 && registers[REG_PWR_CONF]==1);
    const unsigned init_transfers=transfers;
    assert(uploaded_bytes==8192 && clock_us<100000 && bus_adds==1);
    assert(tab5_motion_init()==ESP_OK && transfers==init_transfers);
    got=99;lost=true;
    assert(drain(64)==ESP_ERR_INVALID_STATE && got==0 && !lost);
    assert(tab5_motion_set_enabled(true)==ESP_OK && motion_enabled);
    assert(registers[REG_PWR_CTRL]==4 && registers[REG_PWR_CONF]==0);
    assert(registers[REG_FIFO_CONFIG_0]==0x01 && registers[REG_FIFO_CONFIG_1]==0x40 && flushes==1);
    const unsigned enabled_transfers=transfers;
    assert(tab5_motion_set_enabled(true)==ESP_OK && transfers==enabled_transfers);
    assert(drain(64)==ESP_OK && got==0 && !lost && fifo_reads==0); /* Empty FIFO: no data transfer. */
    sample(4096,-4096,32767);sample(INT16_MIN,0,1);sample(-1,2,-3);
    assert(drain(64)==ESP_OK && got==3 && !lost);
    assert(out[0].x==4096 && out[0].y==-4096 && out[0].z==32767);  /* Oldest first, exact raw values. */
    assert(out[1].x==INT16_MIN && out[1].z==1 && out[2].x==-1 && out[2].y==2 && out[2].z==-3);
    assert(drain(64)==ESP_OK && got==0); /* Each frame is delivered once. */
    for(int i=0;i<50;++i)sample((int16_t)i,0,0);
    assert(drain(64)==ESP_OK && got==40 && out[39].x==39); /* Bounded transfer, */
    assert(drain(64)==ESP_OK && got==10 && out[0].x==40);  /* and the rest follows in order. */
    for(int i=0;i<9;++i)sample((int16_t)i,0,0);
    assert(drain(4)==ESP_OK && got==4 && out[3].x==3);     /* Caller capacity is honoured. */
    assert(drain(64)==ESP_OK && got==5 && out[0].x==4);
    assert(drain(0)==ESP_ERR_INVALID_ARG && got==0);
    assert(tab5_motion_read_fifo(NULL,4,&got,&lost)==ESP_ERR_INVALID_ARG && got==0 && !lost);
    /* A full FIFO stopped storing: frames were lost. Report it and restart. */
    for(int i=0;i<400;++i)sample(1,1,1);
    assert(fifo_count==341);
    const unsigned flushes_before=flushes;
    assert(drain(64)==ESP_OK && got==0 && lost && fifo_count==0 && flushes==flushes_before+1);
    assert(drain(64)==ESP_OK && got==0 && !lost);
    /* A backlog of half the FIFO means the reader stalled: reported before
     * any frame can be lost, whatever the exact stop level is. */
    for(int i=0;i<255;++i)sample(2,2,2);
    assert(drain(64)==ESP_OK && got==40 && !lost);  /* 1530 bytes: still a stream */
    for(int i=0;i<41;++i)sample(3,3,3);
    assert(fifo_count==256 && drain(64)==ESP_OK && got==0 && lost && fifo_count==0);
    sample(5,5,5);registers[REG_ERR]=0x40; /* fifo_err: the same (cleared by the read itself). */
    assert(drain(64)==ESP_OK && got==0 && lost && fifo_count==0 && registers[REG_ERR]==0 && motion_ready);
    assert(tab5_motion_set_enabled(false)==ESP_OK && !motion_enabled && motion_ready);
    assert(registers[REG_PWR_CTRL]==0 && registers[REG_PWR_CONF]==1);
    fifo[fifo_count++]=(tab5_motion_sample_t){1234,5678,9012}; /* Retained from an earlier session. */
    assert(tab5_motion_set_enabled(true)==ESP_OK);
    assert(drain(64)==ESP_OK && got==0); /* Enable flushes old frames. */
    unsigned enable_sites=0;
    for(unsigned fail=1;;++fail) {
        reset_fixture();assert(tab5_motion_init()==ESP_OK);
        failed_transfer=transfers+fail;
        const esp_err_t result=tab5_motion_set_enabled(true);
        if(result==ESP_OK){assert(fail>4);break;}
        assert(!motion_enabled && drain(64)==ESP_ERR_INVALID_STATE);
        ++enable_sites;
    }

    unsigned error_sites=0;
    for(unsigned fail=1;fail<=init_transfers;++fail) {
        reset_fixture();failed_transfer=fail;
        assert(tab5_motion_init()!=ESP_OK && !motion_ready && !motion_enabled);
        assert(drain(64)==ESP_ERR_INVALID_STATE && got==0);
        ++error_sites;
    }
    reset_fixture();fail_bus=true;assert(tab5_motion_init()==ESP_FAIL && !writes);
    reset_fixture();fail_add=true;assert(tab5_motion_init()==ESP_FAIL && !writes);
    reset_fixture();registers[0]=0x25;
    assert(tab5_motion_init()==ESP_ERR_NOT_FOUND && !writes);
    assert(tab5_motion_set_enabled(false)==ESP_OK && !writes);
    reset_fixture();reject_config=true;assert(tab5_motion_init()==ESP_ERR_INVALID_RESPONSE);
    reset_fixture();transaction_us=9000;
    assert(tab5_motion_init()==ESP_ERR_TIMEOUT && clock_us<1600000); /* Total init budget. */

    reset_fixture();assert(tab5_motion_init()==ESP_OK);registers[REG_ACC_CONF]^=1;
    assert(tab5_motion_set_enabled(true)==ESP_ERR_INVALID_RESPONSE && !motion_ready);
    assert(tab5_motion_set_enabled(true)==ESP_OK); /* Reinitialization repairs retained config. */
    assert(tab5_motion_set_enabled(false)==ESP_OK);
    /* A FIFO layout that did not take (another frame format) is never read. */
    reject_fifo_config=true;
    assert(tab5_motion_set_enabled(true)==ESP_ERR_INVALID_RESPONSE && !motion_enabled && !motion_ready);
    reject_fifo_config=false;
    assert(tab5_motion_set_enabled(true)==ESP_OK);
    corrupt_power=true;
    assert(tab5_motion_set_enabled(false)==ESP_OK);
    assert(tab5_motion_set_enabled(true)==ESP_ERR_INVALID_RESPONSE && !motion_enabled);
    corrupt_power=false;
    assert(tab5_motion_set_enabled(true)==ESP_OK);
    registers[REG_ERR]=1;sample(1,2,3); /* Fatal device error cannot become a gesture. */
    assert(drain(64)==ESP_ERR_INVALID_RESPONSE && got==0 && !motion_ready);
    registers[REG_ERR]=0;
    assert(tab5_motion_set_enabled(true)==ESP_OK);
    const unsigned resets_before_timeout=resets;
    const unsigned writes_before_timeout=writes;
    sample(12000,0,0);
    for(unsigned site=1;site<=3;++site) { /* Status, length and data transfers. */
        failed_transfer=transfers+site;
        assert(drain(64)==ESP_ERR_TIMEOUT && got==0 && !lost);
        assert(motion_ready && motion_enabled && writes==writes_before_timeout);
    }
    assert(tab5_motion_set_enabled(false)==ESP_OK); /* Worker discards gesture, stops and backs off. */
    assert(tab5_motion_set_enabled(true)==ESP_OK && resets==resets_before_timeout);
    assert(drain(64)==ESP_OK && got==0); /* Frames from before the timeout are flushed. */
    failed_transfer=transfers+1;
    assert(drain(64)==ESP_ERR_TIMEOUT && got==0);
    assert(tab5_motion_set_enabled(false)==ESP_OK);
    registers[REG_ACC_RANGE]=0; /* Retained state is still validated after a transient timeout. */
    assert(tab5_motion_set_enabled(true)==ESP_ERR_INVALID_RESPONSE && !motion_ready && !motion_enabled);
    assert(resets==resets_before_timeout);
    assert(tab5_motion_set_enabled(true)==ESP_OK && resets==resets_before_timeout+1);
    failed_transfer=transfers+1;
    assert(tab5_motion_set_enabled(false)==ESP_ERR_TIMEOUT && !motion_enabled);
    assert(drain(64)==ESP_ERR_INVALID_STATE && got==0);
    printf("PASS BMI270 HAL: %u initialization and %u enable I/O failure sites; headerless accel-only FIFO, exact raw frames once and in order, bounded transfers, overflow reported and flushed, standby, accel-only power, bounded deadlines, transient timeout recovery without reupload, retained config verification, bus reuse, no IRQ/wake writes\n",error_sites,enable_sites);
    return 0;
}
