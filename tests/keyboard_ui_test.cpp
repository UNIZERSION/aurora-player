#undef NDEBUG /* zig -O1/-O2 predefines NDEBUG; keep assert() checks active */
// Reuse the production LVGL pixel/touch harness, with an independent keyboard
// scenario. The old preview remains a separate regression test.
#define main existing_preview_main
#include "aurora_ui_preview.cpp"
#undef main
static void keypress(uint32_t key){assert(player_ui_keyboard(key));step();no_trails();}
static void selection(player_ui_action_t action,int index){size_t before=actions.size();keypress(LV_KEY_ENTER);assert(actions.size()==before+1&&actions.back()==action&&values.back()==index);}
int main(int argc,char **argv){
 setvbuf(stdout,nullptr,_IONBF,0);target_rgb565=true;std::string dir=argc>1?argv[1]:".";
 lv_init();auto *display=lv_display_create(1280,720);lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);lv_display_set_buffers(display,buffer,nullptr,sizeof(buffer)/2,LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(display,flush);
 auto *input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(input,read_pointer);player_ui_create(lv_screen_active(),apply,nullptr);
 state.title="Aurora Protocol";state.artist="Hikari Collective";state.album="Neon Tides";state.clock_text="14:28";state.firmware="1.3.10";state.speed_percent=100;state.volume=65;state.clock24=true;state.brightness=50;state.dragon_speed=10;state.animations=false;
 player_ui_track_t track{"Keyboard test track","Artist",200000,false,"/Music/test.mp3"};std::vector<player_ui_track_t> tracks(10000,track);
 player_ui_set_playlist(tracks.data(),tracks.size(),0);state.track_count=10000;player_ui_set_state(&state);step();const auto count=objects(player_ui_root());
 // Focus itself cannot start playback. Escape restores the exact approved UI.
 auto unchanged=std::vector<uint32_t>(pixels,pixels+1280*720);size_t before=actions.size();keypress(LV_KEY_HOME);assert(actions.size()==before);assert(!std::equal(unchanged.begin(),unchanged.end(),pixels));save(dir+"/keyboard-queue.ppm");keypress(LV_KEY_ESC);assert(std::equal(unchanged.begin(),unchanged.end(),pixels));assert(!player_ui_keyboard(LV_KEY_ESC));
 keypress(LV_KEY_DOWN);selection(PLAYER_UI_SELECT_TRACK,1);keypress(PLAYER_UI_KEY_PAGE_DOWN);selection(PLAYER_UI_SELECT_TRACK,5);keypress(PLAYER_UI_KEY_PAGE_UP);selection(PLAYER_UI_SELECT_TRACK,1);
 keypress(LV_KEY_END);selection(PLAYER_UI_SELECT_TRACK,9999);keypress(LV_KEY_DOWN);selection(PLAYER_UI_SELECT_TRACK,9999);keypress(LV_KEY_HOME);keypress(LV_KEY_UP);selection(PLAYER_UI_SELECT_TRACK,0);assert(objects(player_ui_root())==count);
 assert(!player_ui_keyboard(LV_KEY_NEXT)&&!player_ui_keyboard(LV_KEY_PREV)&&!player_ui_keyboard('x')&&player_ui_keyboard_text_allowed());
 state.library_tab=2;player_ui_set_state(&state);keypress(LV_KEY_END);selection(PLAYER_UI_SELECT_TRACK,9999);assert(objects(player_ui_root())==count);
 // Replacing/filtering a model cannot retain a stale row index.
 auto *prepared=player_ui_prepare_playlist(&track,1);player_ui_apply_playlist(prepared,0);player_ui_free_playlist(prepared);state.track_count=1;state.track_index=0;keypress(LV_KEY_ENTER);assert(values.back()==0);
 player_ui_set_playlist(nullptr,0,SIZE_MAX);before=actions.size();for(auto key:{LV_KEY_UP,LV_KEY_DOWN,LV_KEY_ENTER,LV_KEY_HOME,LV_KEY_END})keypress(key);assert(actions.size()==before);
 std::vector<std::string> names;std::vector<const char*> folders;names.reserve(10000);for(int i=0;i<10000;i++)names.emplace_back("Music / Collection "+std::to_string(i));for(auto &name:names)folders.push_back(name.c_str());
 auto *model=player_ui_prepare_folders(folders.data(),folders.size());player_ui_apply_folders(model,0);player_ui_free_folders(model);state.library_tab=1;player_ui_set_state(&state);step();assert(objects(player_ui_root())==count);
 keypress(LV_KEY_DOWN);selection(PLAYER_UI_FOLDER_SELECT,1);keypress(PLAYER_UI_KEY_PAGE_DOWN);selection(PLAYER_UI_FOLDER_SELECT,7);keypress(PLAYER_UI_KEY_PAGE_UP);selection(PLAYER_UI_FOLDER_SELECT,1);keypress(LV_KEY_END);selection(PLAYER_UI_FOLDER_SELECT,9999);save(dir+"/keyboard-files.ppm");assert(objects(player_ui_root())==count);
 model=player_ui_prepare_folders(folders.data(),2);player_ui_apply_folders(model,0);player_ui_free_folders(model);keypress(LV_KEY_ENTER);assert(values.back()==0);keypress(LV_KEY_END);selection(PLAYER_UI_FOLDER_SELECT,1);
 // Touch row activation clears keyboard decoration without adding a key action.
 tap(990,210,PLAYER_UI_FOLDER_SELECT);assert(!player_ui_keyboard(LV_KEY_ESC));
 model=player_ui_prepare_folders(nullptr,0);player_ui_apply_folders(model,SIZE_MAX);player_ui_free_folders(model);before=actions.size();keypress(LV_KEY_ENTER);keypress(LV_KEY_END);assert(actions.size()==before);
 puts("PASS: 10,000 Queue/Favorites/Files rows; bounded objects, Enter, paging, boundaries, model replacement, empty lists, touch clears focus, exact restoration.");
 // Settings: ordered focus, category activation, sliders, hidden controls and Esc.
 player_ui_open_settings();step();assert(!player_ui_keyboard_text_allowed());keypress(LV_KEY_HOME);keypress(LV_KEY_NEXT);keypress(LV_KEY_ENTER);assert(find_text(player_ui_root(),"DISPLAY"));
 keypress(LV_KEY_RIGHT);assert(state.brightness==55);keypress(LV_KEY_LEFT);assert(state.brightness==50);save(dir+"/keyboard-settings.ppm");
 keypress(LV_KEY_END);assert(find_text(player_ui_root(),"Reset display and dragon"));keypress(LV_KEY_PREV);keypress(LV_KEY_ENTER);assert(actions.back()==PLAYER_UI_DIM_TIME&&values.back()==300);
 keypress(LV_KEY_ESC);assert(find_text(player_ui_root(),"SETTINGS"));keypress(LV_KEY_ESC);assert(player_ui_keyboard_text_allowed());
 // Screen-off and controller-owned modal input must not reach library rows.
 before=actions.size();player_ui_set_overlay(true);assert(!player_ui_keyboard(LV_KEY_ENTER)&&!player_ui_keyboard(LV_KEY_ESC)&&!player_ui_keyboard_text_allowed());player_ui_set_overlay(false);
 player_ui_set_sleeping(true);assert(!player_ui_keyboard(LV_KEY_ENTER)&&!player_ui_keyboard_text_allowed());player_ui_set_sleeping(false);assert(actions.size()==before);
 puts("PASS: settings focus, slider 5% increments, offscreen controls, submenu Escape, text gating, sleeping/modal input isolation.");
 lv_obj_delete(player_ui_root());return 0;
}
