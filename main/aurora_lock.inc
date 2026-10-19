// The controller owns PIN verification and persistence. This view holds only
// the number of entered digits, and never stores the PIN itself.
namespace {
void lock_wait_release() {
 for(auto *input=lv_indev_get_next(nullptr);input;input=lv_indev_get_next(input))
  if(lv_indev_get_type(input)==LV_INDEV_TYPE_POINTER)lv_indev_wait_release(input);
}
void lock_delete_panel() {
 if(ui.lock_panel){lv_obj_delete(ui.lock_panel);ui.lock_panel=nullptr;}
 ui.lock_list=ui.lock_image=ui.lock_title=ui.lock_message=ui.lock_cancel=ui.lock_clock=nullptr;
 for(auto &dot:ui.lock_dots)dot=nullptr;
 for(auto &key:ui.lock_keys)key=nullptr;
 ui.lock_panel_kind=0;ui.lock_can_cancel=false;ui.lock_input_enabled=false;
}
void lock_apply_background() {
 if(!ui.lock_image)return;
 const auto *image=ui.lock_background;
 if(!image){visible(ui.lock_image,false);lv_image_set_src(ui.lock_image,nullptr);return;}
 // The background worker supplies native pixels with the static veil baked in.
 // Avoid full-screen software scaling/blending on every PIN redraw.
 lv_image_set_src(ui.lock_image,image);lv_image_set_scale(ui.lock_image,256);
 lv_image_set_antialias(ui.lock_image,false);lv_obj_set_pos(ui.lock_image,0,0);
 visible(ui.lock_image,true);
}
lv_obj_t *lock_create_panel(int kind,int x,int y,int width,int height) {
 lock_delete_panel();keyboard_clear();close_menu();lock_wait_release();ui.overlay=true;
 // A full opaque sibling on the active screen lets LVGL cull the player.
 // The black sleep cover alone lives on layer_top and always stays above us.
 ui.lock_panel_kind=kind;ui.lock_panel=plain(lv_obj_get_screen(ui.root),0,0,1280,720);solid(ui.lock_panel,0x090e0f);
 lv_obj_add_flag(ui.lock_panel,LV_OBJ_FLAG_CLICKABLE);
 ui.lock_image=lv_image_create(ui.lock_panel);lv_obj_remove_flag(ui.lock_image,LV_OBJ_FLAG_CLICKABLE);lock_apply_background();
 label(ui.lock_panel,"A U R O R A",36,31,250,&aurora_sans20,0xd6ddcd);
 ui.lock_clock=label(ui.lock_panel,ui.clock?lv_label_get_text(ui.clock):"",1080,31,160,&aurora_mono29,WHITE);lv_obj_set_style_text_align(ui.lock_clock,LV_TEXT_ALIGN_RIGHT,0);
 auto *card=plain(ui.lock_panel,x,y,width,height);solid(card,0x101718,25);lv_obj_set_style_bg_opa(card,235,0);
 lv_obj_set_style_border_width(card,1,0);lv_obj_set_style_border_color(card,color(0x88917b),0);lv_obj_set_style_border_opa(card,125,0);
 lv_obj_set_style_shadow_width(card,40,0);lv_obj_set_style_shadow_opa(card,100,0);lv_obj_set_style_shadow_color(card,color(0x000000),0);
 lv_obj_add_flag(card,LV_OBJ_FLAG_CLICKABLE);return card;
}
void lock_button_event(lv_event_t *event) {
 auto *control=static_cast<Control*>(lv_obj_get_user_data(lv_event_get_target_obj(event)));
 if(control->action==PLAYER_UI_LOCK_INPUT){
  if(control->value==-2){if(ui.lock_can_cancel)emit(PLAYER_UI_LOCK_INPUT,-2);}
  else if(ui.lock_input_enabled)emit(PLAYER_UI_LOCK_INPUT,control->value);
 }else if(ui.lock_panel_kind==2)emit(PLAYER_UI_LOCK_BACKGROUND_SELECT,control->value);
}
lv_obj_t *lock_button(lv_obj_t *parent,const char *name,int x,int y,int width,int height,player_ui_action_t action,int value,bool numeral=false) {
 auto *key=button(parent,x,y,width,height,action,NONE,PILL,name,value);
 lv_obj_remove_event_cb(key,control_click);lv_obj_add_event_cb(key,lock_button_event,LV_EVENT_CLICKED,nullptr);
 if(numeral){auto *caption=lv_obj_get_child(key,0);lv_obj_set_style_text_font(caption,&aurora_mono29,0);lv_obj_set_height(caption,lv_font_get_line_height(&aurora_mono29));lv_obj_center(caption);}
 lv_obj_set_style_opa(key,90,LV_STATE_DISABLED);return key;
}
}
extern "C" void player_ui_set_lock_preferences(bool enabled,const char *background_name) {
 ui.lock_enabled=enabled;ui.lock_background_name=background_name?background_name:"";
 if(ui.lock_enabled_button){
  static_cast<Control*>(lv_obj_get_user_data(ui.lock_enabled_button))->action=enabled?PLAYER_UI_LOCK_DISABLE:PLAYER_UI_LOCK_ENABLE;
  active(ui.lock_enabled_button,enabled);text(lv_obj_get_child(ui.lock_enabled_button,0),enabled?"ON":"OFF");
 }
 if(ui.lock_change_button){if(enabled)lv_obj_remove_state(ui.lock_change_button,LV_STATE_DISABLED);else lv_obj_add_state(ui.lock_change_button,LV_STATE_DISABLED);}
 std::string caption="Background: ";caption+=ui.lock_background_name.empty()?"Frosted glass":ui.lock_background_name; text(ui.lock_background_label,caption.c_str());
}
extern "C" void player_ui_show_lock_pin(const char *title,const char *message,unsigned digits,bool can_cancel,bool input_enabled) {
 if(!ui.root)return;
 if(!ui.lock_panel||ui.lock_panel_kind!=1){
  auto *card=lock_create_panel(1,420,64,440,592);
  ui.lock_title=label(card,"",30,29,380,&aurora_sans20,WHITE);lv_obj_set_style_text_align(ui.lock_title,LV_TEXT_ALIGN_CENTER,0);
  ui.lock_message=label(card,"",28,73,384,&aurora_sans16,MUTED);lv_obj_set_height(ui.lock_message,48);lv_label_set_long_mode(ui.lock_message,LV_LABEL_LONG_WRAP);lv_obj_set_style_text_align(ui.lock_message,LV_TEXT_ALIGN_CENTER,0);
  for(int i=0;i<4;i++){auto *dot=plain(card,154+i*38,132,18,18);solid(dot,0x1a2320,LV_RADIUS_CIRCLE);lv_obj_set_style_border_width(dot,2,0);lv_obj_set_style_border_color(dot,color(0x89927e),0);ui.lock_dots[i]=dot;}
  for(int i=0;i<9;i++){char digit[2]={char('1'+i),0};ui.lock_keys[i]=lock_button(card,digit,55+(i%3)*114,182+(i/3)*81,102,68,PLAYER_UI_LOCK_INPUT,i+1,true);}
  ui.lock_keys[9]=lock_button(card,"0",169,425,102,68,PLAYER_UI_LOCK_INPUT,0,true);
  ui.lock_keys[10]=lock_button(card,"Delete",283,425,102,68,PLAYER_UI_LOCK_INPUT,-1);
  ui.lock_cancel=lock_button(card,"Cancel",130,520,180,48,PLAYER_UI_LOCK_INPUT,-2);
 }
 ui.lock_can_cancel=can_cancel;ui.lock_input_enabled=input_enabled;ui.overlay=true;
 text(ui.lock_title,title&&*title?title:"Enter PIN");text(ui.lock_message,message&&*message?message:"Enter your 4-digit PIN to unlock");
 // LVGL invalidates a style even when the assigned value is unchanged. Touch
 // one dot only; radius/opacity and the other dots remain static while typing.
 for(unsigned i=0;i<4;i++){
  const auto fill=color(i<std::min(digits,4u)?LIME:0x1a2320),border=color(i<digits?LIME:0x89927e);
  if(!lv_color_eq(lv_obj_get_style_bg_color(ui.lock_dots[i],0),fill))lv_obj_set_style_bg_color(ui.lock_dots[i],fill,0);
  if(!lv_color_eq(lv_obj_get_style_border_color(ui.lock_dots[i],0),border))lv_obj_set_style_border_color(ui.lock_dots[i],border,0);
 }
 for(auto *key:ui.lock_keys)if(key){if(input_enabled)lv_obj_remove_state(key,LV_STATE_DISABLED);else lv_obj_add_state(key,LV_STATE_DISABLED);}
 visible(ui.lock_cancel,can_cancel);visible(ui.lock_panel,true);lv_obj_move_foreground(ui.lock_panel);
}
extern "C" void player_ui_show_lock_backgrounds(const char *const *names,size_t count,int selected,const char *status) {
 if(!ui.root)return;
 auto *card=lock_create_panel(2,245,70,790,580);ui.lock_can_cancel=true;
 label(card,"Lock background",32,28,610,&aurora_sans20,WHITE);
 lock_button(card,"Done",642,18,116,53,PLAYER_UI_LOCK_INPUT,-2);
 label(card,"SD / Aurora / Lockscreen",32,76,710,&aurora_sans18,MUTED);
 auto *help=label(card,status&&*status?status:"Choose a JPG or PNG, or keep the frosted player view.",32,112,710,&aurora_sans16,MUTED);lv_obj_set_height(help,44);lv_label_set_long_mode(help,LV_LABEL_LONG_WRAP);
 auto *list=plain(card,30,174,730,372);lv_obj_add_flag(list,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_scroll_dir(list,LV_DIR_VER);lv_obj_set_scrollbar_mode(list,LV_SCROLLBAR_MODE_ON);lv_obj_set_style_width(list,6,LV_PART_SCROLLBAR);lv_obj_set_style_bg_color(list,color(0xb7c78d),LV_PART_SCROLLBAR);lv_obj_set_style_bg_opa(list,255,LV_PART_SCROLLBAR);
 ui.lock_list=list;auto *fallback=lock_button(list,"Frosted glass (default)",0,0,708,58,PLAYER_UI_LOCK_BACKGROUND_SELECT,-1);active(fallback,selected<0);
 if(names)for(size_t i=0;i<count;i++){auto *row=lock_button(list,names[i]?names[i]:"",0,68+int(i)*68,708,58,PLAYER_UI_LOCK_BACKGROUND_SELECT,int(i));active(row,int(i)==selected);}
 if(!count){auto *empty=label(list,"No JPG or PNG files found in this folder.",14,84,680,&aurora_sans18,MUTED);lv_obj_set_height(empty,48);lv_label_set_long_mode(empty,LV_LABEL_LONG_WRAP);}
 add_scroll_handle(list,730,372);lv_obj_move_foreground(ui.lock_panel);
}
extern "C" void player_ui_close_lock_panel(void) {
 lock_wait_release();lock_delete_panel();ui.overlay=false;ui.dragon_tick=lv_tick_get();if(ui.root)lv_obj_invalidate(ui.root);
}
extern "C" bool player_ui_lock_panel_visible(void) { return ui.lock_panel!=nullptr; }
extern "C" void player_ui_set_lock_clock(const char *clock_text) { text(ui.lock_clock,clock_text); }
extern "C" void player_ui_open_display_settings(void) { if(ui.root){settings_page(2);settings_visible(true);} }
extern "C" void player_ui_prepare_lock_capture(void) {
 if(!ui.root)return;
 keyboard_clear();close_menu();settings_visible(false);ui.overlay=true;visible(ui.lock_panel,false);visible(ui.notice,false);
}
extern "C" void player_ui_set_lock_background(const lv_image_dsc_t *image) {
 if(image){
  const size_t stride=image->header.stride?image->header.stride:size_t(image->header.w)*2;
  if(image->header.cf!=LV_COLOR_FORMAT_RGB565||!image->data||image->header.w!=1280||image->header.h!=720||stride<size_t(image->header.w)*2||image->data_size<stride*image->header.h)return;
 }
 const auto *previous=ui.lock_background;ui.lock_background=image;lock_apply_background();
 if(previous&&previous!=image)lv_image_cache_drop(previous);
}
