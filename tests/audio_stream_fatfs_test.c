/* Run the production streaming read/reset functions against the firmware's
 * actual FatFs on a private RAM exFAT disk. SD command counts are simulated
 * from the ESP32-P4 64-byte DMA alignment rule; they are not hardware timings. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ff.h"
#include "diskio.h"

static ssize_t fatfs_read_adapter(int descriptor,void *buffer,size_t bytes);
static off_t fatfs_seek_adapter(int descriptor,off_t offset,int origin);
#define read fatfs_read_adapter
#define lseek fatfs_seek_adapter
#define send audio_command_send
#include "../main/audio_engine.c"
#undef read
#undef lseek
#undef send

#define TEST_SECTORS (128u*1024u)
#define TEST_BYTES (192u*1024u+137u)
static BYTE *disk_bytes;
static FIL source;
static uint64_t sd_commands,sd_sectors;
static unsigned descriptor_reads;
static int inject_interrupt,inject_error,inject_short;
PARTITION VolToPart[FF_VOLUMES]={{0,0}};
void *ff_memalloc(UINT size){return malloc(size);}
void ff_memfree(void *p){free(p);}
int ff_mutex_create(int volume){(void)volume;return 1;}
void ff_mutex_delete(int volume){(void)volume;}
int ff_mutex_take(int volume){(void)volume;return 1;}
void ff_mutex_give(int volume){(void)volume;}
DWORD get_fattime(void){return (DWORD)(2026-1980)<<25 | 10u<<21 | 9u<<16;}
DSTATUS disk_initialize(BYTE drive){return drive==0?0:STA_NOINIT;}
DSTATUS disk_status(BYTE drive){return drive==0?0:STA_NOINIT;}
DRESULT disk_read(BYTE drive,BYTE *buffer,LBA_t sector,UINT count)
{
    if(drive || (uint64_t)sector+count>TEST_SECTORS)return RES_PARERR;
    sd_sectors+=count;
    /* sdmmc_read_sectors issues one multi-sector command for an aligned
     * internal buffer, otherwise one command per sector via its bounce buffer. */
    sd_commands+=((uintptr_t)buffer&63u)?count:1;
    memcpy(buffer,disk_bytes+(size_t)sector*512u,(size_t)count*512u);
    return RES_OK;
}
DRESULT disk_write(BYTE drive,const BYTE *buffer,LBA_t sector,UINT count)
{
    if(drive || (uint64_t)sector+count>TEST_SECTORS)return RES_PARERR;
    memcpy(disk_bytes+(size_t)sector*512u,buffer,(size_t)count*512u);
    return RES_OK;
}
DRESULT disk_ioctl(BYTE drive,BYTE command,void *buffer)
{
    if(drive)return RES_PARERR;
    switch(command) {
    case CTRL_SYNC:case CTRL_TRIM:return RES_OK;
    case GET_SECTOR_COUNT:*(LBA_t*)buffer=TEST_SECTORS;return RES_OK;
    case GET_SECTOR_SIZE:*(WORD*)buffer=512;return RES_OK;
    case GET_BLOCK_SIZE:*(DWORD*)buffer=8;return RES_OK;
    default:return RES_PARERR;
    }
}
static void check(FRESULT result)
{
    if(result!=FR_OK)fprintf(stderr,"FatFs error %d\n",result);
    assert(result==FR_OK);
}
static ssize_t fatfs_read_adapter(int descriptor,void *buffer,size_t bytes)
{
    assert(descriptor==42);
    assert(!((uintptr_t)buffer&63u));
    assert(bytes<=SD_STAGING_BYTES);
    if(bytes>=512)assert(!(f_tell(&source)&511u) && !(bytes&511u));
    ++descriptor_reads;
    if(inject_interrupt){inject_interrupt=0;errno=EINTR;return -1;}
    if(inject_error){inject_error=0;errno=EIO;return -1;}
    if(inject_short){inject_short=0;bytes=bytes>1?bytes/2:bytes;}
    UINT count=0;
    if(f_read(&source,buffer,(UINT)bytes,&count)!=FR_OK){errno=EIO;return -1;}
    return (ssize_t)count;
}
static off_t fatfs_seek_adapter(int descriptor,off_t offset,int origin)
{
    assert(descriptor==42 && origin==SEEK_SET && offset>=0);
    return f_lseek(&source,(FSIZE_t)offset)==FR_OK?(off_t)f_tell(&source):-1;
}
static void reopen(void)
{
    check(f_close(&source));
    check(f_open(&source,"0:/Music/stream.bin",FA_READ));
}
int main(void)
{
    assert(FF_FS_EXFAT==1);
    disk_bytes=calloc(TEST_SECTORS,512);assert(disk_bytes);
    uint8_t *expected=malloc(TEST_BYTES),*allocation=malloc(INPUT_BYTES+128);
    assert(expected && allocation);
    uint8_t *destination=(uint8_t*)(((uintptr_t)allocation+63u)&~(uintptr_t)63u)+13;
    for(size_t i=0;i<TEST_BYTES;i++)expected[i]=(uint8_t)(i*37u+(i>>8));
    FATFS filesystem;BYTE scratch[4096];UINT count;
    MKFS_PARM options={.fmt=FM_EXFAT|FM_SFD,.n_fat=1,.au_size=32768};
    check(f_mkfs("0:",&options,scratch,sizeof scratch));
    check(f_mount(&filesystem,"0:",1));check(f_mkdir("0:/Music"));
    check(f_open(&source,"0:/Music/stream.bin",FA_CREATE_ALWAYS|FA_WRITE));
    check(f_write(&source,expected,TEST_BYTES,&count));assert(count==TEST_BYTES);
    check(f_close(&source));check(f_open(&source,"0:/Music/stream.bin",FA_READ));
    s=calloc(1,sizeof(*s));assert(s);
    s->mutex=xSemaphoreCreateMutex();assert(s->mutex);
    s->file_descriptor=42;
    s->read_staging_allocation=malloc(SD_STAGING_BYTES+63);assert(s->read_staging_allocation);
    s->read_staging=(uint8_t*)(((uintptr_t)s->read_staging_allocation+63u)&~(uintptr_t)63u);
    s->input=malloc(INPUT_BYTES);assert(s->input);

    /* A 23-byte ID3 header plus retained MP3 bytes reproduces the two
     * independent sources of DMA misalignment in the old direct read. */
    check(f_lseek(&source,23));sd_commands=sd_sectors=0;
    check(f_read(&source,destination,INPUT_BYTES,&count));assert(count==INPUT_BYTES);
    assert(!memcmp(destination,expected+23,INPUT_BYTES));
    uint64_t previous_commands=sd_commands;
    reopen();assert(reset_input(23)==0);sd_commands=sd_sectors=0;descriptor_reads=0;
    assert(read_audio(destination,INPUT_BYTES)==INPUT_BYTES);
    assert(!s->read_failed && !memcmp(destination,expected+23,INPUT_BYTES));
    assert(previous_commands>=100 && sd_commands<=12 && descriptor_reads<=6);
    printf("64 KiB at ID3 offset 23 + destination offset 13: direct %llu simulated SD commands, staged %llu; %u descriptor reads\n",
           (unsigned long long)previous_commands,(unsigned long long)sd_commands,descriptor_reads);

    const unsigned offsets[]={0,23,44,54,511,512,32765};
    for(unsigned i=0;i<sizeof offsets/sizeof offsets[0];i++) {
        assert(reset_input(offsets[i])==0);
        assert(read_audio(destination,INPUT_BYTES)==INPUT_BYTES);
        assert(!memcmp(destination,expected+offsets[i],INPUT_BYTES));
        assert(s->file_read_offset==(uint64_t)offsets[i]+INPUT_BYTES);
    }
    assert(reset_input(54)==0);inject_short=1;inject_interrupt=1;
    assert(read_audio(destination,INPUT_BYTES)==INPUT_BYTES);
    assert(!s->read_failed && !memcmp(destination,expected+54,INPUT_BYTES));

    assert(reset_input(TEST_BYTES-100)==0);
    assert(read_audio(destination,400)==100 && !s->read_failed);
    assert(!memcmp(destination,expected+TEST_BYTES-100,100));
    assert(read_audio(destination,400)==0 && !s->read_failed);

    assert(reset_input(23)==0);s->info.data_offset=23;s->info.data_size=TEST_BYTES-23;
    inject_error=1;assert(fill_input()==-1 && s->read_failed);
    assert(reset_input(44)==0 && !s->read_failed);
    assert(read_audio(destination,64)==64 && !memcmp(destination,expected+44,64));
    assert(reset_input((uint64_t)INT32_MAX+1)==-1);

    check(f_close(&source));check(f_mount(NULL,"0:",0));
    vSemaphoreDelete(s->mutex);free(s->read_staging_allocation);free(s->input);free(s);s=NULL;
    free(expected);free(allocation);free(disk_bytes);
    puts("Production stream tests: sector alignment, arbitrary ID3/WAV offsets, seek, short read, EINTR, EOF and I/O error passed");
    return 0;
}
