#undef NDEBUG /* zig -O1/-O2 predefines NDEBUG; keep assert() checks active */
#define main existing_preview_main
#include "aurora_ui_preview.cpp"
#undef main
#include "aurora_eq_dialog.hpp"
#include "audio_level_policy.h"
#include "../components/tab5_hal/include/tab5_keyboard_protocol.h"
#include <deque>
static lv_obj_t *dialog=nullptr,*search_input=nullptr,*dialog_key_focus=nullptr;
static lv_obj_t *bt_status=nullptr,*bt_list=nullptr,*power_message=nullptr;
static int dialog_kind=0;static bool dialog_closable=true;
// The production close helper now clears its controller queue. The queue's
// ordering/concurrency has a separate extracted-controller test.
static int eq_input_mux=0,eq_input_read=0,eq_input_count=0;
static void portENTER_CRITICAL(int*){}
static void portEXIT_CRITICAL(int*){}
static std::vector<int> dialog_commands;
static void enqueue(int action,int=0){dialog_commands.push_back(action);}
#include "eq_dialog_helpers.h"
struct EqEvent {int kind,index,value;};static std::vector<EqEvent> eq_events;
static audio_eq_config_t config{};static unsigned slot=0;
static audio_eq_config_t presets[4]{};static uint32_t sample_rate=44100;
static void changed(int kind,int index,int value){
 eq_events.push_back({kind,index,value});
 switch(kind){
  case EQ_BAND:assert(index>=0&&index<11&&value>=-12&&value<=12);config.gain_db[index]=value;break;
  case EQ_PREAMP:case EQ_HEADROOM:assert(false);break; // Retired controls never emit callbacks.
  case EQ_ENABLE:config.enabled=value;break;
  case EQ_SELECT_SLOT:assert(index>=0&&index<4);slot=index;break;
  case EQ_RESET:for(auto &gain:config.gain_db)gain=0;break;
  case EQ_FACTORY:assert(index>=0&&index<int(player::EQ_FACTORY_PRESET_COUNT));memcpy(config.gain_db,player::eq_factory_presets[index].gain_db,sizeof config.gain_db);config.preamp_db=AUDIO_MASTER_GAIN_DB;config.enabled=1;break;
  case EQ_SAVE:presets[slot]=config;break;
  case EQ_LOAD:config=presets[slot];config.preamp_db=AUDIO_MASTER_GAIN_DB;break;
  case EQ_CLOSE:close_dialog();return;
  default:assert(false);
 }
 eq_dialog_refresh(dialog,config,slot,sample_rate,"User preset ready. Changes are applied live.");
}
static void open_eq(){auto *p=new_dialog("11-BAND EQUALIZER",false);dialog_kind=DIALOG_EQ;dialog_closable=true;eq_dialog_build(p,config,slot,sample_rate,"Adjust bands, then save to a User slot.",changed);step();}
static void dialog_key(uint32_t key,uint8_t modifiers=0,bool repeat=false){dialog_keyboard({key,modifiers,repeat});step();}
static void tap_eq_text(const char *s){auto *o=find_text(dialog,s);assert(o);lv_area_t a;lv_obj_get_coords(o,&a);tap((a.x1+a.x2)/2,(a.y1+a.y2)/2);}
static void tap_factory_option(unsigned option,const std::string &screenshot={}){
 auto *ui=aurora_eq_detail::state(dialog);lv_area_t area;lv_obj_get_coords(ui->factory,&area);tap((area.x1+area.x2)/2,(area.y1+area.y2)/2);assert(lv_dropdown_is_open(ui->factory));
 auto *list=lv_dropdown_get_list(ui->factory);lv_obj_scroll_to_y(list,0,LV_ANIM_OFF);step();auto *label=lv_obj_get_child(list,0);assert(label);lv_area_t text_area;lv_obj_get_coords(label,&text_area);lv_obj_get_coords(list,&area);
 const int font_height=lv_font_get_line_height(lv_obj_get_style_text_font(label,0));const int pitch=font_height+lv_obj_get_style_text_line_space(label,0);assert(pitch>=44&&lv_area_get_width(&area)==232&&area.y1>=0&&area.y2<720);
 if(!screenshot.empty())save(screenshot);
 const int y=text_area.y1+int(option)*pitch+font_height/2;assert(y>area.y1&&y<area.y2);tap((area.x1+area.x2)/2,y);assert(!lv_dropdown_is_open(ui->factory));
}
static void redraw_check(){lv_refr_now(lv_display_get_default());auto p=std::vector<uint32_t>(pixels,pixels+1280*720);lv_obj_invalidate(lv_screen_active());lv_refr_now(lv_display_get_default());assert(std::equal(p.begin(),p.end(),pixels));}
int main(int argc,char **argv){
 setvbuf(stdout,nullptr,_IONBF,0);target_rgb565=true;std::string dir=argc>1?argv[1]:".";
 lv_init();auto*d=lv_display_create(1280,720);lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);lv_display_set_buffers(d,buffer,nullptr,sizeof(buffer)/2,LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(d,flush);
 auto *input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(input,read_pointer);player_ui_create(lv_screen_active(),apply,nullptr);
 const char*names[]={"Aurora Protocol","Midnight Circuit","Glass Ocean","Binary Bloom","Solar Drift","Echo Chamber"};const char*artists[]={"Hikari Collective","Lo-Fi Assembly","Nexis","Studio Kumo","Yugen","Vector Fields"};const char*paths[]={"/sdcard/Music/aurora.mp3","/sdcard/Music/midnight.mp3","/sdcard/Music/glass.wav","/sdcard/Music/binary.flac","/sdcard/Music/solar.aac","/sdcard/Music/echo.mp3"};player_ui_track_t rows[6]{};for(int i=0;i<6;i++)rows[i]={names[i],artists[i],uint32_t(276000-i*17000),false,paths[i]};
 state.title=names[0];state.artist=artists[0];state.album="Neon Tides";state.clock_text="14:28";state.firmware="1.3.11";state.speed_percent=100;state.position_ms=137000;state.duration_ms=276000;state.sample_rate=44100;state.bitrate_kbps=320;state.format="MP3";state.track_count=6;state.volume=65;state.battery_percent=78;state.battery_charging=true;state.bluetooth_available=true;state.dragon_type=1;state.dragon_speed=10;state.dragon_colors=true;state.clock24=true;state.brightness=100;state.animations=true;state.covers=true;
 player_ui_set_playlist(rows,6,0);player_ui_set_state(&state);float bands[56];int16_t wave[128];for(int i=0;i<56;i++)bands[i]=.17f+.68f*fabsf(sinf(i*.32f))*fabsf(cosf(i*.095f));for(int i=0;i<128;i++)wave[i]=int16_t(28000*sinf(i*.13f));player_ui_set_spectrum(bands,56,wave,128);step(8);player_ui_set_spectrum(bands,56,wave,128);step();save(dir+"/aurora-main-eq.ppm");
 state.animations=false;player_ui_set_state(&state);step();config.enabled=1;config.preamp_db=AUDIO_MASTER_GAIN_DB;open_eq();save(dir+"/eq-flat.ppm");auto *ui=aurora_eq_detail::state(dialog);assert(ui&&ui->binding_count==21);
 static_assert(EQ_CLOSE==7&&EQ_HEADROOM==8&&EQ_FACTORY==9,"Existing callback IDs must remain stable");
 lv_font_glyph_dsc_t arrow{};assert(lv_font_get_glyph_dsc(lv_obj_get_style_text_font(ui->factory,LV_PART_INDICATOR),&arrow,0xf078,0));
 assert(!find_text(dialog,"Headroom")&&!find_text(dialog,"PREAMP"));assert(lv_obj_get_x(ui->factory)==780&&lv_obj_get_y(ui->factory)==12&&lv_obj_get_width(ui->factory)==232);assert(lv_obj_get_width(lv_obj_get_child(dialog,0))==740);
 const auto widget_count=objects(dialog);lv_area_t panel;lv_obj_get_coords(dialog,&panel);assert(lv_area_get_width(&panel)==1180&&lv_area_get_height(&panel)==630&&panel.x1==50&&panel.y1==45);
 // Each slider must accept a drag 32 px to the side of its eight-pixel rail.
 // The underlying LVGL widget, not a synthetic VALUE_CHANGED event, emits gain.
 for(int i=0;i<11;i++){
  lv_area_t a;lv_obj_get_coords(ui->sliders[i],&a);size_t before=eq_events.size();pointer={(a.x1+a.x2)/2+32,(a.y1+a.y2)/2};pressed=true;step();pointer.y-=60;step();assert(eq_events.size()>before&&eq_events.back().kind==EQ_BAND&&eq_events.back().index==i);assert(config.gain_db[i]>0);
  int held=lv_slider_get_value(ui->sliders[i]);audio_eq_config_t stale{};stale.enabled=1;eq_dialog_refresh(dialog,stale,slot,sample_rate,"Incoming snapshot while dragging");assert(lv_slider_get_value(ui->sliders[i])==held);
  pressed=false;step();eq_dialog_refresh(dialog,config,slot,sample_rate,"Edited curve");
 }
 assert(objects(dialog)==widget_count);redraw_check();
 lv_area_t a;
 // Range edges are real top/bottom touches. No neighboring lane may change.
 lv_obj_get_coords(ui->sliders[3],&a);tap((a.x1+a.x2)/2,a.y1+1);assert(config.gain_db[3]==12);tap((a.x1+a.x2)/2,a.y2-1);assert(config.gain_db[3]==-12);
 for(int i=0;i<11;i++)config.gain_db[i]=int8_t((i%5)-2);config.preamp_db=AUDIO_MASTER_GAIN_DB;eq_dialog_refresh(dialog,config,slot,sample_rate,"User 1 selected. Save stores the current curve.");step();save(dir+"/eq-custom.ppm");
 tap_eq_text("User 3");assert(slot==2&&eq_events.back().kind==EQ_SELECT_SLOT);auto saved=config;tap_eq_text("Save");assert(eq_events.back().kind==EQ_SAVE);tap_eq_text("Flat");assert(eq_events.back().kind==EQ_RESET&&config.preamp_db==AUDIO_MASTER_GAIN_DB);for(int gain:config.gain_db)assert(gain==0);tap_eq_text("Load");assert(eq_events.back().kind==EQ_LOAD&&!memcmp(&saved,&config,sizeof config));
 puts("PASS: no PREAMP or Headroom widgets; 11 real 76px slider hit areas; common player gain policy, Flat bands only, four User actions and 21 bindings.");
 // Factory curves apply immediately through real dropdown touch events and do
 // not overwrite any of the four User memories or their selected slot.
 const auto user_presets=std::vector<audio_eq_config_t>(presets,presets+4);const unsigned user_slot=slot;assert(player::eq_factory_match(config)==-1&&lv_dropdown_get_selected(ui->factory)==0);
 for(unsigned preset=0;preset<player::EQ_FACTORY_PRESET_COUNT;preset++){
  const auto before=eq_events.size();tap_factory_option(preset+1,preset==0?dir+"/eq-factory-list.ppm":"");assert(eq_events.size()==before+1&&eq_events.back().kind==EQ_FACTORY&&eq_events.back().index==int(preset));assert(!memcmp(config.gain_db,player::eq_factory_presets[preset].gain_db,sizeof config.gain_db)&&config.preamp_db==AUDIO_MASTER_GAIN_DB);assert(lv_dropdown_get_selected(ui->factory)==preset+1&&slot==user_slot&&!memcmp(presets,user_presets.data(),sizeof presets));
 }
 auto before_placeholder=eq_events.size();tap_factory_option(0);assert(eq_events.size()==before_placeholder&&lv_dropdown_get_selected(ui->factory)==player::EQ_FACTORY_PRESET_COUNT);
 config.preamp_db=AUDIO_MASTER_GAIN_DB;eq_dialog_refresh(dialog,config,slot,sample_rate,"Fixed output level");step();tap_factory_option(2);assert(config.preamp_db==AUDIO_MASTER_GAIN_DB);save(dir+"/eq-factory-bass.ppm");
 config.gain_db[7]=12;eq_dialog_refresh(dialog,config,slot,sample_rate,"Custom curve");assert(player::eq_factory_match(config)==-1&&lv_dropdown_get_selected(ui->factory)==0);tap_eq_text("Load");assert(!memcmp(&saved,&config,sizeof config));assert(!memcmp(presets,user_presets.data(),sizeof presets));
 puts("PASS: all eight factory presets through real 44px dropdown rows; immediate callback and curve selection; Custom/Presets match, placeholder no-op, fixed base gain, four User memories and selected User slot unchanged.");
 // The actual production keyboard dispatcher operates these real widgets.
 // List navigation previews its choice; only Enter applies, Escape cancels.
 dialog_key_focus=nullptr;size_t keyboard_events=eq_events.size();dialog_key(TAB5_KEY_ENTER);assert(dialog_key_focus==ui->factory&&!lv_dropdown_is_open(ui->factory)&&eq_events.size()==keyboard_events);dialog_key(TAB5_KEY_ENTER,0,true);assert(!lv_dropdown_is_open(ui->factory));
 dialog_key(TAB5_KEY_ENTER);assert(lv_dropdown_is_open(ui->factory));dialog_key(TAB5_KEY_DOWN);dialog_key(TAB5_KEY_DOWN);assert(lv_dropdown_get_selected(ui->factory)==2&&eq_events.size()==keyboard_events);dialog_key(TAB5_KEY_ENTER);assert(!lv_dropdown_is_open(ui->factory)&&eq_events.size()==keyboard_events+1&&eq_events.back().kind==EQ_FACTORY&&eq_events.back().index==1);assert(!memcmp(config.gain_db,player::eq_factory_presets[1].gain_db,sizeof config.gain_db));
 // Re-selecting a recognized curve must behave exactly like tapping that row,
 // even when its gains match but EQ has been switched off.
 config.enabled=0;config.preamp_db=AUDIO_MASTER_GAIN_DB;eq_dialog_refresh(dialog,config,slot,sample_rate,"EQ bands bypassed");step();assert(lv_dropdown_get_selected(ui->factory)==2);keyboard_events=eq_events.size();dialog_key(TAB5_KEY_ENTER);dialog_key(TAB5_KEY_ENTER,0,true);assert(eq_events.size()==keyboard_events&&!config.enabled&&config.preamp_db==AUDIO_MASTER_GAIN_DB&&lv_dropdown_is_open(ui->factory));dialog_key(TAB5_KEY_ENTER);assert(eq_events.size()==keyboard_events+1&&eq_events.back().kind==EQ_FACTORY&&eq_events.back().index==1&&config.enabled&&config.preamp_db==AUDIO_MASTER_GAIN_DB&&!lv_dropdown_is_open(ui->factory));dialog_key(TAB5_KEY_ENTER,0,true);assert(eq_events.size()==keyboard_events+1&&!lv_dropdown_is_open(ui->factory));
 const auto confirmed=config;keyboard_events=eq_events.size();dialog_key(TAB5_KEY_ENTER);dialog_key(TAB5_KEY_RIGHT);assert(lv_dropdown_get_selected(ui->factory)==3);dialog_key(TAB5_KEY_ESC);assert(!lv_dropdown_is_open(ui->factory)&&lv_dropdown_get_selected(ui->factory)==2&&eq_events.size()==keyboard_events&&!memcmp(&config,&confirmed,sizeof config)&&dialog_commands.empty());dialog_key(TAB5_KEY_ESC,0,true);assert(dialog_commands.empty());
 dialog_key(TAB5_KEY_ENTER);dialog_key(TAB5_KEY_UP);dialog_key(TAB5_KEY_TAB);assert(!lv_dropdown_is_open(ui->factory)&&dialog_key_focus==ui->enable&&lv_dropdown_get_selected(ui->factory)==2&&eq_events.size()==keyboard_events);dialog_key(TAB5_KEY_TAB,TAB5_KEY_MOD_SHIFT);assert(dialog_key_focus==ui->factory);dialog_key(TAB5_KEY_ENTER);dialog_key(TAB5_KEY_ENTER,0,true);assert(lv_dropdown_is_open(ui->factory));dialog_key(TAB5_KEY_ESC);dialog_key(TAB5_KEY_ESC);assert(dialog_commands.size()==1&&dialog_commands.back()==CLOSE);dialog_commands.clear();
 dialog_key(TAB5_KEY_TAB);dialog_key(TAB5_KEY_TAB);assert(dialog_key_focus==ui->sliders[0]);const int previous_gain=config.gain_db[0];dialog_key(TAB5_KEY_RIGHT);assert(config.gain_db[0]==previous_gain+1&&eq_events.back().kind==EQ_BAND);tap_eq_text("Load");assert(!memcmp(&saved,&config,sizeof config));
 puts("PASS: extracted production keyboard dispatcher with real LVGL dropdown focus, Enter open/confirm and same-preset reapply (EQ ON) exactly once, arrows preview, Escape cancel before modal close, held Enter/Escape suppression, Tab/Shift+Tab, and slider editing.");
 tap_eq_text("EQ ON");assert(!config.enabled&&eq_events.back().kind==EQ_ENABLE);tap_eq_text("EQ OFF");assert(config.enabled);
 size_t events=eq_events.size();sample_rate=32000;eq_dialog_refresh(dialog,config,slot,sample_rate,"User 3 loaded");step();assert(eq_events.size()==events);assert(lv_color_eq(lv_obj_get_style_text_color(ui->frequencies[9],0),lv_color_hex(0x747d77)));assert(lv_color_eq(lv_obj_get_style_text_color(ui->frequencies[10],0),lv_color_hex(0x747d77)));assert(!lv_color_eq(lv_obj_get_style_text_color(ui->frequencies[8],0),lv_color_hex(0x747d77)));save(dir+"/eq-32khz.ppm");redraw_check();
 sample_rate=44100;eq_dialog_refresh(dialog,config,slot,sample_rate,"Ready");assert(lv_color_eq(lv_obj_get_style_text_color(ui->frequencies[10],0),lv_color_hex(aurora_eq_detail::white)));
 step(12);const uint64_t idle_pixels=flushed;for(int i=0;i<8;i++){eq_dialog_refresh(dialog,config,slot,sample_rate,"Ready");step();}assert(flushed==idle_pixels);
 // Live reduction redraws only its small label, without touching config or
 // emitting a control action. Identical meter samples must paint zero pixels.
 const auto meter_config=config;events=eq_events.size();eq_dialog_meter(dialog,0,true);step();
 assert(!strcmp(lv_label_get_text(ui->limit),"LIMIT  0.0 dB"));
 auto before_meter=std::vector<uint32_t>(pixels,pixels+1280*720);const uint64_t meter_pixels=flushed;lv_area_t meter_area;lv_obj_get_coords(ui->limit,&meter_area);
 eq_dialog_meter(dialog,6.5f,true);step();assert(!strcmp(lv_label_get_text(ui->limit),"LIMIT  -6.5 dB"));assert(lv_color_eq(lv_obj_get_style_text_color(ui->limit,0),lv_color_hex(0xffb169)));
 assert(flushed>meter_pixels&&flushed-meter_pixels<100000);
 for(int y=0;y<720;y++)for(int x=0;x<1280;x++)if(x<meter_area.x1||x>meter_area.x2||y<meter_area.y1||y>meter_area.y2)assert(pixels[y*1280+x]==before_meter[y*1280+x]);
 assert(!memcmp(&config,&meter_config,sizeof config)&&eq_events.size()==events);save(dir+"/eq-limit-active.ppm");
 const uint64_t held_meter_pixels=flushed;for(int i=0;i<8;i++){eq_dialog_meter(dialog,6.5f,true);step();}assert(flushed==held_meter_pixels);
 eq_dialog_meter(dialog,0.4f,true);assert(lv_color_eq(lv_obj_get_style_text_color(ui->limit,0),lv_color_hex(aurora_eq_detail::muted)));eq_dialog_meter(dialog,0.5f,true);assert(lv_color_eq(lv_obj_get_style_text_color(ui->limit,0),lv_color_hex(0xffb169)));
 eq_dialog_meter(dialog,6.5f,false);assert(!strcmp(lv_label_get_text(ui->limit),"LIMIT  0.0 dB"));
 // The output limiter can remain in its hold/release tail after EQ is switched
 // off. Configuration refresh must preserve the actual measured attenuation.
 eq_dialog_meter(dialog,6.5f,true);config.enabled=0;eq_dialog_refresh(dialog,config,slot,sample_rate,"EQ bypassed");assert(!strcmp(lv_label_get_text(ui->limit),"LIMIT  -6.5 dB"));
 eq_dialog_meter(dialog,4.2f,true);assert(!strcmp(lv_label_get_text(ui->limit),"LIMIT  -4.2 dB"));eq_dialog_meter(dialog,0,true);assert(!strcmp(lv_label_get_text(ui->limit),"LIMIT  0.0 dB"));
 eq_dialog_meter(dialog,6.5f,false);assert(!strcmp(lv_label_get_text(ui->limit),"LIMIT  0.0 dB"));config.enabled=1;eq_dialog_refresh(dialog,config,slot,sample_rate,"Ready");
 for(float bad:{NAN,INFINITY,-1.f}){eq_dialog_meter(dialog,bad,true);assert(!strcmp(lv_label_get_text(ui->limit),"LIMIT  0.0 dB"));}
 eq_dialog_meter(dialog,1000.f,true);assert(!strcmp(lv_label_get_text(ui->limit),"LIMIT  -60.0 dB"));eq_dialog_meter(dialog,0,true);step();redraw_check();eq_dialog_meter(nullptr,1,true);
 puts("PASS: LIMIT meter0/6.5dB, threshold colors, inactive clearing, real EQ-OFF limiter release6.5/4.2/0dB, finite/clamped input, bounded label-only redraw and eight unchanged meter samples with zero painted pixels.");
 tap_eq_text("Close");assert(!dialog&&eq_events.back().kind==EQ_CLOSE);
 // Parent ownership: repeated creation/deletion never leaves stale bindings.
 for(int i=0;i<12;i++){open_eq();assert(objects(dialog)==widget_count);close_dialog();}step();
 puts("PASS: 11 real vertical sliders with 76px hit targets, signed gain limits and no PREAMP control, stale refresh while held, 4 preset slots/actions, bypass switch, rate availability, no refresh callbacks, bounded widgets and repeated dialog cleanup.");
 lv_obj_delete(player_ui_root());return 0;
}
