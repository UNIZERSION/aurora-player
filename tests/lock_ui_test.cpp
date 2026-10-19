#undef NDEBUG /* zig -O1/-O2 predefines NDEBUG; keep assert() checks active */
// Real production LVGL pixels and pointer dispatch. Security/state-machine
// tests live separately; this fixture verifies the public lock view contract.
#define main existing_preview_main
#include "aurora_ui_preview.cpp"
#undef main
static unsigned player_draws;
static void lock_tap(const char *caption,int expected,int value){
 auto *label=find_text(lv_screen_active(),caption);assert(label);auto *button=lv_obj_get_parent(label);
 assert(lv_obj_get_width(button)>=44&&lv_obj_get_height(button)>=44);
 lv_area_t box;lv_obj_get_coords(button,&box);tap((box.x1+box.x2)/2,(box.y1+box.y2)/2,expected);assert(values.back()==value);
}
static void masked(unsigned expected){
 auto *panel=lv_obj_get_child(lv_screen_active(),-1);assert(panel);auto *card=lv_obj_get_child(panel,-1);assert(card);unsigned filled=0;
 for(unsigned i=0;i<lv_obj_get_child_count(card);i++){
  auto *object=lv_obj_get_child(card,i);
  if(lv_obj_get_width(object)==18&&lv_obj_get_height(object)==18&&lv_color_eq(lv_obj_get_style_bg_color(object,0),lv_color_hex(0xe5f478)))++filled;
 }assert(filled==expected);
}
int main(int argc,char **argv){
 setvbuf(stdout,nullptr,_IONBF,0);target_rgb565=true;std::string dir=argc>1?argv[1]:".";
 lv_init();auto *display=lv_display_create(1280,720);lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);lv_display_set_buffers(display,buffer,nullptr,sizeof(buffer)/2,LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(display,flush);
 auto *input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(input,read_pointer);player_ui_create(lv_screen_active(),apply,nullptr);lv_obj_add_event_cb(player_ui_root(),[](lv_event_t*){++player_draws;},LV_EVENT_DRAW_MAIN,nullptr);
 state.title="Aurora Protocol";state.artist="Hikari Collective";state.album="Neon Tides";state.clock_text="14:28";state.firmware="1.3.13";state.speed_percent=100;state.volume=65;state.clock24=true;state.brightness=50;state.dragon_speed=10;state.animations=false;
 player_ui_track_t track{"Glass Horizon","Hikari Collective",200000,false,"/Music/test.mp3"};player_ui_set_playlist(&track,1,0);state.track_count=1;player_ui_set_state(&state);step();
 const auto baseline=std::vector<uint32_t>(pixels,pixels+1280*720);const auto root_count=objects(player_ui_root());
 save(dir+"/lock-main.ppm");FILE *capture=fopen((dir+"/lock-main.rgb565").c_str(),"wb");assert(capture);for(auto pixel:baseline){uint16_t native=uint16_t((((pixel>>16)&255)*31+127)/255<<11|(((pixel>>8)&255)*63+127)/255<<5|((pixel&255)*31+127)/255);assert(fwrite(&native,2,1,capture)==1);}fclose(capture);
 player_ui_open_display_settings();step();assert(find_text(player_ui_root(),"DISPLAY"));player_ui_set_lock_preferences(false,"");
 player_ui_prepare_lock_capture();step();assert(!find_text(player_ui_root(),"DISPLAY"));assert(std::equal(baseline.begin(),baseline.end(),pixels));
 // The first frame is opaque while its asynchronously blurred image loads.
 player_ui_show_lock_pin("Enter PIN","Enter your 4-digit PIN to unlock",0,false,true);step();masked(0);assert(player_ui_lock_panel_visible()&&!player_ui_keyboard_text_allowed());
 unsigned draws_before=player_draws;lv_obj_invalidate(player_ui_root());lv_refr_now(display);assert(player_draws==draws_before);
 assert((pixels[500*1280+1100]&0xffffff)!= (baseline[500*1280+1100]&0xffffff));save(dir+"/lock-loading.ppm");
 size_t before=actions.size();assert(player_ui_keyboard(LV_KEY_ESC));assert(player_ui_keyboard(LV_KEY_ENTER));assert(player_ui_keyboard('x'));assert(player_ui_keyboard(LV_KEY_DOWN));assert(actions.size()==before);
 player_ui_set_sleeping(true);step();for(auto pixel:pixels)assert((pixel&0xffffff)==0);assert(player_ui_keyboard('1'));assert(actions.size()==before);
 player_ui_set_sleeping(false);step();assert(find_text(lv_screen_active(),"Enter PIN"));assert(player_ui_lock_panel_visible());
 player_ui_set_lock_clock("14:29");step();assert(find_text(lv_screen_active(),"14:29"));
 // Real taps at every existing hardware control are swallowed by the backdrop.
 for(int id=0;id<24;id++){const auto &r=aurora_controls[id];if(r.x>=420&&r.x<860)continue;tap(r.x+r.w/2,r.y+r.h/2);assert(actions.size()==before);}
 assert(!find_text(lv_screen_active(),"Cancel"));
 // Updating one masked digit must redraw a small region, not the keypad or
 // wallpaper; an unchanged view must cause no display traffic at all.
 lv_refr_now(display);auto before_digits=flushed;
 player_ui_show_lock_pin("Enter PIN","Enter your 4-digit PIN to unlock",1,false,true);lv_refr_now(display);
 const auto changed_digit_pixels=flushed-before_digits;
 before_digits=flushed;player_ui_show_lock_pin("Enter PIN","Enter your 4-digit PIN to unlock",1,false,true);lv_refr_now(display);
 const auto unchanged_digit_pixels=flushed-before_digits;
 printf("PIN dirty-area pixels: changed=%llu unchanged=%llu\n",(unsigned long long)changed_digit_pixels,(unsigned long long)unchanged_digit_pixels);
 assert(changed_digit_pixels<=18*18&&unchanged_digit_pixels==0);
 player_ui_show_lock_pin("Enter PIN","Enter your 4-digit PIN to unlock",0,false,true);step();
 lock_tap("1",PLAYER_UI_LOCK_INPUT,1);player_ui_show_lock_pin("Enter PIN","Enter your 4-digit PIN to unlock",1,false,true);step();masked(1);assert(find_text(lv_screen_active(),"14:29"));
 lock_tap("5",PLAYER_UI_LOCK_INPUT,5);player_ui_show_lock_pin("Enter PIN","Enter your 4-digit PIN to unlock",2,false,true);step();masked(2);
 lock_tap("Delete",PLAYER_UI_LOCK_INPUT,-1);assert(player_ui_keyboard('9')&&values.back()==9);assert(player_ui_keyboard(LV_KEY_BACKSPACE)&&values.back()==-1);
 player_ui_show_lock_pin("Enter PIN","Incorrect PIN. Try again.",0,false,true);step();masked(0);save(dir+"/lock-wrong-pin.ppm");
 player_ui_show_lock_pin("Try again shortly","Too many attempts. Try again in 30 seconds.",0,false,false);step();before=actions.size();tap(525,280);assert(player_ui_keyboard('1'));assert(player_ui_keyboard(LV_KEY_BACKSPACE));assert(player_ui_keyboard(LV_KEY_ESC));assert(actions.size()==before);save(dir+"/lock-cooldown.ppm");
 // Source ownership stays with the caller and a native RGB565 image fills
 // the display without affecting the underlying player widgets or geometry.
 std::vector<uint16_t> background(1280*720);for(int y=0;y<720;y++)for(int x=0;x<1280;x++)background[y*1280+x]=uint16_t(((x*31/1279)<<11)|((y*63/719)<<5)|(31-x*31/1279));
 lv_image_dsc_t image{};image.header.magic=LV_IMAGE_HEADER_MAGIC;image.header.cf=LV_COLOR_FORMAT_RGB565;image.header.w=1280;image.header.h=720;image.header.stride=2560;image.data=(const uint8_t*)background.data();image.data_size=background.size()*2;
 player_ui_set_lock_background(&image);player_ui_show_lock_pin("Create PIN","Enter a new 4-digit PIN",3,true,true);step();masked(3);save(dir+"/lock-setup.ppm");
 lock_tap("Cancel",PLAYER_UI_LOCK_INPUT,-2);assert(player_ui_keyboard(LV_KEY_ESC)&&values.back()==-2);
 // Closing a panel during a held pointer cannot deliver its release to Play.
 const auto &play=aurora_controls[0];pointer={play.x+play.w/2,play.y+play.h/2};pressed=true;step(4);before=actions.size();player_ui_close_lock_panel();pressed=false;step(4);assert(actions.size()==before);
 assert(!player_ui_lock_panel_visible()&&player_ui_keyboard_text_allowed());assert(std::equal(baseline.begin(),baseline.end(),pixels));assert(objects(player_ui_root())==root_count);
 player_ui_set_lock_preferences(true,"Clouds.png");player_ui_open_display_settings();step();assert(find_text(player_ui_root(),"DISPLAY"));
 player_ui_show_lock_backgrounds(nullptr,0,-1,"SD card unavailable. Frosted glass is still available.");step();assert(find_text(lv_screen_active(),"No JPG or PNG files found in this folder."));lock_tap("Frosted glass (default)",PLAYER_UI_LOCK_BACKGROUND_SELECT,-1);
 char mutable_name[]="Clouds.png";const char *names[]={mutable_name,"Forest.jpg","Night.jpg"};player_ui_show_lock_backgrounds(names,3,0,nullptr);mutable_name[0]='X';step();assert(find_text(lv_screen_active(),"Clouds.png"));save(dir+"/lock-backgrounds.ppm");
 lock_tap("Forest.jpg",PLAYER_UI_LOCK_BACKGROUND_SELECT,1);lock_tap("Done",PLAYER_UI_LOCK_INPUT,-2);
 std::vector<std::string> many_names;std::vector<const char*> many;many_names.reserve(128);for(int i=0;i<128;i++)many_names.emplace_back("Wallpaper "+std::to_string(i)+".png");for(auto &name:many_names)many.push_back(name.c_str());
 player_ui_show_lock_backgrounds(many.data(),many.size(),-1,nullptr);assert(player_ui_keyboard(LV_KEY_END));assert(player_ui_keyboard(LV_KEY_ENTER));assert(actions.back()==PLAYER_UI_LOCK_BACKGROUND_SELECT&&values.back()==127);
 player_ui_close_lock_panel();player_ui_set_lock_background(nullptr);player_ui_open_display_settings();step();assert(find_text(player_ui_root(),"DISPLAY"));
 if(argc>2){std::vector<uint16_t> frosted(1280*720);FILE *file=fopen(argv[2],"rb");assert(file);assert(fread(frosted.data(),2,frosted.size(),file)==frosted.size());fclose(file);image.data=(const uint8_t*)frosted.data();player_ui_set_lock_background(&image);player_ui_prepare_lock_capture();player_ui_show_lock_pin("Enter PIN","Enter your 4-digit PIN to unlock",0,false,true);step();save(dir+"/lock-frosted.ppm");player_ui_close_lock_panel();player_ui_set_lock_background(nullptr);}
 lv_obj_delete(player_ui_root());assert(!player_ui_lock_panel_visible());
 puts("PASS: real LVGL PIN dots, 102x68 touch keys, wrong PIN/cooldown, blocked Escape/shortcuts/player touches, held-touch release isolation, exact main panel restoration, copied wallpaper names, default/empty selection and native RGB565 background.");
}
