#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include "eq_store.h"
#include "audio_level_policy.h"
#include "eq_presets.h"
#include "audio_fx.h"

static unsigned critical_depth=0, notices=0, audio_calls=0, reverb_calls=0, effect_calls=0;
static audio_eq_config_t audio_config{};
static audio_reverb_config_t audio_reverb{};
static uint8_t audio_effect=0;
using esp_err_t=int;
#define ESP_OK 0
using QueueHandle_t=void *;
#define pdTRUE 1
static QueueHandle_t commands=nullptr;
static std::atomic_bool lock_input_gate{false};
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
static void portENTER_CRITICAL(int *){assert(!critical_depth);++critical_depth;}
static void portEXIT_CRITICAL(int *){assert(critical_depth==1);--critical_depth;}
static void player_ui_set_notice(const char *){assert(!critical_depth);++notices;}
static int audio_engine_set_eq(const audio_eq_config_t *c){assert(!critical_depth);audio_config=*c;++audio_calls;return 0;}
static esp_err_t audio_engine_set_reverb(const audio_reverb_config_t *c){assert(!critical_depth);audio_reverb=*c;++reverb_calls;return ESP_OK;}
static esp_err_t audio_engine_set_effect(uint8_t e){assert(!critical_depth);audio_effect=e;++effect_calls;return ESP_OK;}
struct CommandStub {int action,value,fields[5];};
static unsigned queued_commands=0;static CommandStub last_command{};
template<class C> static int xQueueSend(QueueHandle_t,const C *c,int){
 assert(!critical_depth);++queued_commands;last_command={c->action,c->value,{c->fields[0],c->fields[1],c->fields[2],c->fields[3],c->fields[4]}};return pdTRUE;
}
static bool display_lock_ok=true;
static bool tab5_display_lock(int){assert(!critical_depth);return display_lock_ok;}
static void tab5_display_unlock(){}
static uint64_t now_ms(){return 123000;}
#include "eq_controller_globals.h"
static DialogKind dialog_kind=DIALOG_EQ;
using lv_obj_t=int;
static lv_obj_t *dialog=nullptr,*search_input=nullptr,*dialog_key_focus=nullptr;
static lv_obj_t *bt_status=nullptr,*bt_list=nullptr,*power_message=nullptr;
static void lv_obj_delete(lv_obj_t *){}
static void player_ui_set_overlay(bool){}
#include "eq_controller_close.h"
static audio_eq_config_t refreshed{};static unsigned refreshes=0;
static void eq_dialog_refresh(lv_obj_t *,const audio_eq_config_t &c,unsigned,uint32_t,const char *){
 assert(!critical_depth);refreshed=c;++refreshes;
}
static audio_reverb_config_t reverb_refreshed{};static unsigned reverb_refreshes=0;static bool reverb_refreshed_enabled=false;
static void reverb_dialog_refresh(lv_obj_t *,const audio_reverb_config_t &c,bool enabled,const char *){
 assert(!critical_depth);reverb_refreshed=c;reverb_refreshed_enabled=enabled;++reverb_refreshes;
}
enum {AUDIO_STOPPED,AUDIO_PLAYING,AUDIO_PAUSED,AUDIO_ENDED};

struct Settings {char last_path[240]{};uint32_t resume_ms=0;uint8_t effect=0;};
struct Track {std::string path;};
struct FakeSettingsStore {
 bool succeeds=true;unsigned calls=0;
 bool save(const Settings &){assert(!critical_depth);++calls;return succeeds;}
};
struct FakeEqStore {
 bool succeeds=true;unsigned calls=0;player::EqBank written;
 bool save(const player::EqBank &b){assert(!critical_depth);++calls;written=b;return succeeds;}
};
struct FakeReverbStore {
 bool succeeds=true;unsigned calls=0;audio_reverb_config_t written{};
 bool save(const audio_reverb_config_t &c){assert(!critical_depth);++calls;written=c;return succeeds;}
};
struct FakeStorageLayout {bool persistent_read_error=false;};
struct Player {
 player::EqBank eq_bank;
 FakeEqStore eq_store;FakeSettingsStore store;
 audio_reverb_config_t reverb_config=audio_reverb_default_config();
 FakeReverbStore reverb_store;
 bool reverb_dirty=false,reverb_refresh_pending=false;
 const char *reverb_status="Changes are saved automatically.";
 FakeStorageLayout storage_layout;bool persistence_loaded=true;
 bool eq_dirty=false,eq_refresh_pending=false,dirty=false,mounted=true,scan_job=false;
 uint64_t dirty_since=0,last_save=0,save_retry_after=0,last_state=55;
 const char *eq_status="";
 Settings settings;
 struct {std::string path;int state=AUDIO_STOPPED;uint32_t position_ms=0,sample_rate=44100;} audio;
 Track *track(){return nullptr;}
 void notice(const char *){assert(!critical_depth);++notices;}
 void mark_dirty(){dirty=true;dirty_since=now_ms();}
#include "eq_controller_methods.h"
};

