"""Run the real controller's artwork scheduling methods with a deterministic UI/worker.

No device access. Checks view mapping, priority, gating and lock ownership.
"""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--zig', required=True)
parser.add_argument('--build', required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'main/main.cpp').read_text(encoding='utf-8-sig')
start = source.index('    void request_main_artwork(')
end = source.index('    int queue_index()', start)
methods = source[start:end]
build = Path(args.build).resolve()
build.mkdir(parents=True, exist_ok=True)
harness = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
struct Track { std::string path; };
struct Harness {
 bool locked=false,lock_fail=false,request_fail=false,retry_due=false;
 unsigned main_requests=0,batch_requests=0,locks=0,unlocks=0;
 std::string main_path;
 std::vector<std::string> batch,events;
 std::vector<size_t> visible{0,1};
} h;
void *dialog=nullptr;
bool tab5_display_lock(unsigned timeout){assert(timeout==2);assert(!h.locked);if(h.lock_fail)return false;h.locked=true;++h.locks;return true;}
void tab5_display_unlock(){assert(h.locked);h.locked=false;++h.unlocks;}
bool player_artwork_request(const char *path){assert(!h.locked);++h.main_requests;h.events.push_back("main");if(h.request_fail)return false;h.retry_due=false;h.main_path=path?path:"";return true;}
bool player_artwork_needs_retry(){return h.retry_due;}
bool player_artwork_request_thumbnails(const char *const *paths,size_t count){assert(!h.locked);assert(count<=8);++h.batch_requests;h.events.push_back("batch");h.batch.clear();for(size_t i=0;i<count;++i)h.batch.emplace_back(paths[i]);return true;}
size_t player_ui_get_missing_thumbnails(size_t *out,size_t capacity){assert(h.locked&&capacity==8);size_t n=0;for(auto i:h.visible){if(n==capacity)break;out[n++]=i;}return n;}
struct Player {
 struct {bool covers=true;} settings;
 struct {std::vector<Track> tracks{{"/Music/A.mp3"},{"/Music/B.flac"},{"/Music/C.aac"}};std::vector<size_t> visible{2,0,1};} library;
 bool mounted=true,dimmed=false,play_pending=false,resume_pending=false;
 void *screenshot_job=nullptr,*scan_job=nullptr;
 // Lock-screen controller state (main/aurora_lock_controller.inc): an active
 // lock flow/screen lock, or a pending lock backdrop job, owns SD/decoder I/O.
 bool lock_flow_active=false;
 void *lock_job=nullptr;
 bool lock_active() const {return lock_flow_active;}
 uint64_t last_artwork=0;
 std::string requested_artwork;
 int selected=0;
 const Track *track(){return selected<0?nullptr:&library.tracks[size_t(selected)];}
''' + methods + r'''
};
int main(){
 Player p;
 p.service_artwork(199);assert(!h.main_requests&&!h.batch_requests);
 p.service_artwork(200);assert(h.main_path=="/Music/A.mp3");
 assert((h.batch==std::vector<std::string>{"/Music/C.aac","/Music/A.mp3"}));
 assert((h.events==std::vector<std::string>{"main","batch"}));
 p.service_artwork(399);assert(h.batch_requests==1);
 p.service_artwork(400);assert(h.main_requests==1&&h.batch_requests==2);
 // A temporary decode failure may retry the same selection once the worker's
 // negative TTL expires. Successful images remain deduplicated indefinitely.
 h.retry_due=true;p.request_main_artwork("/Music/A.mp3");assert(h.main_requests==2&&!h.retry_due);
 p.request_main_artwork("/Music/A.mp3");assert(h.main_requests==2);
 // New filtered order is resolved under controller ownership, not by stale
 // pointers cached in UI rows; invalid indices never address the track array.
 h.visible={0,1,2,999};p.library.visible={1,999,2};p.selected=2;
 p.service_artwork(600);assert(h.main_path=="/Music/C.aac");
 assert((h.batch==std::vector<std::string>{"/Music/B.flac","/Music/C.aac"}));
 // Queue failure must permit a later retry; a lock failure must not be
 // interpreted as an empty viewport, discarding a valid in-flight batch.
 p.selected=1;h.request_fail=true;p.service_artwork(800);assert(p.requested_artwork=="/Music/C.aac");
 h.request_fail=false;h.lock_fail=true;auto batches=h.batch_requests;
 p.service_artwork(1000);assert(h.main_path=="/Music/B.flac"&&h.batch_requests==batches);
 h.lock_fail=false;
 auto main=h.main_requests;
 // Every gate (including the lock screen and its backdrop job) must clear the
 // batch and issue no main-cover I/O. The last mode leaves covers disabled.
 for(int mode=0;mode<11;++mode){
  p.mounted=mode!=0;p.settings.covers=mode!=1;p.dimmed=mode==2;dialog=mode==3?&p:nullptr;
  p.screenshot_job=mode==4?&p:nullptr;p.scan_job=mode==5?&p:nullptr;
  p.play_pending=mode==6;p.resume_pending=mode==7;
  p.lock_flow_active=mode==8;p.lock_job=mode==9?&p:nullptr;
  if(mode==10){p.mounted=true;p.settings.covers=false;}
  p.service_artwork(1200+200*mode);assert(h.batch.empty()&&h.main_requests==main);
 }
 p.request_main_artwork("/Music/B.flac");assert(h.main_path.empty());
 assert(p.requested_artwork.empty());
 p.settings.covers=true;p.mounted=true;p.dimmed=p.play_pending=p.resume_pending=false;
 p.screenshot_job=p.scan_job=dialog=nullptr;p.lock_flow_active=false;p.lock_job=nullptr;
 p.selected=-1;h.visible.clear();
 p.service_artwork(3600);assert(h.batch.empty()&&h.main_path.empty());
 assert(!h.locked&&h.locks==h.unlocks);
 puts("PASS: production artwork controller; eight-row mapping, 200ms cadence, current-cover priority, gates, failed-queue retry, lock failure and no worker I/O under UI lock.");
}
'''
(build / 'artwork-controller.cpp').write_text(harness)
exe = build / 'artwork-controller.exe'
subprocess.run([args.zig, 'c++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                str(build / 'artwork-controller.cpp'), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
