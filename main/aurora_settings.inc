// Included in player_ui.cpp's namespace: every preference shares the same
// touch geometry and styling, without a second legacy settings window.
void settings_page(int page) {
 ui.settings_page=std::clamp(page,0,6);
 static const char *titles[]={"SETTINGS","DRAGON","DISPLAY","AUDIO","LIBRARY","CLOCK","BATTERY & POWER"};
 if(!ui.settings_heading)return;
 text(ui.settings_heading,titles[ui.settings_page]);
 if(page)lv_obj_remove_flag(ui.settings_back,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(ui.settings_back,LV_OBJ_FLAG_HIDDEN);
 for(int i=0;i<7;i++){if(i==ui.settings_page)lv_obj_remove_flag(ui.settings_pages[i],LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(ui.settings_pages[i],LV_OBJ_FLAG_HIDDEN);}
}
void create_settings() {
 ui.settings=plain(ui.root,888,188,352,326);solid(ui.settings,BLACK);lv_obj_add_flag(ui.settings,LV_OBJ_FLAG_HIDDEN);
 ui.settings_heading=label(ui.settings,"SETTINGS",0,13,237,&aurora_sans18,WHITE);
 ui.settings_back=setting_button(ui.settings,"Back",253,0,84,PLAYER_UI_SETTINGS,0);
 for(auto &page:ui.settings_pages){page=plain(ui.settings,0,61,350,265);solid(page,BLACK);lv_obj_add_flag(page,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_scroll_dir(page,LV_DIR_VER);lv_obj_set_scrollbar_mode(page,LV_SCROLLBAR_MODE_ON);lv_obj_set_style_width(page,6,LV_PART_SCROLLBAR);lv_obj_set_style_bg_color(page,color(0xb7c78d),LV_PART_SCROLLBAR);lv_obj_set_style_bg_opa(page,255,LV_PART_SCROLLBAR);lv_obj_add_event_cb(page,[](lv_event_t*){ui.interaction_tick=lv_tick_get();},LV_EVENT_SCROLL,nullptr);}
 auto *p=ui.settings_pages[0];
 const char *names[]={"Dragon","Display","Audio","Library","Clock","Battery / Power"};
 for(int i=0;i<6;i++)setting_button(p,names[i],(i%2)*174,(i/2)*70,163,PLAYER_UI_SETTINGS,i+1);

 p=ui.settings_pages[1];
 ui.dragon_types[0]=setting_button(p,"Living dragon",0,0,163,PLAYER_UI_DRAGON_TYPE,0);ui.dragon_types[1]=setting_button(p,"Flight",174,0,163,PLAYER_UI_DRAGON_TYPE,1);
 label(p,"Speed",0,77,225,&aurora_sans18);ui.speed_value=label(p,"1.0x",270,77,67,&aurora_mono19,LIME);ui.dragon_speed=setting_slider(p,121,4,16);
 label(p,"Shifting colors",0,177,245,&aurora_sans18);ui.dragon_colors=setting_button(p,"ON",258,163,79,PLAYER_UI_DRAGON_COLORS);
 p=ui.settings_pages[2];
 label(p,"Brightness",0,0,240,&aurora_sans18);ui.brightness_value=label(p,"100%",270,0,67,&aurora_mono19,LIME);ui.brightness=setting_slider(p,45,10,100);
 label(p,"Animations",0,104,244,&aurora_sans18);ui.animations=setting_button(p,"ON",258,89,79,PLAYER_UI_ANIMATIONS);
 label(p,"Screen lock",0,169,244,&aurora_sans18);ui.lock_enabled_button=setting_button(p,"OFF",258,154,79,PLAYER_UI_LOCK_ENABLE);
 label(p,"Require a 4-digit PIN after screen off",0,212,337,&aurora_sans16,MUTED);
 ui.lock_change_button=setting_button(p,"Change PIN",0,250,163,PLAYER_UI_LOCK_CHANGE);lv_obj_add_state(ui.lock_change_button,LV_STATE_DISABLED);lv_obj_set_style_opa(ui.lock_change_button,100,LV_STATE_DISABLED);
 setting_button(p,"Lock background",174,250,163,PLAYER_UI_LOCK_BACKGROUND);
 ui.lock_background_label=label(p,"Background: Frosted glass",0,322,337,&aurora_sans16,MUTED);lv_obj_set_height(ui.lock_background_label,48);lv_label_set_long_mode(ui.lock_background_label,LV_LABEL_LONG_WRAP);
 label(p,"Screen off after",0,399,337,&aurora_sans18);
 const char *dims[]={"Never","30 sec","2 min","5 min"};const int times[]={0,30,120,300};
 for(int i=0;i<4;i++)ui.dim_buttons[i]=setting_button(p,dims[i],(i%2)*174,434+(i/2)*65,163,PLAYER_UI_DIM_TIME,times[i]);
 setting_button(p,"Reset display and dragon",0,575,337,PLAYER_UI_RESET_VISUAL);
 p=ui.settings_pages[3];
 ui.jack_status=label(p,"Output: speaker",0,0,337,&aurora_sans18);
 label(p,"Jack inserted = speaker off",0,32,337,&aurora_sans16,MUTED);
 label(p,"Turntable speed",0,80,235,&aurora_sans18);ui.track_speed_value=label(p,"100%",257,80,80,&aurora_mono19,LIME);
 ui.track_speed=setting_slider(p,128,75,125);
 setting_button(p,"Reset to 100%",0,170,337,PLAYER_UI_TRACK_SPEED,100);
 label(p,"Speed changes pitch",0,238,337,&aurora_sans16,MUTED);
 setting_button(p,"Reverb settings",0,282,337,PLAYER_UI_REVERB_SETTINGS);
 p=ui.settings_pages[4];
 setting_button(p,"Search",0,0,163,PLAYER_UI_SEARCH);setting_button(p,"Update library",174,0,163,PLAYER_UI_RESCAN);
 label(p,"Sort by",0,76,337,&aurora_sans18);
 const char *sorts[]={"Path","Title","Duration"};for(int i=0;i<3;i++)ui.sort_buttons[i]=setting_button(p,sorts[i],i*115,112,107,PLAYER_UI_SORT_SET,i);
 label(p,"Cover artwork",0,198,239,&aurora_sans18);ui.cover_toggle=setting_button(p,"ON",258,183,79,PLAYER_UI_COVERS);
 p=ui.settings_pages[5];
 label(p,"Clock format",0,16,240,&aurora_sans18);ui.clock_format=setting_button(p,"24 h",258,0,79,PLAYER_UI_CLOCK_FORMAT);
 setting_button(p,"Set date and time",0,81,337,PLAYER_UI_SET_CLOCK);
 label(p,"Saved in the device clock",0,166,337,&aurora_sans16,MUTED);
 p=ui.settings_pages[6];
 ui.power_level=label(p,"Battery: --",0,0,337,&aurora_mono25,LIME);
 ui.power_details=label(p,"",0,43,337,&aurora_sans16);lv_obj_set_height(ui.power_details,84);lv_label_set_long_mode(ui.power_details,LV_LABEL_LONG_WRAP);
 setting_button(p,"Save settings",0,147,337,PLAYER_UI_SAVE);
 auto *off=setting_button(p,"Power off",0,213,337,PLAYER_UI_POWER_OFF);ink(lv_obj_get_child(off,0),0xffb1a0);
 for(auto *page:ui.settings_pages)add_scroll_handle(page,350,265);
 settings_page(0);
 player_ui_set_lock_preferences(ui.lock_enabled,ui.lock_background_name.c_str());
}
void update_settings_state() {
 const auto &s=ui.state;char speed[12];unsigned percent=s.speed_percent?s.speed_percent:100;snprintf(speed,sizeof speed,"%u%%",percent);text(ui.track_speed_value,speed);if(!lv_obj_has_state(ui.track_speed,LV_STATE_PRESSED))lv_slider_set_value(ui.track_speed,percent,LV_ANIM_OFF);const int times[]={0,30,120,300};
 for(int i=0;i<4;i++)active(ui.dim_buttons[i],s.dim_seconds==unsigned(times[i]));
 for(int i=0;i<3;i++)active(ui.sort_buttons[i],s.sort==i);
 active(ui.cover_toggle,s.covers);text(lv_obj_get_child(ui.cover_toggle,0),s.covers?"ON":"OFF");
 text(ui.jack_status,s.bluetooth_connected?"Output: Bluetooth":!s.headphones_known?"Jack: checking...":s.headphones?"Output: headphones":"Output: speaker");
 char b[150];if(s.battery_percent>=0)snprintf(b,sizeof b,"Battery: %d%%",std::clamp<int>(s.battery_percent,0,100));else snprintf(b,sizeof b,"Battery: --");text(ui.power_level,b);
 if(s.battery_percent>=0){
  char current[32];if(s.battery_charging_known)snprintf(current,sizeof current,"%+d mA",s.battery_ma);else snprintf(current,sizeof current,"Current unavailable");
  snprintf(b,sizeof b,"%.2f V  /  %s\n%s\nSmoothed voltage estimate",s.battery_mv/1000.,current,!s.battery_charging_known?"Charge status unavailable":s.battery_charging?"Charging":"Not charging");
 }else snprintf(b,sizeof b,"Battery reading unavailable");text(ui.power_details,b);
}