static Player fresh(){
 eq_input_read=eq_input_count=0;critical_depth=notices=audio_calls=0;audio_config={};
 refreshed={};refreshes=0;reverb_calls=effect_calls=reverb_refreshes=queued_commands=0;
 audio_reverb={};audio_effect=0;reverb_refreshed={};reverb_refreshed_enabled=false;last_command={};
 lock_input_gate.store(false);
 dialog_kind=DIALOG_EQ;display_lock_ok=true;return {};
}
static void input(int kind,int index=-1,int value=0){eq_ui_action(kind,index,value);}
static bool same(const audio_eq_config_t &a,const audio_eq_config_t &b){return !memcmp(&a,&b,sizeof a);}

int main(){
 {
  auto p=fresh();
  for(int i=0;i<10000;i++)input(EQ_BAND,0,i%25-12);
  assert(eq_input_count==1);p.poll_eq_edits();
  assert(p.eq_bank.current.gain_db[0]==9999%25-12&&audio_calls==1&&p.eq_dirty);
  input(EQ_BAND,0,4);input(EQ_SAVE);input(EQ_BAND,0,11);p.poll_eq_edits();
  assert(p.eq_bank.current.gain_db[0]==11);
  assert(p.eq_bank.presets[0].gain_db[0]==4&&p.eq_store.written.current.gain_db[0]==4);
  assert(p.eq_bank.saved_mask==1&&p.eq_dirty);
  assert(same(audio_config,p.eq_bank.current));
  puts("PASS coalescing: 10000 drags become one pending edit; Save is an ordered click-time barrier.");
 }
 {
  auto p=fresh();p.eq_bank.current.gain_db[2]=-3;
  input(EQ_RESET);input(EQ_BAND,2,7);p.poll_eq_edits();
  assert(p.eq_bank.current.gain_db[2]==7);
  assert(same(refreshed,p.eq_bank.current));
  p.eq_bank.saved_mask=2;p.eq_bank.presets[1].gain_db[1]=-5;
  input(EQ_SELECT_SLOT,1);input(EQ_LOAD);input(EQ_BAND,1,9);p.poll_eq_edits();
  assert(p.eq_bank.selected==1&&p.eq_bank.current.gain_db[1]==9);
  assert(same(refreshed,p.eq_bank.current));
  input(EQ_SELECT_SLOT,3);input(EQ_PREAMP,-1,-12);input(EQ_SAVE);p.poll_eq_edits();
  assert(p.eq_bank.saved_mask==10&&p.eq_bank.presets[3].preamp_db==AUDIO_MASTER_GAIN_DB);
  assert(p.eq_bank.presets[1].gain_db[1]==-5);
  puts("PASS ordering: Flat/Load then drag preserves later value; Select/Save targets the chosen slot; final view agrees.");
 }
 {
  auto p=fresh();input(EQ_BAND,4,120);input(EQ_PREAMP,-1,-100);p.poll_eq_edits();
  assert(p.eq_bank.current.gain_db[4]==12&&p.eq_bank.current.preamp_db==AUDIO_MASTER_GAIN_DB);
  input(EQ_ENABLE);input(EQ_ENABLE);p.poll_eq_edits();assert(p.eq_bank.current.enabled==1);
  input(EQ_ENABLE);input(EQ_RESET);p.poll_eq_edits();assert(p.eq_bank.current.enabled==0);
  for(int v:p.eq_bank.current.gain_db)assert(v==0);
  assert(p.eq_bank.current.preamp_db==AUDIO_MASTER_GAIN_DB);
  auto before=p.eq_bank.current;input(EQ_LOAD);p.poll_eq_edits();assert(same(before,p.eq_bank.current));
  input(EQ_BAND,1,6);input(EQ_CLOSE);input(EQ_BAND,1,-6);p.poll_eq_edits();
  assert(dialog_kind==DIALOG_NONE&&p.eq_bank.current.gain_db[1]==6&&eq_input_count==0);
  dialog_kind=DIALOG_EQ;p.poll_eq_edits();assert(p.eq_bank.current.gain_db[1]==6);
  puts("PASS validation/lifecycle: clamps, on/off, Flat preserves bypass and the fixed base level, empty Load, close discards later edits.");
 }
 {
  auto p=fresh();p.eq_bank.current.preamp_db=-4;p.eq_bank.current.gain_db[1]=6;
  const auto legacy=p.eq_bank.current;
  input(EQ_HEADROOM);input(EQ_PREAMP,-1,0);p.poll_eq_edits();
  assert(same(legacy,p.eq_bank.current)&&audio_calls==0); // Retired events cannot change level.
  p.eq_bank.saved_mask=15;
  for(unsigned i=0;i<4;++i){p.eq_bank.presets[i].gain_db[i]=int(i)+1;p.eq_bank.presets[i].preamp_db=i?-24:0;}
  const auto saved=p.eq_bank;
  for(unsigned i=0;i<player::EQ_FACTORY_PRESET_COUNT;++i){
   p.eq_bank.current.enabled=0;
   input(EQ_FACTORY,int(i));p.poll_eq_edits();
   assert(player::eq_factory_match(p.eq_bank.current)==int(i));
   assert(p.eq_bank.current.enabled==1&&p.eq_bank.current.preamp_db==AUDIO_MASTER_GAIN_DB);
   assert(same(audio_config,p.eq_bank.current)&&same(refreshed,p.eq_bank.current));
   assert(p.eq_bank.saved_mask==15&&!memcmp(saved.presets,p.eq_bank.presets,sizeof saved.presets));
  }
  auto before=p.eq_bank.current;
  input(EQ_FACTORY,-1);input(EQ_FACTORY,player::EQ_FACTORY_PRESET_COUNT);p.poll_eq_edits();
  assert(same(before,p.eq_bank.current));
  p.eq_bank.current.preamp_db=0;
  input(EQ_FACTORY,1);input(EQ_SELECT_SLOT,2);input(EQ_SAVE);input(EQ_FACTORY,3);p.poll_eq_edits();
  assert(p.eq_bank.current.preamp_db==AUDIO_MASTER_GAIN_DB&&player::eq_factory_match(p.eq_bank.current)==3);
  assert(player::eq_factory_match(p.eq_bank.presets[2])==1);
  assert(p.eq_bank.presets[2].preamp_db==AUDIO_MASTER_GAIN_DB);
  input(EQ_LOAD);p.poll_eq_edits();assert(player::eq_factory_match(p.eq_bank.current)==1);
  // Older User records retain their legacy bytes until explicitly overwritten,
  // but loading their working curve always uses the common player's level.
  input(EQ_SELECT_SLOT,0);input(EQ_LOAD);p.poll_eq_edits();
  assert(p.eq_bank.current.gain_db[0]==1&&p.eq_bank.current.preamp_db==AUDIO_MASTER_GAIN_DB);
  assert(p.eq_bank.presets[0].preamp_db==0);
  input(EQ_ENABLE);p.poll_eq_edits();assert(p.eq_bank.current.preamp_db==AUDIO_MASTER_GAIN_DB);
  input(EQ_RESET);p.poll_eq_edits();assert(p.eq_bank.current.preamp_db==AUDIO_MASTER_GAIN_DB);
  puts("PASS fixed level/factory: all 8 curves, Flat, EQ ON/OFF and legacy Load use one base level; retired preamp controls do nothing; User files retained and Save ordering preserved.");
 }
 {
  auto p=fresh();for(int i=0;i<32;i++)input(EQ_ENABLE);
  input(EQ_RESET);assert(notices==1&&eq_input_count==32);
  p.poll_eq_edits();assert(eq_input_count==0&&p.eq_bank.current.enabled==1);
  for(int i=0;i<31;i++)input(EQ_ENABLE);
  input(EQ_BAND,0,1);input(EQ_BAND,0,8);
  assert(eq_input_count==32&&notices==1);p.poll_eq_edits();assert(p.eq_bank.current.gain_db[0]==8);
  assert(critical_depth==0);
  puts("PASS bounded ring: full queue rejects only new barrier with notice; last drag can still coalesce; I/O outside critical section.");
 }
 {
  auto p=fresh();input(EQ_RESET);display_lock_ok=false;p.poll_eq_edits();
  assert(p.eq_refresh_pending&&refreshes==0);
  display_lock_ok=true;input(EQ_BAND,0,9);p.eq_refresh();
  assert(p.eq_refresh_pending&&refreshes==0&&eq_input_count==1);
  p.poll_eq_edits();assert(!p.eq_refresh_pending&&refreshes==1&&refreshed.gain_db[0]==9);
  input(EQ_BAND,1,10);close_dialog();assert(eq_input_count==0);
  dialog_kind=DIALOG_EQ;p.poll_eq_edits();assert(p.eq_bank.current.gain_db[1]==0);
  puts("PASS refresh/reopen: display-lock failure retries, queued newer input prevents stale refresh, external close purges ring.");
 }
 {
  auto p=fresh();p.eq_store.succeeds=false;
  input(EQ_BAND,1,3);input(EQ_SAVE);p.poll_eq_edits();
  assert(p.eq_dirty&&p.dirty&&p.save_retry_after==now_ms()+10000&&p.eq_store.calls==1);
  p.eq_store.succeeds=true;p.save();assert(!p.eq_dirty&&!p.dirty&&p.save_retry_after==0);
  unsigned calls=p.eq_store.calls;p.save();assert(p.eq_store.calls==calls);
  input(EQ_BAND,1,4);p.poll_eq_edits();p.store.succeeds=false;p.save();
  assert(!p.eq_dirty&&p.dirty);calls=p.eq_store.calls;p.store.succeeds=true;p.save();
  assert(!p.dirty&&p.eq_store.calls==calls);
  p.mounted=false;input(EQ_BAND,0,-5);input(EQ_SAVE);p.poll_eq_edits();
  assert(p.eq_dirty&&p.eq_store.calls==calls);
  p.mounted=true;p.scan_job=true;p.save();assert(p.eq_dirty&&p.eq_store.calls==calls);
  p.scan_job=false;p.save();assert(!p.eq_dirty&&p.eq_store.written.current.gain_db[0]==-5);
  puts("PASS save/restore state: EQ write failure retained dirty, settings-only retry avoids rewriting EQ, absent card/scan defers save.");
 }
 {
  auto p=fresh();
  lock_input_gate.store(true);input(EQ_BAND,0,5);reverb_ui_action(REVERB_AMOUNT,10);airpods_ui_action(1,2);
  assert(eq_input_count==0&&queued_commands==0&&notices==0);
  lock_input_gate.store(false);airpods_ui_action(3,7);
  assert(queued_commands==1&&last_command.action==AIRPODS_ACTION&&last_command.value==3&&last_command.fields[0]==7);
  input(EQ_BAND,0,5);p.poll_eq_edits();assert(p.eq_bank.current.gain_db[0]==5);
  puts("PASS lock gate: locked screen drops EQ, Reverb and AirPods input before it reaches the ring or queue.");
 }
 {
  auto p=fresh();dialog_kind=DIALOG_REVERB;
  const auto eq_before=p.eq_bank.current;
  for(int i=0;i<100;i++)reverb_ui_action(REVERB_AMOUNT,i);
  assert(eq_input_count==1);
  reverb_ui_action(REVERB_ENABLE,1);input(EQ_BAND,0,5);input(EQ_RESET);
  p.poll_eq_edits();
  assert(eq_input_count==0&&p.reverb_config.amount==60&&same(audio_config,audio_eq_config_t{})&&audio_calls==0);
  assert(!memcmp(&eq_before,&p.eq_bank.current,sizeof eq_before)); // EQ events are inert in the Reverb dialog.
  assert(p.settings.effect==2&&audio_effect==2&&p.reverb_dirty&&p.dirty&&audio_reverb.amount==60);
  assert(!p.reverb_refresh_pending&&reverb_refreshes==1&&reverb_refreshed.amount==60&&reverb_refreshed_enabled);
  p.reverb_store.succeeds=false;p.save();
  assert(p.reverb_dirty&&p.dirty&&p.save_retry_after==now_ms()+10000&&p.reverb_store.calls==1&&p.reverb_refresh_pending);
  p.reverb_store.succeeds=true;p.save();
  assert(!p.reverb_dirty&&!p.dirty&&p.save_retry_after==0&&p.reverb_store.written.amount==60);
  unsigned calls=p.reverb_store.calls;p.save();assert(p.reverb_store.calls==calls);
  reverb_ui_action(REVERB_RESET,0);p.poll_eq_edits();
  const auto def=audio_reverb_default_config();
  assert(def.amount!=60&&p.reverb_config.amount==def.amount&&p.reverb_config.decay_ms==def.decay_ms);
  assert(p.reverb_config.tone_hz==def.tone_hz&&p.reverb_config.low_cut_hz==def.low_cut_hz&&p.reverb_dirty);
  reverb_ui_action(REVERB_CLOSE,0);reverb_ui_action(REVERB_AMOUNT,10);p.poll_eq_edits();
  assert(dialog_kind==DIALOG_NONE&&eq_input_count==0&&p.reverb_config.amount==def.amount);
  puts("PASS reverb ring: drags coalesce, EQ events ignored, reverb write failure retained dirty, Reset restores defaults, Close purges later input.");
 }
 return 0;
}
