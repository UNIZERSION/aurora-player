// Keyboard focus is independent of the playing track. Only the eight existing
// row objects draw its outline; navigating a large library never builds widgets.
namespace {
bool keyboard_navigation(uint32_t key) {
 return key==LV_KEY_UP||key==LV_KEY_DOWN||key==LV_KEY_HOME||key==LV_KEY_END||key==LV_KEY_ENTER||key==PLAYER_UI_KEY_PAGE_UP||key==PLAYER_UI_KEY_PAGE_DOWN;
}
void keyboard_reveal(lv_obj_t *list,size_t row,int pitch,int height) {
 lv_anim_delete(list,nullptr);lv_obj_update_layout(list);
 int top=int(row)*pitch,scroll=std::max(0,int(lv_obj_get_scroll_y(list))),viewport=lv_obj_get_height(list);
 if(top<scroll)scroll=top;else if(top+height>scroll+viewport)scroll=top+height-viewport;
 lv_obj_scroll_to_y(list,scroll,LV_ANIM_OFF);
}
size_t keyboard_index(uint32_t key,size_t current,size_t count,size_t page) {
 if(!count)return SIZE_MAX;
 if(key==LV_KEY_HOME)return 0;
 if(key==LV_KEY_END)return count-1;
 if(current>=count)return 0;
 if(key==LV_KEY_UP)return current?current-1:0;
 if(key==LV_KEY_DOWN)return std::min(count-1,current+1);
 if(key==PLAYER_UI_KEY_PAGE_UP)return current>page?current-page:0;
 if(key==PLAYER_UI_KEY_PAGE_DOWN)return current+std::min(page,count-1-current);
 return current;
}
// Settings and context menus contain a bounded, static set of direct children.
// Enumerate controls, including scrolled-off ones, without allocating a group.
unsigned keyboard_controls(lv_obj_t *parent,lv_obj_t **items,unsigned capacity) {
 unsigned count=0;
 for(uint32_t i=0;i<lv_obj_get_child_count(parent)&&count<capacity;i++) {
  auto *o=lv_obj_get_child(parent,i);
  if(lv_obj_has_flag(o,LV_OBJ_FLAG_CLICKABLE)&&!lv_obj_has_flag(o,LV_OBJ_FLAG_HIDDEN)&&!lv_obj_has_flag(o,LV_OBJ_FLAG_FLOATING)&&!lv_obj_has_state(o,LV_STATE_DISABLED))items[count++]=o;
 }
 return count;
}
void keyboard_reveal_control(lv_obj_t *o) {
 keyboard_widget(o);lv_obj_update_layout(o);lv_obj_scroll_to_view(o,LV_ANIM_OFF);
}
bool keyboard_settings(uint32_t key,lv_obj_t *parent) {
 const bool forward=key==LV_KEY_NEXT||key==LV_KEY_DOWN,backward=key==LV_KEY_PREV||key==LV_KEY_UP;
 if(!keyboard_navigation(key)&&!forward&&!backward&&key!=LV_KEY_LEFT&&key!=LV_KEY_RIGHT)return false;
 // Wallpaper scanning is capped at 128 files, plus the default choice.
 lv_obj_t *items[130];unsigned count=keyboard_controls(parent,items,parent==ui.lock_list?130:24);if(!count)return true;
 unsigned index=0;bool found=false;
 for(unsigned i=0;i<count;i++)if(items[i]==ui.keyboard_widget){index=i;found=true;break;}
 if(found&&(key==LV_KEY_LEFT||key==LV_KEY_RIGHT)&&lv_obj_check_type(items[index],&lv_slider_class)) {
  auto *slider=items[index];int step=slider==ui.brightness?5:1;
  int value=std::clamp(lv_slider_get_value(slider)+(key==LV_KEY_RIGHT?step:-step),lv_slider_get_min_value(slider),lv_slider_get_max_value(slider));
  if(value!=lv_slider_get_value(slider)){lv_slider_set_value(slider,value,LV_ANIM_OFF);lv_obj_send_event(slider,LV_EVENT_VALUE_CHANGED,nullptr);}
  return true;
 }
 if(key==LV_KEY_HOME||key==PLAYER_UI_KEY_PAGE_UP)index=0;
 else if(key==LV_KEY_END||key==PLAYER_UI_KEY_PAGE_DOWN)index=count-1;
 else if(found&&(forward||key==LV_KEY_RIGHT))index=(index+1)%count;
 else if(found&&(backward||key==LV_KEY_LEFT))index=(index+count-1)%count;
 keyboard_reveal_control(items[index]);
 if(key==LV_KEY_ENTER&&!lv_obj_check_type(items[index],&lv_slider_class)) {
  int page=ui.settings_page;lv_obj_send_event(items[index],LV_EVENT_CLICKED,nullptr);
  // Enter on a category exposes its first control immediately. Do not retain a
  // pointer to the old menu when the action deleted it or opened an overlay.
  if(ui.settings_open&&page!=ui.settings_page&&!ui.overlay){unsigned n=keyboard_controls(ui.settings_pages[ui.settings_page],items,24);if(n)keyboard_reveal_control(items[0]);}
 }
 return true;
}
}
extern "C" bool player_ui_keyboard_text_allowed(void) {
 return ui.root&&!ui.settings_open&&!ui.overlay&&!ui.sleeping&&!ui.menu&&!ui.lock_panel;
}
extern "C" bool player_ui_keyboard(uint32_t key) {
 if(ui.lock_panel){
  if(ui.sleeping)return true;
  if(ui.lock_panel_kind==1){
   if(key==LV_KEY_ESC&&ui.lock_can_cancel)emit(PLAYER_UI_LOCK_INPUT,-2);
   else if(ui.lock_input_enabled&&key>='0'&&key<='9')emit(PLAYER_UI_LOCK_INPUT,int(key-'0'));
   else if(ui.lock_input_enabled&&(key==LV_KEY_BACKSPACE||key==LV_KEY_DEL))emit(PLAYER_UI_LOCK_INPUT,-1);
  }else if(key==LV_KEY_ESC)emit(PLAYER_UI_LOCK_INPUT,-2);
  else if(ui.lock_list)keyboard_settings(key,ui.lock_list);
  return true;
 }
 if(!ui.root||ui.overlay||ui.sleeping)return false;
 if(key==LV_KEY_ESC) {
  if(ui.menu){close_menu();keyboard_clear();return true;}
  if(ui.settings_open){keyboard_clear();if(ui.settings_page)settings_page(0);else settings_visible(false);return true;}
  return keyboard_clear();
 }
 if(ui.menu)return keyboard_settings(key,lv_obj_get_child(ui.menu,0));
 if(ui.settings_open)return keyboard_settings(key,ui.settings_pages[ui.settings_page]);
 if(!keyboard_navigation(key))return false;
 const bool folders=ui.state.library_tab==1;size_t count=folders?ui.folder_count:ui.tracks.size();
 if(!count){keyboard_clear();return true;}
 const size_t initial=folders?ui.folder_data->selected:ui.state.track_index;
 size_t row=ui.keyboard_tab==ui.state.library_tab?ui.keyboard_row:initial;
 row=keyboard_index(key,row,count,folders?6:4);
 ui.keyboard_tab=ui.state.library_tab;ui.keyboard_row=row;ui.interaction_tick=lv_tick_get();
 auto *list=folders?ui.folders:ui.playlist;keyboard_reveal(list,row,folders?54:CELL,folders?52:CELL);
 if(folders)ui.folder_data->refresh();else refresh_rows();lv_obj_invalidate(list);
 if(key==LV_KEY_ENTER)emit(folders?PLAYER_UI_FOLDER_SELECT:PLAYER_UI_SELECT_TRACK,int(row));
 return true;
}
