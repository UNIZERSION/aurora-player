/* Exercises the actual patched FatFs source/config on a private 64 MiB RAM
 * disk. It never opens or formats a real device. ESP VFS/SD timing excluded. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "diskio.h"

#define SECTORS (128u * 1024u)
#define SECTOR_SIZE 512u
static BYTE *disk_bytes;
static uint64_t read_sectors;
PARTITION VolToPart[FF_VOLUMES]={{0,0}};
void *ff_memalloc(UINT size){return malloc(size);}
void ff_memfree(void *p){free(p);}
int ff_mutex_create(int volume){(void)volume;return 1;}
void ff_mutex_delete(int volume){(void)volume;}
int ff_mutex_take(int volume){(void)volume;return 1;}
void ff_mutex_give(int volume){(void)volume;}
DWORD get_fattime(void){return (DWORD)(2026-1980)<<25 | 10u<<21 | 9u<<16 | 16u<<11;}
DSTATUS disk_initialize(BYTE drive){return drive==0?0:STA_NOINIT;}
DSTATUS disk_status(BYTE drive){return drive==0?0:STA_NOINIT;}
DRESULT disk_read(BYTE drive,BYTE *buffer,LBA_t sector,UINT count)
{
    if(drive || (uint64_t)sector+count>SECTORS)return RES_PARERR;
    read_sectors+=count;
    memcpy(buffer,disk_bytes+(size_t)sector*SECTOR_SIZE,(size_t)count*SECTOR_SIZE);return RES_OK;
}
DRESULT disk_write(BYTE drive,const BYTE *buffer,LBA_t sector,UINT count)
{
    if(drive || (uint64_t)sector+count>SECTORS)return RES_PARERR;
    memcpy(disk_bytes+(size_t)sector*SECTOR_SIZE,buffer,(size_t)count*SECTOR_SIZE);return RES_OK;
}
DRESULT disk_ioctl(BYTE drive,BYTE command,void *buffer)
{
    if(drive)return RES_PARERR;
    switch(command) {
    case CTRL_SYNC:case CTRL_TRIM:return RES_OK;
    case GET_SECTOR_COUNT:*(LBA_t*)buffer=SECTORS;return RES_OK;
    case GET_SECTOR_SIZE:*(WORD*)buffer=SECTOR_SIZE;return RES_OK;
    case GET_BLOCK_SIZE:*(DWORD*)buffer=8;return RES_OK;
    default:return RES_PARERR;
    }
}
static void check(FRESULT result,const char *operation)
{
    if(result!=FR_OK)fprintf(stderr,"%s returned FatFs error %d\n",operation,result);
    assert(result==FR_OK);
}
int main(void)
{
    assert(FF_FS_EXFAT==1 && FF_USE_LFN!=0);
    disk_bytes=calloc(SECTORS,SECTOR_SIZE);assert(disk_bytes);
    BYTE scratch[4096];FATFS fs;FIL file;UINT count;
    MKFS_PARM options={.fmt=FM_EXFAT|FM_SFD,.n_fat=1,.au_size=32768};
    check(f_mkfs("0:",&options,scratch,sizeof scratch),"RAM exFAT format");
    assert(!memcmp(disk_bytes+3,"EXFAT   ",8));
    check(f_mount(&fs,"0:",1),"mount");assert(fs.fs_type==FS_EXFAT);
    check(f_mkdir("0:/.tab5-player"),"settings directory");
    check(f_mkdir("0:/Music"),"music directory");
    const char *path="0:/.tab5-player/settings-a.bin";
    BYTE expected[65539],actual[65539];
    for(unsigned i=0;i<sizeof expected;i++)expected[i]=(BYTE)(i*37u+19u);
    check(f_open(&file,path,FA_CREATE_ALWAYS|FA_WRITE),"settings create");
    check(f_write(&file,expected,sizeof expected,&count),"settings write");assert(count==sizeof expected);
    check(f_sync(&file),"settings sync");check(f_close(&file),"settings close");
    check(f_open(&file,"0:/Music/Neon \xC5\x9Awiat\xC5\x82o 01.wav",FA_CREATE_ALWAYS|FA_WRITE),"UTF-8 long filename");
    check(f_write(&file,"RIFF-test",9,&count),"music write");check(f_close(&file),"music close");
    check(f_mount(NULL,"0:",0),"unmount");memset(&fs,0,sizeof fs);
    check(f_mount(&fs,"0:",1),"remount");assert(fs.fs_type==FS_EXFAT);
    /* The status display may inspect already mounted metadata, but must not
     * enumerate allocation bits just to report card capacity. Verify that
     * opening the root exposes metadata without issuing storage reads. */
    assert(fs.free_clst>fs.n_fatent-2);
    FF_DIR root;uint64_t reads_before=read_sectors;
    check(f_opendir(&root,"0:/"),"root metadata");
    assert(root.obj.fs==&fs && root.obj.fs->csize>0);
    assert(root.obj.fs->free_clst>root.obj.fs->n_fatent-2);
    check(f_closedir(&root),"root metadata close");
    assert(read_sectors==reads_before);
    check(f_open(&file,path,FA_READ),"settings reopen");assert(f_size(&file)==sizeof expected);
    check(f_read(&file,actual,sizeof actual,&count),"settings read");assert(count==sizeof actual && !memcmp(expected,actual,sizeof actual));
    check(f_lseek(&file,32765),"seek across cluster");
    check(f_read(&file,actual,16,&count),"cluster boundary read");assert(count==16 && !memcmp(expected+32765,actual,16));
    check(f_close(&file),"read close");
    FILINFO info;check(f_stat("0:/Music/Neon \xC5\x9Awiat\xC5\x82o 01.wav",&info),"UTF-8 stat");assert(info.fsize==9);
    reads_before=read_sectors;
    DWORD free_clusters;FATFS *mounted;check(f_getfree("0:",&free_clusters,&mounted),"free space");assert(free_clusters>0 && mounted==&fs);
    assert(read_sectors>reads_before);
    printf("Mounted root metadata: 0 sectors read; first f_getfree: %llu bitmap sectors read\n",(unsigned long long)(read_sectors-reads_before));
    check(f_mount(NULL,"0:",0),"final unmount");free(disk_bytes);
    puts("exFAT RAM-disk tests: format, mount, settings write/sync/remount/read, cluster seek, UTF-8 filenames, free space passed");
    return 0;
}
