namespace {
struct FolderList {
 struct Cell {lv_obj_t *box{},*caption{},*picture{},*symbol{};lv_draw_buf_t *cache{};FolderList *owner{};size_t index=SIZE_MAX;};
 lv_obj_t *list{},*spacer{};std::vector<std::string> names;Cell cells[11]{};
 int first=-1,row_width=795,cell_count=11;size_t selected=SIZE_MAX;bool embedded=false,updating=false,capturing=false;
 ~FolderList(){for(auto &c:cells)if(c.cache){lv_image_set_src(c.picture,nullptr);lv_image_cache_drop(c.cache);lv_draw_buf_destroy(c.cache);}}
 void background(lv_event_t *event){
  // Opaque row snapshots overwrite their rectangles. Fill only uncovered gaps
  // and gutters; retain LVGL scrolling, cover checks and live-label fallback.
  lv_area_t viewport;lv_obj_get_coords(list,&viewport);auto *layer=lv_event_get_layer(event);int next_y=viewport.y1;
  for(int offset=0;offset<cell_count&&first>=0;++offset){
   size_t index=size_t(first+offset);auto &c=cells[index%cell_count];
   if(c.index!=index||!c.cache||lv_obj_has_flag(c.box,LV_OBJ_FLAG_HIDDEN)||lv_obj_has_flag(c.picture,LV_OBJ_FLAG_HIDDEN))continue;
   lv_area_t row;lv_obj_get_coords(c.box,&row);int top=std::max(viewport.y1,row.y1),bottom=std::min(viewport.y2,row.y2);if(top>bottom)continue;
   rect(layer,viewport.x1,next_y,lv_area_get_width(&viewport),top-next_y,BLACK);
   rect(layer,viewport.x1,top,row.x1-viewport.x1,bottom-top+1,BLACK);
   rect(layer,row.x2+1,top,viewport.x2-row.x2,bottom-top+1,BLACK);next_y=bottom+1;
  }
  rect(layer,viewport.x1,next_y,lv_area_get_width(&viewport),viewport.y2-next_y+1,BLACK);lv_event_stop_processing(event);
 }
 void cache_row(Cell &c){
  // Eight bounded rows in Files, eleven in the legacy dialog/test API. Align
  // every source scanline once, avoiding unaligned PSRAM copies while scrolling.
  if(!c.cache)c.cache=lv_draw_buf_create(row_width,52,LV_COLOR_FORMAT_RGB565,(row_width*2+3)&~3);
  if(!c.cache)return;
  lv_image_cache_drop(c.cache);
  capturing=true;auto result=lv_snapshot_take_to_draw_buf(c.box,LV_COLOR_FORMAT_RGB565,c.cache);capturing=false;if(result!=LV_RESULT_OK)return;
  const uint32_t compact=c.cache->header.stride,aligned=(compact+3U)&~3U;
  if(aligned!=compact&&aligned*c.cache->header.h<=c.cache->data_size){
   for(uint32_t y=c.cache->header.h;y-->0;){auto *destination=c.cache->data+y*aligned;memmove(destination,c.cache->data+y*compact,compact);memset(destination+compact,0,aligned-compact);}
   c.cache->header.stride=aligned;
  }
  lv_draw_buf_flush_cache(c.cache,nullptr);lv_image_set_src(c.picture,c.cache);
  visible(c.caption,false);visible(c.symbol,false);lv_obj_set_style_bg_opa(c.box,0,0);visible(c.picture,true);
 }
 void refresh(){
  if(updating)return;
  int next=std::max(0,int(lv_obj_get_scroll_y(list))/54);if(next==first)return;first=next;
  for(int i=0;i<cell_count;i++){size_t index=size_t(first+i);auto &c=cells[index%cell_count];
   if(index>=names.size()){c.index=SIZE_MAX;visible(c.box,false);continue;}
   if(c.index==index)continue;
   c.index=index;visible(c.box,true);visible(c.picture,false);visible(c.caption,true);visible(c.symbol,true);
   lv_obj_set_y(c.box,int(index)*54);text(c.caption,names[index].c_str());
   solid(c.box,index==selected?0x343d25:0x161c19,4);ink(c.caption,index==selected?LIME:WHITE);cache_row(c);
  }
 }
 void replace(std::vector<std::string> &incoming,size_t choice){
  const bool reveal=choice!=selected;int scroll=std::max(0,int(lv_obj_get_scroll_y(list)));selected=choice;
  updating=true;lv_anim_delete(list,nullptr);names.swap(incoming);
  // Hide stale bottom rows before recomputing a shorter/empty list's extent.
  first=-1;for(auto &c:cells){c.index=SIZE_MAX;if(c.box)visible(c.box,false);}
  lv_obj_set_height(spacer,std::max(1,int(names.size())*54));lv_obj_update_layout(list);
  if(reveal&&selected<names.size())scroll=selected>3?int(selected-3)*54:0;
  int maximum=std::max(0,int(names.size())*54-int(lv_obj_get_height(list)));
  lv_obj_scroll_to_y(list,std::clamp(scroll,0,maximum),LV_ANIM_OFF);updating=false;refresh();lv_obj_invalidate(list);
 }
};
void folder_symbol(lv_event_t *event){
 auto *c=static_cast<FolderList::Cell*>(lv_event_get_user_data(event));lv_area_t a;lv_obj_get_coords(lv_event_get_target_obj(event),&a);auto *l=lv_event_get_layer(event);
 uint32_t foreground=c->index==c->owner->selected?LIME:0xabb5af;
 rect(l,a.x1,a.y1+4,24,17,foreground,2);rect(l,a.x1+1,a.y1,10,7,foreground,2);
 rect(l,a.x1+2,a.y1+6,20,13,c->index==c->owner->selected?0x343d25:0x161c19,1);
}
FolderList *make_folder_list(lv_obj_t *parent,bool embedded){
 auto *data=new FolderList;data->embedded=embedded;data->row_width=embedded?358:795;data->cell_count=embedded?8:11;
 const int width=embedded?367:808,height=embedded?328:460;
 data->list=plain(parent,embedded?880:12,embedded?186:58,width,height);solid(data->list,BLACK);
 lv_obj_add_flag(data->list,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_scroll_dir(data->list,LV_DIR_VER);lv_obj_set_scrollbar_mode(data->list,LV_SCROLLBAR_MODE_ON);
 lv_obj_set_style_width(data->list,embedded?6:7,LV_PART_SCROLLBAR);lv_obj_set_style_bg_color(data->list,color(0xb7c78d),LV_PART_SCROLLBAR);lv_obj_set_style_bg_opa(data->list,255,LV_PART_SCROLLBAR);
 lv_obj_add_event_cb(data->list,[](lv_event_t *e){static_cast<FolderList*>(lv_event_get_user_data(e))->background(e);},static_cast<lv_event_code_t>(LV_EVENT_DRAW_MAIN|LV_EVENT_PREPROCESS),data);
 lv_obj_add_event_cb(data->list,keyboard_touch,LV_EVENT_PRESSED,nullptr);data->spacer=plain(data->list,0,0,1,1);
 for(int i=0;i<data->cell_count;i++){auto &c=data->cells[i];c.owner=data;c.box=plain(data->list,0,0,data->row_width,52);lv_obj_add_flag(c.box,LV_OBJ_FLAG_CLICKABLE);
  if(embedded){c.symbol=plain(c.box,13,16,24,21);lv_obj_add_event_cb(c.symbol,folder_symbol,LV_EVENT_DRAW_MAIN,&c);}
  c.caption=label(c.box,"",embedded?49:18,14,data->row_width-(embedded?65:40),&aurora_sans18);
  c.picture=lv_image_create(c.box);lv_obj_set_pos(c.picture,0,0);lv_obj_remove_flag(c.picture,LV_OBJ_FLAG_CLICKABLE);visible(c.picture,false);visible(c.box,false);
  lv_obj_add_event_cb(c.box,[](lv_event_t *e){auto *c=static_cast<FolderList::Cell*>(lv_event_get_user_data(e));if(c->index<c->owner->names.size())emit(PLAYER_UI_FOLDER_SELECT,int(c->index));},LV_EVENT_SHORT_CLICKED,&c);
  if(embedded){
   lv_obj_add_event_cb(c.box,keyboard_touch,LV_EVENT_PRESSED,nullptr);
   lv_obj_add_event_cb(c.box,[](lv_event_t *e){auto *c=static_cast<FolderList::Cell*>(lv_event_get_user_data(e));if(!c->owner->capturing&&ui.keyboard_tab==1&&ui.keyboard_row==c->index)keyboard_outline(e);},LV_EVENT_DRAW_POST,&c);
   lv_obj_add_event_cb(c.box,[](lv_event_t *e){auto *o=lv_event_get_target_obj(e);auto *cell=static_cast<FolderList::Cell*>(lv_event_get_user_data(e));if(!cell->owner->capturing&&lv_obj_has_state(o,LV_STATE_PRESSED)){lv_area_t a;lv_obj_get_coords(o,&a);rect(lv_event_get_layer(e),a.x1,a.y1,lv_area_get_width(&a),lv_area_get_height(&a),LIME,4,28);}},LV_EVENT_DRAW_POST,&c);
   for(auto event:{LV_EVENT_PRESSED,LV_EVENT_RELEASED,LV_EVENT_PRESS_LOST})lv_obj_add_event_cb(c.box,press_feedback,event,nullptr);
  }
 }
 lv_obj_add_event_cb(data->list,[](lv_event_t *e){auto *data=static_cast<FolderList*>(lv_event_get_user_data(e));if(data->embedded)ui.interaction_tick=lv_tick_get();data->refresh();},LV_EVENT_SCROLL,data);
 lv_obj_add_event_cb(data->list,[](lv_event_t *e){auto *data=static_cast<FolderList*>(lv_event_get_user_data(e));if(ui.folder_data==data){ui.folder_data=nullptr;ui.folders=nullptr;ui.folder_count=0;}delete data;},LV_EVENT_DELETE,data);
 add_scroll_handle(data->list,width,height);return data;
}
lv_obj_t *create_embedded_folders(){ui.folder_data=make_folder_list(ui.root,true);visible(ui.folder_data->list,false);return ui.folder_data->list;}
}
struct player_ui_folders {std::vector<std::string> names;};
extern "C" player_ui_folders_t *player_ui_prepare_folders(const char *const *names,size_t count){
 auto *prepared=new(std::nothrow) player_ui_folders;
 if(!prepared){return nullptr;}
 if(!names){count=0;}
 // Keep height and action indices within LVGL's signed coordinate space.
 count=std::min<size_t>(count,INT32_MAX/54);prepared->names.reserve(count);
 for(size_t i=0;i<count;i++){prepared->names.emplace_back(names[i]?names[i]:"");}
 return prepared;
}
extern "C" void player_ui_apply_folders(player_ui_folders_t *prepared,size_t selected){
 if(!prepared||!ui.folder_data){return;}
 if(ui.keyboard_tab==1)keyboard_clear();
 ui.folder_data->replace(prepared->names,selected);
 ui.folder_count=ui.folder_data->names.size();
 refresh_library_view();
}
extern "C" void player_ui_free_folders(player_ui_folders_t *prepared){delete prepared;}
extern "C" lv_obj_t *player_ui_create_folder_list(lv_obj_t *parent,const char *const *names,size_t count,size_t selected){
 auto *data=make_folder_list(parent,false);auto *prepared=player_ui_prepare_folders(names,count);
 if(prepared){data->replace(prepared->names,selected);player_ui_free_folders(prepared);}return data->list;
}
