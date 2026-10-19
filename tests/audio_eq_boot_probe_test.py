"""Exercise the actual boot probe, including its allocation failure paths.

Fake RTOS scheduling cannot validate actual P4 scheduling/timing; it validates
the production probe's branch coverage, bounds and completion/ownership flow.
"""
from pathlib import Path
import argparse
import subprocess

p = argparse.ArgumentParser()
p.add_argument("--zig", required=True)
p.add_argument("--build", required=True)
args = p.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / "main/audio_engine.c").read_text(encoding="utf-8")
start = source.index("typedef struct { uint32_t min_us,max_us,count; } eq_timing_range_t;")
end = source.index("\n#endif", start)
production = source[start:end]
build = Path(args.build).resolve()
build.mkdir(parents=True, exist_ok=True)
fixture = r'''
#undef NDEBUG
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "audio_eq.h"
#define PCM_FRAMES 1152
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
typedef struct {int16_t pcm[PCM_FRAMES*2];} pcm_block_t;
typedef struct {audio_eq_t *eq;pcm_block_t *blocks;} engine_t;
typedef struct {int given;} semaphore_t;
typedef semaphore_t *SemaphoreHandle_t;
static semaphore_t semaphore;
static int fail_sem,fail_task,creates,deletes,takes,task_creates,task_deletes;
static unsigned logs,errors,calls,resets,active_changes,fade_calls,steady_calls;
static int64_t now;
static audio_eq_config_t previous;
static void log_message(const char *tag,const char *format,...)
{(void)tag;(void)format;++logs;}
#define ESP_LOGI log_message
#define ESP_LOGW log_message
static int64_t esp_timer_get_time(void){now+=100;return now;}
static SemaphoreHandle_t xSemaphoreCreateBinary(void)
{++creates;semaphore.given=0;return fail_sem?NULL:&semaphore;}
static int xSemaphoreGive(SemaphoreHandle_t h){assert(h==&semaphore);h->given=1;return 1;}
static int xSemaphoreTake(SemaphoreHandle_t h,unsigned timeout)
{assert(h==&semaphore&&h->given&&timeout==portMAX_DELAY);++takes;return 1;}
static void vSemaphoreDelete(SemaphoreHandle_t h){assert(h==&semaphore);++deletes;}
static void vTaskDelete(void *handle){assert(!handle);++task_deletes;}
static int xTaskCreatePinnedToCore(void (*fn)(void*),const char *name,unsigned stack,
                                void *param,unsigned priority,void *handle,int core)
{
    assert(!strcmp(name,"eq_boot_probe")&&stack==8192&&priority==7&&!handle&&core==1);
    ++task_creates;if(fail_task)return 0;fn(param);return pdPASS;
}
static void checked_reset(audio_eq_t *eq,uint32_t rate)
{++resets;memset(&previous,0,sizeof previous);audio_eq_reset(eq,rate);}
static void checked_process(audio_eq_t *eq,int16_t *pcm,size_t frames,const audio_eq_config_t *c)
{
    assert(frames==PCM_FRAMES&&c->enabled);
    for(unsigned i=0;i<frames*2;++i)assert(pcm[i]==((i&32)?24000:-24000));
    for(unsigned i=0;i<11;++i)assert(c->gain_db[i]==12||c->gain_db[i]==-12);
    if(memcmp(c,&previous,sizeof *c)){
        if(previous.enabled){assert(!eq->transition&&eq->current.count==11);++active_changes;}
        previous=*c;
    }else if(eq->transition)++fade_calls;
    else ++steady_calls;
    ++calls;audio_eq_process(eq,pcm,frames,c);
}
#define audio_eq_reset checked_reset
#define audio_eq_process checked_process
'''
checks = r'''
#undef audio_eq_reset
#undef audio_eq_process
int main(void)
{
    audio_eq_t eq,saved;pcm_block_t block,saved_block;
    memset(&eq,0xa5,sizeof eq);memset(&block,0x5a,sizeof block);
    saved=eq;saved_block=block;engine_t e={&eq,&block};
    fail_sem=1;eq_timing_boot(&e);
    assert(creates==1&&deletes==0&&task_creates==0&&takes==0&&calls==0);
    assert(!memcmp(&eq,&saved,sizeof eq)&&!memcmp(&block,&saved_block,sizeof block));
    fail_sem=0;fail_task=1;eq_timing_boot(&e);
    assert(creates==2&&deletes==1&&task_creates==1&&takes==0&&calls==0);
    assert(!memcmp(&eq,&saved,sizeof eq)&&!memcmp(&block,&saved_block,sizeof block));
    fail_task=0;eq_timing_boot(&e);
    assert(creates==3&&deletes==2&&task_creates==2&&takes==1&&task_deletes==1);
    assert(calls==34&&resets==3&&active_changes==12&&fade_calls==5&&steady_calls==14);
    assert(eq.rate==96000&&eq.transition==0&&eq.current.count==11&&logs==6);
    /* Call the task directly to inspect its exact recorded measurement sets. */
    eq_timing_context_t ctx={.engine=&e,.complete=&semaphore};eq_timing_task(&ctx);
    assert(ctx.elapsed_us>0);
    for(unsigned i=0;i<3;++i){
        assert(ctx.result[i].change.count==4&&ctx.result[i].steady.count==4);
        assert(ctx.result[i].change.min_us==100&&ctx.result[i].change.max_us==100);
        assert(ctx.result[i].steady.min_us==100&&ctx.result[i].steady.max_us==100);
        assert(ctx.result[i].fade.count==(i==2?4u:0u));
        assert(ctx.result[i].fade.max_us==(i==2?100u:0u));
    }
    puts("PASS production EQ boot probe: semaphore/task failure skip unchanged state; matched core1/prio7; 34 bounded PCM blocks,12 active-to-active changes, correct fade/steady statistics and completion cleanup. Fake RTOS does not measure hardware time.");
    return errors;
}
'''
fixture_path = build / "audio_eq_boot_probe_test.c"
fixture_path.write_text(fixture + production + checks, encoding="utf-8")
exe = build / "audio_eq_boot_probe_test.exe"
subprocess.run([args.zig, "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(root / "main"), str(fixture_path),
                str(root / "main/audio_eq.c"), "-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True)
