"""Compile actual shutdown methods extracted from main.cpp against deterministic host faults.
No duplicate controller implementation, no device access. The generated harness is scratch output.
"""
from pathlib import Path
import argparse, subprocess
p=argparse.ArgumentParser();p.add_argument('--zig',required=True);p.add_argument('--build',required=True);args=p.parse_args()
root=Path(__file__).resolve().parents[1];source=(root/'main/main.cpp').read_text(encoding='utf-8-sig');build=Path(args.build).resolve();build.mkdir(parents=True,exist_ok=True)
def method(name):
    start=source.index('    void '+name+'(')
    end=source.find('\n    void ',start+1)
    if name=='perform_shutdown':end=source.index('\n    const player::Track',start)
    return source[start:end]
methods='\n'.join(method(n) for n in ['restore_shutdown_error','shutdown_error','request_shutdown','perform_shutdown'])
start=source.index('            if (shutdown_requested) {',source.index('    void run()'))
end=source.index('                Command pending{};',start)
tick='    void tick() {\n'+source[start:end]+'            }\n    }\n'
harness=r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <atomic>
using esp_err_t=int;constexpr int ESP_OK=0,ESP_ERR_TIMEOUT=1;static const char *TAG="test";
template<class... T> void log_ignore(T...){ }
#define ESP_LOGE(...) log_ignore(__VA_ARGS__)
#define ESP_LOGW(...) log_ignore(__VA_ARGS__)
#define pdMS_TO_TICKS(x) (x)
enum {POWER_RETRY,RESTART};
struct audio_snapshot_t { unsigned position_ms=0; };
struct Test {
 uint64_t now=0;bool locked=false,asleep=false,ui_sleep=false;
 unsigned restore_lock_fail=0,blank_lock_fail=0,wake_fail=0,blank_fail=0,stop_fail=0,art_fail=0,unmount_fail=0,lock_worker_fail=0,motion_fail=0;
 unsigned cutoff=0,wakes=0,blanks=0,dialogs=0,locks=0,unlocks=0,stops=0,art_stops=0,unmounts=0;
 unsigned settings_saves=0,favorite_saves=0,save_position=0,pico_stops=0,lock_worker_stops=0,motion_stops=0,motion_disarms=0;
 bool lock_worker_live=false;
 bool save_fail=false,favorite_fail=false;unsigned live_position=54321;
} t;
namespace player {
 void sleep_tap_arm(bool enabled){assert(!enabled);++t.motion_disarms;}
 bool sleep_tap_stop_and_wait(uint32_t timeout){assert(!t.locked&&timeout==1800);++t.motion_stops;if(t.motion_fail){--t.motion_fail;return false;}return true;}
}
uint64_t now_ms(){return t.now;}
bool tab5_display_lock(unsigned ms){assert(!t.locked);if(ms==20&&t.restore_lock_fail){--t.restore_lock_fail;return false;}if(ms==1000&&t.blank_lock_fail){--t.blank_lock_fail;return false;}t.locked=true;++t.locks;return true;}
void tab5_display_unlock(){assert(t.locked);t.locked=false;++t.unlocks;}
void player_ui_set_sleeping(bool value){assert(t.locked);t.ui_sleep=value;}
esp_err_t tab5_display_sleep(bool value,unsigned){assert(t.locked);if(value){++t.blanks;if(t.blank_fail){--t.blank_fail;return 1;}}else{++t.wakes;if(t.wake_fail){--t.wake_fail;return 1;}}t.asleep=value;return 0;}
int *new_dialog(const char*,bool){assert(t.locked);++t.dialogs;return reinterpret_cast<int*>(1);}
int *label(int*,const char *text,int,int,int){assert(text);return reinterpret_cast<int*>(2);}
void button(int*,const char*,int,int,int,int){}
void audio_engine_get_snapshot(audio_snapshot_t *a){a->position_ms=t.live_position;}
esp_err_t audio_engine_stop_and_wait(unsigned){assert(!t.locked&&!t.lock_worker_live);++t.stops;if(t.stop_fail){--t.stop_fail;return 1;}t.live_position=0;return 0;}
bool player_artwork_stop_and_wait(unsigned){assert(!t.locked);++t.art_stops;if(t.art_fail){--t.art_fail;return false;}return true;}
esp_err_t bluetooth_bridge_shutdown(unsigned){assert(!t.locked);++t.pico_stops;return 0;}
esp_err_t tab5_storage_unmount(){assert(!t.locked);++t.unmounts;if(t.unmount_fail){--t.unmount_fail;return 1;}return 0;}
esp_err_t tab5_power_off(){assert(!t.locked);assert(t.asleep);++t.cutoff;return ESP_ERR_TIMEOUT;}
const char *esp_err_to_name(int){return "test failure";}
void vTaskDelay(unsigned){}
struct Favorites {bool changed=true;bool dirty(){return changed;}bool save(){assert(!t.lock_worker_live);++t.favorite_saves;if(t.favorite_fail)return false;changed=false;return true;}};
struct Library {void flush_metadata_cache(unsigned){assert(!t.locked);}};
struct Player {
 struct {bool persistent_read_error=false;} storage_layout;
 bool shutdown_requested=false,shutdown_attempted=false,shutdown_failed=false;
 bool shutdown_error_pending=false,shutdown_error_dialog_ready=false;
 const char *shutdown_failure_message=nullptr;uint64_t shutdown_error_retry_at=0;
 bool audio_ready=true,bt_ready=true,mounted=true,dirty=false,dimmed=false,play_pending=false,resume_pending=false;
 unsigned pending_resume=0;void *screenshot_job=nullptr,*scan_job=nullptr;int *power_message=nullptr;
 struct LockJob {std::atomic_bool cancel{false};};LockJob *lock_job=nullptr;
 struct {unsigned brightness=73;} settings;
 audio_snapshot_t audio{},shutdown_audio{};std::atomic_bool artwork_io_allowed{true};Favorites favorites;Library library;
 void notice(const char*){}
 void power_status(const char*){assert(!t.locked);}
 void save(){assert(!t.locked&&!t.lock_worker_live);assert(mounted);++t.settings_saves;t.save_position=audio.position_ms;dirty=t.save_fail;}
 bool lock_stop_worker(uint32_t timeout){
  assert(!t.locked&&timeout==3000);++t.lock_worker_stops;
  if(lock_job){assert(lock_job->cancel.load(std::memory_order_acquire));t.lock_worker_live=true;}
  if(t.lock_worker_fail){--t.lock_worker_fail;return false;}
  lock_job=nullptr;t.lock_worker_live=false;return true;
 }
'''+methods+'\n'+tick+r'''
};
void idle(Player &p,unsigned loops){for(unsigned i=0;i<loops;i++){t.now+=25;p.tick();}assert(!t.locked);assert(t.locks==t.unlocks);}
int main(){
 // Restore lock timeout and then brightness failure must retry just the screen.
 t={};Player p;p.request_shutdown();t.restore_lock_fail=1;t.wake_fail=1;p.tick();
 assert(t.cutoff==1&&p.shutdown_error_pending&&p.dimmed&&t.asleep);unsigned dialogs=t.dialogs;
 idle(p,19);assert(t.cutoff==1&&t.dialogs==dialogs);idle(p,1);assert(t.wakes==1&&p.shutdown_error_pending&&t.ui_sleep);
 assert(t.dialogs==dialogs+1);idle(p,20);assert(t.wakes==2&&!p.shutdown_error_pending&&!p.dimmed&&!t.asleep&&!t.ui_sleep);
 assert(t.dialogs==dialogs+1);idle(p,100);assert(t.cutoff==1);
 assert(t.settings_saves==1&&t.save_position==54321&&!p.mounted);
 p.request_shutdown();p.tick();assert(t.cutoff==2&&t.settings_saves==1&&p.shutdown_audio.position_ms==54321);
 // Failed persistence stays mounted and is retried only on explicit request.
 t={};Player s;t.save_fail=true;s.request_shutdown();s.tick();assert(t.cutoff==0&&s.mounted&&s.dirty&&s.shutdown_failed);
 idle(s,100);assert(t.settings_saves==1&&t.cutoff==0);t.save_fail=false;s.request_shutdown();s.tick();
 assert(t.settings_saves==2&&t.save_position==54321&&t.cutoff==1);
 // Favorites failure retains SD and stops before cutoff; retry keeps snapshot.
 t={};Player f;t.favorite_fail=true;f.request_shutdown();f.tick();assert(f.mounted&&t.cutoff==0&&t.unmounts==0);
 t.favorite_fail=false;f.request_shutdown();f.tick();assert(t.favorite_saves==2&&t.save_position==54321&&t.cutoff==1);
 // An unreadable/migration-blocked persistent source must never be replaced
 // by the in-memory favorites fallback, even when its writer would succeed.
 t={};Player unreadable;unreadable.storage_layout.persistent_read_error=true;
 unreadable.request_shutdown();unreadable.tick();
 assert(unreadable.shutdown_failed&&unreadable.mounted&&unreadable.favorites.dirty());
 assert(t.favorite_saves==0&&t.unmounts==0&&t.cutoff==0);
 idle(unreadable,100);assert(t.favorite_saves==0&&t.cutoff==0);
 unreadable.request_shutdown();unreadable.tick();assert(t.favorite_saves==0&&t.unmounts==0&&t.cutoff==0);
 unreadable.storage_layout.persistent_read_error=false;unreadable.request_shutdown();unreadable.tick();
 assert(t.favorite_saves==1&&!unreadable.favorites.dirty()&&t.cutoff==1);
 // Unmount consumes handle even on failure: never save to it on a retry.
 t={};Player u;t.unmount_fail=1;u.request_shutdown();u.tick();assert(!u.mounted&&t.cutoff==0&&t.settings_saves==1);
 u.request_shutdown();u.tick();assert(t.unmounts==1&&t.settings_saves==1&&t.cutoff==1);
 // Failed blanking must keep power on; only the explicit retry cuts again.
 t={};Player b;t.blank_fail=1;b.request_shutdown();b.tick();assert(t.cutoff==0&&b.shutdown_failed&&!t.asleep);
 idle(b,30);assert(t.cutoff==0);b.request_shutdown();b.tick();assert(t.cutoff==1);
 // A busy display at the blanking step must not trigger hardware cutoff.
 t={};Player busy;busy.request_shutdown();t.blank_lock_fail=1;busy.tick();assert(t.cutoff==0&&busy.shutdown_failed&&!t.asleep);
 idle(busy,30);assert(t.cutoff==0);busy.request_shutdown();busy.tick();assert(t.cutoff==1);
 // Quiescence failures must prevent save/unmount/cutoff.
 t={};Player a;t.stop_fail=1;a.request_shutdown();a.tick();assert(t.cutoff==0&&t.settings_saves==0&&t.unmounts==0);
 a.request_shutdown();a.tick();assert(t.cutoff==1&&t.save_position==54321);
 t={};Player c;t.art_fail=1;c.request_shutdown();c.tick();assert(t.cutoff==0&&t.settings_saves==0&&t.unmounts==0);
 c.request_shutdown();c.tick();assert(t.cutoff==1&&t.save_position==54321);
 // Request cancels the wallpaper reader immediately, but cancellation is not
 // completion. A timeout keeps its job/SD alive and permits only explicit retry.
 t={};Player reader;Player::LockJob job;reader.lock_job=&job;t.lock_worker_live=true;t.lock_worker_fail=1;
 reader.request_shutdown();assert(job.cancel.load(std::memory_order_acquire));reader.tick();
 assert(reader.shutdown_failed&&reader.lock_job==&job&&reader.mounted&&t.lock_worker_live);
 assert(t.lock_worker_stops==1&&t.stops==0&&t.settings_saves==0&&t.favorite_saves==0&&t.unmounts==0&&t.cutoff==0);
 idle(reader,100);assert(t.lock_worker_stops==1&&t.stops==0&&t.settings_saves==0&&t.cutoff==0);
 reader.request_shutdown();reader.tick();
 assert(t.lock_worker_stops==2&&!reader.lock_job&&!t.lock_worker_live&&t.cutoff==1&&t.save_position==54321);
 // Motion sampling must disarm immediately and acknowledge its bus is idle
 // before other shutdown work or a power cut; failed join stays on for retry.
 t={};Player motion;t.motion_fail=1;motion.request_shutdown();assert(t.motion_disarms==1);motion.tick();
 assert(motion.shutdown_failed&&t.motion_stops==1&&t.lock_worker_stops==0&&t.stops==0&&t.cutoff==0);
 idle(motion,100);assert(t.motion_stops==1&&t.cutoff==0);
 motion.request_shutdown();motion.tick();assert(t.motion_disarms==2&&t.motion_stops==2&&t.cutoff==1);
 // A live SD screenshot defers shutdown; live scan must finish first.
 t={};Player j;j.screenshot_job=&j;j.request_shutdown();j.tick();assert(!j.shutdown_requested&&t.cutoff==0);
 t={};Player q;q.scan_job=&q;q.request_shutdown();idle(q,20);assert(t.cutoff==0&&t.settings_saves==0);q.scan_job=nullptr;q.tick();assert(t.cutoff==1);
 assert(!t.locked&&t.locks==t.unlocks);
 puts("PASS: extracted production shutdown flow; deferred lock/wake recovery, single error dialog, no automatic cutoff retry, explicit retry, saved-position preservation, failed save/favorites/unmount/blank/audio/artwork, unreadable persistence blocks favorite writes, wallpaper cancellation/completion barrier, motion disarm/join fault barrier, screenshot and scan barriers.");
}
'''
cpp=build/'shutdown_controller_test.cpp';cpp.write_text(harness,encoding='utf-8')
exe=build/'shutdown_controller_test.exe'
subprocess.run([args.zig,'c++','-std=c++17','-O1','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
