#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "audio_engine.h"

enum {GATE_CONFIGURE=1,GATE_WRITE,GATE_MUTE,GATE_UNMUTE};
static HANDLE entered,release_gate;
static volatile LONG gate_kind,writes,mutes,output_muted,fail_mute;
static uint32_t current_rate;
static void gate(unsigned kind)
{
    if(InterlockedCompareExchange(&gate_kind,0,(LONG)kind)==(LONG)kind) {
        SetEvent(entered);
        assert(WaitForSingleObject(release_gate,3000)==WAIT_OBJECT_0);
    }
}
static void arm(unsigned kind)
{ ResetEvent(entered);ResetEvent(release_gate);InterlockedExchange(&gate_kind,(LONG)kind); }
static esp_err_t configure(void *user,uint32_t rate)
{ (void)user;gate(GATE_CONFIGURE);current_rate=rate;return ESP_OK; }
static esp_err_t write_pcm(void *user,const int16_t *pcm,size_t frames)
{
    (void)user;(void)pcm;gate(GATE_WRITE);InterlockedIncrement(&writes);
    Sleep((DWORD)(frames*1000/current_rate));return ESP_OK;
}
static esp_err_t volume(void *user,uint8_t percent,bool mute)
{
    (void)user;assert(percent<=100);gate(mute?GATE_MUTE:GATE_UNMUTE);
    if(mute && InterlockedCompareExchange(&fail_mute,0,0))return ESP_FAIL;
    if(mute)InterlockedIncrement(&mutes);
    InterlockedExchange(&output_muted,mute);return ESP_OK;
}
static audio_snapshot_t snapshot(void)
{ audio_snapshot_t a;audio_engine_get_snapshot(&a);return a; }
static void wait_state(audio_state_t wanted)
{
    ULONGLONG start=GetTickCount64();
    while(snapshot().state!=wanted && GetTickCount64()-start<3500)Sleep(1);
    assert(snapshot().state==wanted);
}
typedef struct {esp_err_t result;uint32_t timeout;} waiter_t;
static DWORD WINAPI wait_pause(void *arg)
{ waiter_t *w=arg;w->result=audio_engine_pause_and_wait(w->timeout);return 0; }
static HANDLE start_wait(waiter_t *w)
{ w->result=999;w->timeout=2000;return CreateThread(NULL,0,wait_pause,w,0,NULL); }
static void finish_wait(HANDLE thread,waiter_t *w,esp_err_t expected)
{ assert(WaitForSingleObject(thread,2500)==WAIT_OBJECT_0);CloseHandle(thread);assert(w->result==expected); }
static void assert_quiet_state(const char *path,audio_state_t wanted)
{
    audio_snapshot_t a=snapshot();LONG count=InterlockedCompareExchange(&writes,0,0);
    assert(a.state==wanted && !strcmp(a.path,path));
    assert(InterlockedCompareExchange(&output_muted,0,0));
    Sleep(80);
    audio_snapshot_t b=snapshot();
    assert(b.state==wanted && b.position_ms==a.position_ms);
    assert(count==InterlockedCompareExchange(&writes,0,0));
}
static void assert_quiet(const char *path) { assert_quiet_state(path,AUDIO_PAUSED); }
int main(int argc,char **argv)
{
    assert(argc==2);
    entered=CreateEvent(NULL,TRUE,FALSE,NULL);release_gate=CreateEvent(NULL,TRUE,FALSE,NULL);
    assert(entered && release_gate);
    audio_output_t output={.configure=configure,.write=write_pcm,.volume=volume};
    assert(audio_engine_init(&output)==ESP_OK);
    /* An idle route handover still forces a new mute without inventing PAUSED. */
    assert(audio_engine_pause_and_wait(1000)==ESP_OK);
    assert_quiet_state("",AUDIO_STOPPED);

    /* Decoder has acknowledged PAUSED, but configure remains in flight. */
    arm(GATE_CONFIGURE);assert(audio_engine_play(argv[1])==ESP_OK);
    assert(WaitForSingleObject(entered,2000)==WAIT_OBJECT_0);
    waiter_t waiter;HANDLE thread=start_wait(&waiter);assert(thread);
    wait_state(AUDIO_PAUSED);assert(WaitForSingleObject(thread,50)==WAIT_TIMEOUT);
    SetEvent(release_gate);finish_wait(thread,&waiter,ESP_OK);assert_quiet(argv[1]);

    /* A cached mute/old PAUSED cannot ACK a fresh token. */
    LONG prior_mutes=InterlockedCompareExchange(&mutes,0,0);
    arm(GATE_MUTE);thread=start_wait(&waiter);
    assert(WaitForSingleObject(entered,2000)==WAIT_OBJECT_0);
    assert(WaitForSingleObject(thread,50)==WAIT_TIMEOUT);
    SetEvent(release_gate);finish_wait(thread,&waiter,ESP_OK);
    assert(InterlockedCompareExchange(&mutes,0,0)>prior_mutes);assert_quiet(argv[1]);

    /* An unmute callback in flight must complete before the new forced mute. */
    arm(GATE_UNMUTE);assert(audio_engine_pause(false)==ESP_OK);
    assert(WaitForSingleObject(entered,2000)==WAIT_OBJECT_0);
    thread=start_wait(&waiter);wait_state(AUDIO_PAUSED);
    assert(WaitForSingleObject(thread,50)==WAIT_TIMEOUT);
    SetEvent(release_gate);finish_wait(thread,&waiter,ESP_OK);assert_quiet(argv[1]);

    /* Timeout is bounded while write is blocked; it does not resume audio. */
    arm(GATE_WRITE);assert(audio_engine_pause(false)==ESP_OK);
    assert(WaitForSingleObject(entered,2000)==WAIT_OBJECT_0);
    ULONGLONG start=GetTickCount64();
    assert(audio_engine_pause_and_wait(30)==ESP_ERR_TIMEOUT);
    assert(GetTickCount64()-start<500);wait_state(AUDIO_PAUSED);
    SetEvent(release_gate);
    assert(audio_engine_pause_and_wait(1000)==ESP_OK);assert_quiet(argv[1]);

    /* Failed mute is reported, not accepted as quiescent. Retry is fresh. */
    InterlockedExchange(&fail_mute,1);
    assert(audio_engine_pause_and_wait(1000)==ESP_FAIL);
    InterlockedExchange(&fail_mute,0);
    assert(audio_engine_pause_and_wait(1000)==ESP_OK);assert_quiet(argv[1]);

    /* Explicit transport supersession during mute cannot return stale ACK. */
    arm(GATE_MUTE);thread=start_wait(&waiter);
    assert(WaitForSingleObject(entered,2000)==WAIT_OBJECT_0);
    assert(audio_engine_stop_and_wait(1000)==ESP_OK);
    SetEvent(release_gate);finish_wait(thread,&waiter,ESP_ERR_INVALID_STATE);
    assert(audio_engine_pause_and_wait(1000)==ESP_OK);
    assert_quiet_state(argv[1],AUDIO_STOPPED);

    /* STOP acknowledges decoder closure before an old write returns. The
     * idle pause barrier must still wait for that output callback. */
    arm(GATE_WRITE);assert(audio_engine_play(argv[1])==ESP_OK);
    assert(WaitForSingleObject(entered,2000)==WAIT_OBJECT_0);
    assert(audio_engine_stop_and_wait(1000)==ESP_OK);
    assert(snapshot().state==AUDIO_STOPPED);
    thread=start_wait(&waiter);
    assert(WaitForSingleObject(thread,50)==WAIT_TIMEOUT);
    SetEvent(release_gate);finish_wait(thread,&waiter,ESP_OK);
    assert_quiet_state(argv[1],AUDIO_STOPPED);

    assert(audio_engine_play(argv[1])==ESP_OK);wait_state(AUDIO_ENDED);
    audio_snapshot_t ended=snapshot();
    assert(audio_engine_pause_and_wait(1000)==ESP_OK);
    assert_quiet_state(argv[1],AUDIO_ENDED);
    assert(snapshot().event_sequence==ended.event_sequence);

    assert(audio_engine_play("missing-pause-test.mp3")==ESP_OK);wait_state(AUDIO_ERROR);
    audio_snapshot_t error=snapshot();
    assert(audio_engine_pause_and_wait(1000)==ESP_OK);
    assert_quiet_state("missing-pause-test.mp3",AUDIO_ERROR);
    assert(snapshot().event_sequence==error.event_sequence);
    assert(!strcmp(snapshot().error,error.error));
    puts("PASS: pause barrier configure/volume/write, fresh mute/token, stable position, timeout/failure/cancel and idle STOPPED/ENDED/ERROR");
    return 0;
}
