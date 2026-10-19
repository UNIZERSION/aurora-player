"""Fault-test the actual sleep-tap controller extracted from main.cpp.

The motion detector and worker have separate tests. This harness verifies the
authorization boundary and explicit audio commands without hardware access.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--zig', required=True)
p.add_argument('--build', type=Path, required=True)
args = p.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'main/main.cpp').read_text(encoding='utf-8-sig')
build = args.build.resolve()
build.mkdir(parents=True, exist_ok=True)


def method(name):
    found = re.search(r'(?m)^    void ' + name + r'\([^\n]*\)\s*\{', source)
    assert found, name
    end = source.index('\n    }', found.end()) + len('\n    }')
    return source[found.start():end]


controller = method('poll_sleep_taps')
# No wake, UI activation or generic toggle is allowed in the tap path.
for prohibited in ('tab5_display_sleep(', 'lock_finish(', 'lock_show_pin_ui(',
                   'PLAYER_UI_PLAY_PAUSE', 'audio_engine_play('):
    assert prohibited not in controller, prohibited
assert 'player::sleep_tap_arm(false);' in method('screen_power')
assert 'player::sleep_tap_arm(false);' in method('keyboard_event')
assert 'player::sleep_tap_arm(false);' in method('request_shutdown')
run = source[source.index('    void run()'):]
assert run.index('screen_power();') < run.index('poll_sleep_taps();')

harness = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "tap_gesture.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=1;
enum AudioState {AUDIO_STOPPED,AUDIO_LOADING,AUDIO_PLAYING,AUDIO_PAUSED,AUDIO_SEEKING,AUDIO_ENDED,AUDIO_ERROR};
struct audio_snapshot_t {AudioState state=AUDIO_PLAYING;};
static const char *TAG="tap-controller-test";
struct Test {
 bool locked=false,sleeping=true,wake=false,armed=false;
 bool route_ok=true,route_wake=false,route_unsleep=false;
 int route_state=-1,route_pause_pending=-1,route_resume_pending=-1;
 unsigned lock_calls=0,unlock_calls=0,fail_lock_at=0,snapshots=0;
 unsigned route_calls=0,activity_calls=0,manual_calls=0,dirty_calls=0,logs=0,takes=0,playing_calls=0;
 bool playing_flag=false;
 uint64_t now=1000;
 player::TapGesture gesture=player::TapGesture::None;
 AudioState live_state=AUDIO_PLAYING;
 esp_err_t transport_result=ESP_OK;
 std::vector<bool> commands;
 std::vector<bool> arm_history;
} t;
uint64_t now_ms(){return t.now;}
bool tab5_display_lock(unsigned wait){assert(wait==2&&!t.locked);++t.lock_calls;if(t.fail_lock_at==t.lock_calls)return false;t.locked=true;return true;}
void tab5_display_unlock(){assert(t.locked);t.locked=false;++t.unlock_calls;}
bool tab5_display_is_sleeping(){assert(t.locked);return t.sleeping;}
bool tab5_display_wake_requested(){assert(t.locked);return t.wake;}
namespace player {
 void sleep_tap_arm(bool enabled){assert(!t.locked);t.armed=enabled;t.arm_history.push_back(enabled);}
 void sleep_tap_set_playing(bool playing){assert(!t.locked);t.playing_flag=playing;++t.playing_calls;}
 TapGesture sleep_tap_take(uint32_t at){assert(!t.locked&&at==uint32_t(t.now));++t.takes;const auto value=t.gesture;t.gesture=TapGesture::None;return value;}
}
void audio_engine_get_snapshot(audio_snapshot_t *snapshot){assert(!t.locked);++t.snapshots;snapshot->state=t.live_state;}
void bluetooth_bridge_set_activity(bool active,bool settings){assert(!t.locked&&active&&!settings);++t.activity_calls;}
esp_err_t audio_engine_pause(bool pause){assert(!t.locked&&t.sleeping&&!t.wake);t.commands.push_back(pause);return t.transport_result;}
const char *esp_err_to_name(int){return "test fault";}
template<class... Args> void log_context(Args...){assert(!t.locked);++t.logs;}
#define ESP_LOGI(...) log_context(__VA_ARGS__)
#define ESP_LOGW(...) log_context(__VA_ARGS__)
struct Lock {
 bool configured=true,is_locked=true,error=false;
 bool enabled()const{return configured;}
 bool locked()const{return is_locked;}
 bool storage_error()const{return error;}
};
struct Airpods {void manual(){assert(!t.locked);++t.manual_calls;}};
struct Player {
 bool dimmed=true,audio_ready=true,shutdown_requested=false;
 bool play_pending=false,resume_pending=false,bt_ready=true;
 Lock screen_lock;
 Airpods airpods_playback;
 audio_snapshot_t audio{};
 bool update_audio_route(){
  assert(!t.locked);++t.route_calls;
  if(t.route_wake)t.wake=true;
  if(t.route_unsleep)t.sleeping=false;
  if(t.route_state>=0)t.live_state=AudioState(t.route_state);
  if(t.route_pause_pending>=0)play_pending=bool(t.route_pause_pending);
  if(t.route_resume_pending>=0)resume_pending=bool(t.route_resume_pending);
  return t.route_ok;
 }
 void mark_dirty(){assert(!t.locked);++t.dirty_calls;}
''' + controller + r'''
};
Player reset(player::TapGesture gesture=player::TapGesture::Triple,AudioState state=AUDIO_PLAYING){t={};t.gesture=gesture;t.live_state=state;return Player{};}
void no_transport(const Player &p){(void)p;assert(t.commands.empty()&&t.activity_calls==0&&t.manual_calls==0&&t.dirty_calls==0&&!t.locked);}
int main(){
 using G=player::TapGesture;
 // Explicit, idempotent commands, leaving PIN/screen untouched.
 auto p=reset();p.poll_sleep_taps();assert(t.commands==std::vector<bool>{true});
 assert(t.route_calls==0&&t.activity_calls==0&&t.manual_calls==1&&t.dirty_calls==1);
 assert(p.dimmed&&p.screen_lock.locked()&&t.sleeping&&!t.wake&&!p.resume_pending);
 p=reset(G::Double,AUDIO_PAUSED);p.poll_sleep_taps();assert(t.commands==std::vector<bool>{false});
 assert(p.resume_pending&&t.route_calls==1&&t.activity_calls==1&&t.manual_calls==1&&t.dirty_calls==1);
 assert(p.dimmed&&p.screen_lock.locked()&&t.sleeping&&!t.wake);
 p=reset(G::Double,AUDIO_PAUSED);p.bt_ready=false;p.poll_sleep_taps();assert(t.commands==std::vector<bool>{false}&&p.resume_pending&&t.route_calls==0&&t.activity_calls==0);
 puts("PASS explicit triple pause/double resume, local and Bluetooth, screen and PIN unchanged.");
 // The worker learns on every poll whether music plays (a pause then needs one strong tap).
 p=reset(G::None);p.audio.state=AUDIO_PLAYING;p.poll_sleep_taps();assert(t.playing_flag&&t.playing_calls==1);
 p=reset(G::None);p.audio.state=AUDIO_PAUSED;p.poll_sleep_taps();assert(!t.playing_flag&&t.playing_calls==1);
 p=reset(G::None);p.dimmed=false;p.audio.state=AUDIO_PLAYING;p.poll_sleep_taps();assert(t.playing_flag&&!t.armed);
 puts("PASS playing state reaches the tap worker on every poll.");
 // Every policy gate is independently required, even if a queued gesture leaks through.
 for(unsigned gate=0;gate<8;gate++){
  p=reset();switch(gate){
   case 0:p.dimmed=false;break;case 1:p.screen_lock.configured=false;break;
   case 2:p.screen_lock.is_locked=false;break;case 3:p.screen_lock.error=true;break;
   case 4:p.audio_ready=false;break;case 5:p.shutdown_requested=true;break;
   case 6:t.sleeping=false;break;case 7:t.wake=true;break;
  }
  p.poll_sleep_taps();no_transport(p);assert(!t.armed&&t.snapshots==0&&t.takes==1);
 }
 p=reset(G::None);p.poll_sleep_taps();no_transport(p);assert(t.snapshots==0&&t.armed);
 puts("PASS gate matrix: physical OFF, PIN configured+locked, valid storage, initialized audio, no shutdown; pending touch wins.");
 // State must be refreshed; cached controller state cannot authorize transport.
 for(auto g:{G::Double,G::Triple})for(unsigned s=AUDIO_STOPPED;s<=AUDIO_ERROR;s++){
  if((g==G::Double&&s==AUDIO_PAUSED)||(g==G::Triple&&s==AUDIO_PLAYING))continue;
  p=reset(g,AudioState(s));p.audio.state=g==G::Double?AUDIO_PAUSED:AUDIO_PLAYING;
  p.poll_sleep_taps();no_transport(p);assert(t.snapshots>=1);
 }
 for(bool pending_resume:{false,true}){
  p=reset(G::Double,AUDIO_PAUSED);p.resume_pending=pending_resume;p.play_pending=!pending_resume;
  p.poll_sleep_taps();no_transport(p);
 }
 // A successfully queued resume must not enqueue another while snapshot is stale PAUSED.
 p=reset(G::Double,AUDIO_PAUSED);p.poll_sleep_taps();t.gesture=G::Double;p.poll_sleep_taps();assert(t.commands.size()==1&&p.resume_pending);
 puts("PASS stale snapshot, wrong audio state, stopped/ended/error never start, pending asynchronous play/resume suppress duplicates.");
 // Busy route/display and wake races discard a gesture; never retry or unlock.
 p=reset(G::Double,AUDIO_PAUSED);t.route_ok=false;p.poll_sleep_taps();no_transport(p);assert(!p.resume_pending&&t.route_calls==1);
 for(unsigned fail:{1U,2U}){p=reset();t.fail_lock_at=fail;p.poll_sleep_taps();no_transport(p);}
 for(bool wake:{false,true}){
  p=reset(G::Double,AUDIO_PAUSED);t.route_wake=wake;t.route_unsleep=!wake;
  p.poll_sleep_taps();no_transport(p);assert(!t.armed&&!p.resume_pending);
 }
 // Route switching can wait for decoder acknowledgement; recheck the live snapshot.
 for(auto s:{AUDIO_STOPPED,AUDIO_LOADING,AUDIO_PLAYING,AUDIO_SEEKING,AUDIO_ENDED,AUDIO_ERROR}){
  p=reset(G::Double,AUDIO_PAUSED);t.route_state=s;p.poll_sleep_taps();no_transport(p);assert(!p.resume_pending);
 }
 for(bool pending_resume:{false,true}){
  p=reset(G::Double,AUDIO_PAUSED);if(pending_resume)t.route_resume_pending=1;else t.route_pause_pending=1;
  p.poll_sleep_taps();no_transport(p);
 }
 puts("PASS display lock timeout, touch during route wait, route failure/state transition/pending command; no delayed retry.");
 // Failed transport must not pretend a resume succeeded or dirty persistence.
 p=reset(G::Double,AUDIO_PAUSED);t.transport_result=ESP_FAIL;p.poll_sleep_taps();
 assert(t.commands.size()==1&&!p.resume_pending&&t.dirty_calls==0&&t.logs==1);
 p.poll_sleep_taps();assert(t.commands.size()==1&&t.dirty_calls==0);
 p=reset();t.transport_result=ESP_FAIL;p.poll_sleep_taps();assert(t.commands.size()==1&&!p.resume_pending&&t.dirty_calls==0);
 assert(!t.locked&&t.lock_calls==t.unlock_calls);
 puts("PASS transport queue failure leaves no retry, no successful-resume flag or persistence update.");
 puts("PASS production wake/shutdown hooks and controller ordering checked directly from source. No physical tap sensitivity claimed.");
}
'''
cpp = build / 'tap_controller_test.cpp'
cpp.write_text(harness, encoding='utf-8')
exe = build / 'tap_controller_test.exe'
subprocess.run([args.zig, 'c++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                '-I', str(root / 'main'), str(cpp), '-o', str(exe)], check=True)
result = subprocess.run([str(exe)], text=True, capture_output=True)
(build / 'results.log').write_text(result.stdout + result.stderr, encoding='utf-8')
print(result.stdout + result.stderr, end='')
result.check_returncode()
files = [root / 'main/main.cpp', root / 'main/tap_gesture.h', Path(__file__)]
(build / 'results.json').write_text(json.dumps({
    'result': 'PASS', 'scope': 'Extracted production sleep-tap transport controller with deterministic faults',
    'limitations': ['No physical IMU sensitivity, bus timing, or audio hardware validation.'],
    'sha256': {str(f.relative_to(root)): hashlib.sha256(f.read_bytes()).hexdigest() for f in files}
}, indent=2) + '\n', encoding='utf-8')
