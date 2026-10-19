#undef NDEBUG /* zig -O1/-O2 predefines NDEBUG; keep assert() checks active */
#define TAB5_DISPLAY_HOST_TEST
#include "../components/tab5_hal/tab5_display.c"
static void complete_queued_transfer(void){
 present_job_t job;assert(xQueueReceive(presentation_queue,&job,0)==pdTRUE);
 ++host_worker_calls;process_presentation(&job);
}
static void lvgl_wait_completion(lv_display_t *display){
 // Match installed LVGL9.3 lv_refr.c:1422: the custom callback waits;
 // its caller clears flushing, without invoking lv_display_flush_ready.
 if(display->flushing){wait_for_flush(display);display->flushing=false;}
}
static void lvgl_submit(lv_display_t *display,const lv_area_t *area,uint16_t *pixels,bool last){
 assert(!display->flushing);display->flushing=true;display->last=last;
 flush(display,area,(uint8_t*)pixels);
}
static void fill_piece(lv_display_t *display,lv_area_t area,uint16_t value,bool last){
 size_t n=lv_area_get_size(&area);uint16_t *pixels=malloc(n*2);for(size_t i=0;i<n;i++)pixels[i]=value;
 lvgl_submit(display,&area,pixels,last);lvgl_wait_completion(display);free(pixels);
}
static uint16_t refresh_pixels[16];
static lv_area_t refresh_area={850,310,857,311};
static void queue_refresh(void){
 lvgl_submit(screen,&refresh_area,refresh_pixels,true);
}
static void set_oracle(uint16_t *oracle,lv_area_t area,const uint16_t *pixels){
 for(int y=area.y1;y<=area.y2;y++)for(int x=area.x1;x<=area.x2;x++)
   oracle[(1279-x)*720+y]=pixels[(y-area.y1)*lv_area_get_width(&area)+x-area.x1];
}
static void test_async_ownership(lv_display_t *display){
 uint16_t buffers[2][16];uint16_t *oracle=malloc(FRAME_BYTES);
 memcpy(oracle,frames[latest],FRAME_BYTES);
 // Model the real LVGL9.3 ordering: draw into the other buffer, wait for the
 // previous transfer, then enqueue. Completion alternates before/inside wait.
 unsigned slot=0;lv_area_t previous={0};
 for(int n=0;n<80;n++){
   unsigned other=slot^1;for(int i=0;i<16;i++)buffers[other][i]=(uint16_t)(n*101+i);
   if(n){
     assert(transfer_pending&&host_queue.queued);
     assert(((host_semaphore_t*)presentation_done)->count==0);
     unsigned before=publications;
     // CPU just modified the OTHER buffer while this transfer remained queued.
     if(n&1){complete_queued_transfer();assert(publications==before+1);}
     assert(display->flushing);lvgl_wait_completion(display);
     assert(!transfer_pending&&!host_queue.queued);
     set_oracle(oracle,previous,buffers[slot]);
     assert(!memcmp(oracle,frames[latest],FRAME_BYTES));
   }
   slot=other;previous=(lv_area_t){30+n*9,210+n,37+n*9,211+n};
   lv_area_t transient=previous;display->last=true;
   uint64_t started=fake_time;unsigned before=publications,ready_before=display->ready;
   lvgl_submit(display,&transient,buffers[slot],true);
   // An enqueue performs no PPA work and cannot make its source reusable.
   assert(fake_time==started&&publications==before&&display->ready==(int)ready_before);
   assert(transfer_pending&&host_queue.queued);
   transient=(lv_area_t){0,0,0,0};display->last=false;
 }
 lvgl_wait_completion(display);set_oracle(oracle,previous,buffers[slot]);
 assert(!memcmp(oracle,frames[latest],FRAME_BYTES));
 free(oracle);
 puts("PASS: 80 delayed transfers, alternating draw buffers, copied area/last flags, completion before/during wait; every framebuffer pixel matches oracle.");
}
static void test_drains(lv_display_t *display){
 uint16_t pixels[16],*oracle=malloc(FRAME_BYTES),*snap=malloc(FRAME_BYTES);
 for(int i=0;i<16;i++){pixels[i]=0x7200+i;refresh_pixels[i]=0x7900+i;}
 const lv_area_t area={750,220,757,221};memcpy(oracle,frames[latest],FRAME_BYTES);
 lvgl_submit(display,&area,pixels,true);
 assert(transfer_pending&&host_queue.queued);
 // This is also the exact API root calls under LVGL lock before power off.
 assert(tab5_display_sleep(true,73)==ESP_OK);
 set_oracle(oracle,area,pixels);assert(!memcmp(oracle,frames[latest],FRAME_BYTES));
 assert(!transfer_pending&&!display->flushing&&!host_queue.queued&&backlight==0);
 host_refresh=queue_refresh;
 assert(tab5_display_sleep(false,73)==ESP_OK);
 set_oracle(oracle,refresh_area,refresh_pixels);assert(!memcmp(oracle,frames[latest],FRAME_BYTES));
 assert(!transfer_pending&&!display->flushing&&!host_queue.queued&&backlight==73);
 // Snapshot must drain both work queued earlier and its own forced refresh.
 for(int i=0;i<16;i++){pixels[i]=0x7300+i;refresh_pixels[i]=0x7a00+i;}
 lvgl_submit(display,&area,pixels,true);
 assert(tab5_display_snapshot_rgb565(snap,NATIVE_W*NATIVE_H)==ESP_OK);
 set_oracle(oracle,area,pixels);set_oracle(oracle,refresh_area,refresh_pixels);
 assert(!memcmp(oracle,frames[latest],FRAME_BYTES));
 for(int y=0;y<720;y++)for(int x=0;x<1280;x++)assert(snap[y*1280+x]==oracle[(1279-x)*720+y]);
 assert(!transfer_pending&&!display->flushing&&!host_queue.queued);
 host_refresh=NULL;free(snap);free(oracle);
 puts("PASS: pending sleep/shutdown drain, wake-refresh before backlight, snapshot drains previous/new work and matches all 921600 pixels.");
}
static void test_full_refresh_pipeline(lv_display_t *display){
 uint16_t *buffers[2]={malloc(DRAW_PIXELS*2),malloc(DRAW_PIXELS*2)};
 uint16_t *oracle=malloc(FRAME_BYTES);memcpy(oracle,frames[latest],FRAME_BYTES);
 unsigned before=publications;int ready_before=display->ready;
 for(int part=0;part<4;part++){
   int slot=part%2;lv_area_t area={0,slot?480:0,1279,slot?719:479};
   for(int i=0;i<lv_area_get_size(&area);i++)buffers[slot][i]=(uint16_t)(i*17+part*131);
   if(part){
     assert(display->flushing);lvgl_wait_completion(display);
     assert(publications==before+(unsigned)(part/2));
     if(part==2){assert(pending);assert(!memcmp(oracle,frames[latest],FRAME_BYTES));}
   }
   set_oracle(oracle,area,buffers[slot]);
   lvgl_submit(display,&area,buffers[slot],slot==1);
 }
 lvgl_wait_completion(display);
 assert(publications==before+2&&pending&&!display->flushing&&!transfer_pending);
 assert(!memcmp(oracle,frames[latest],FRAME_BYTES));
 // The normal LVGL wait clears flushing itself, not through flush_ready.
 assert(display->ready==ready_before);
 free(buffers[0]);free(buffers[1]);free(oracle);
 puts("PASS: two consecutive 1280x720 refreshes, split across the real 1280x480 draw-buffer capacity, publish only complete frames and match every pixel.");
}
static void test_first_visible_frame(lv_display_t *display){
 // The text may already have rendered invisibly. Replacing it with SD artwork
 // must finish both pending work and the final refresh before lighting up.
 const lv_area_t old_area={750,220,757,221};uint16_t old_pixels[16];
 for(unsigned i=0;i<16;i++){old_pixels[i]=0x1111;refresh_pixels[i]=0x6000+i;}
 assert(bsp_display_brightness_set(0)==ESP_OK);
 lvgl_submit(display,&old_area,old_pixels,true);
 assert(transfer_pending&&host_queue.queued&&backlight==0);
 host_refresh=queue_refresh;require_visible_scanout=true;
 const unsigned before=publications;
 assert(tab5_display_reveal(70)==ESP_OK);
 assert(publications==before+2&&backlight==70&&!transfer_pending&&!host_queue.queued);
 assert(scanning==(uint16_t*)frames[latest]&&edges_after_publish>=2);
 for(int y=refresh_area.y1;y<=refresh_area.y2;y++)for(int x=refresh_area.x1;x<=refresh_area.x2;x++)
   assert(scanning[(1279-x)*720+y]==refresh_pixels[(y-refresh_area.y1)*8+x-refresh_area.x1]);
 // Missing/corrupt SD image takes the same safe presentation path, with the
 // original text screen selected instead of artwork; no image buffer needed.
 host_refresh=NULL;
 assert(bsp_display_brightness_set(0)==ESP_OK);
 assert(tab5_display_reveal(73)==ESP_OK&&backlight==73);
 require_visible_scanout=false;
 puts("PASS: first visible startup frame drains invisible text and new artwork, waits two scanout edges, then lights up; unchanged fallback also reveals safely.");
}
int main(int argc,char **argv){
 if(argc>1&&!strncmp(argv[1],"startup-",8)){
   bool failure=false;
   if(!strcmp(argv[1],"startup-expander"))startup_expander_failures=1;
   else if(!strcmp(argv[1],"startup-feature"))startup_feature_failures=1;
   else if(!strcmp(argv[1],"startup-expander-all")){startup_expander_failures=3;failure=true;}
   else if(!strcmp(argv[1],"startup-feature-all")){startup_feature_failures=3;failure=true;}
   else if(!strcmp(argv[1],"startup-i2c-all")){startup_i2c_failures=3;failure=true;}
   else if(!strcmp(argv[1],"startup-reset")){startup_expander_failures=2;startup_reset_failure=true;}
   else assert(false);
   lv_display_t *probe=(void*)1;bool touch=true;
   esp_err_t result=tab5_display_atomic_start(&probe,&touch);
   if(failure){
     assert(result==ESP_FAIL&&!probe&&!touch);
     assert(startup_lvgl_calls==0&&startup_panel_calls==0&&host_semaphore_count==0&&!presentation_queue);
     assert(scanout_qos_calls==0);
     assert(startup_delays==2&&startup_delay_ms==150);
     if(!strcmp(argv[1],"startup-i2c-all"))assert(startup_bus_resets==0&&startup_expander_creates==0);
     else assert(startup_bus_resets==1);
     if(!strcmp(argv[1],"startup-expander-all"))assert(startup_expander_creates==3&&startup_feature_calls==0);
     if(!strcmp(argv[1],"startup-feature-all"))assert(startup_expander_creates==1&&startup_feature_calls==3);
   }else{
     assert(result==ESP_OK&&probe&&touch&&startup_lvgl_calls==1&&startup_panel_calls==1);
     assert(startup_bus_resets==1);
     if(!strcmp(argv[1],"startup-reset"))assert(startup_expander_creates==3&&startup_delays==2&&startup_delay_ms==150);
     else assert(startup_delays==1&&startup_delay_ms==50);
     if(!strcmp(argv[1],"startup-expander"))assert(startup_expander_creates==2&&startup_feature_calls==1);
     if(!strcmp(argv[1],"startup-feature"))assert(startup_expander_creates==1&&startup_feature_calls==2);
   }
   printf("PASS: %s: bounded retry, NULL guarded, one bus reset at most, no duplicate LVGL/panel initialization.\n",argv[1]);
   return 0;
 }
 AXI_ICM.mst_awqos_reg0.val=0x76543210u;AXI_ICM.mst_arqos_reg0.val=0x89abcdefu;
 lv_display_t *boot=NULL;bool ready=false;assert(tab5_display_atomic_start(&boot,&ready)==ESP_OK&&ready);assert(touch_mode==LV_INDEV_MODE_TIMER&&touch_period==16);
 assert(scanout_qos_calls==1&&AXI_ICM.mst_awqos_reg0.val==0x76543210u&&AXI_ICM.mst_arqos_reg0.val==0x89afcdefu);
 puts("PASS: before panel scanout, DW-GDMA memory-master read QoS is 15; its nonzero write priority and every other master priority are preserved.");
 assert(startup_lvgl_calls==1&&startup_panel_calls==1&&startup_expander_creates==1&&startup_feature_calls==1&&startup_bus_resets==0&&startup_delays==0);
 host_progress=complete_queued_transfer;
 scanning=(uint16_t*)frames[0];published_frame=scanning;sequence_ptr=&vsync_sequence;lv_display_t display={0};
 for(int y=0;y<720;y+=80){fill_piece(&display,(lv_area_t){0,y,1279,y+79},y/80+1,y==640);if(y<640)assert(publications==0);}
 assert(publications==1&&pending&&vsync_sequence==0);
 // A second frame can be rendered immediately while the first is pending.
 assert(finish_present(NULL)==ESP_OK&&back==2&&vsync_sequence==0);
 assert(!memcmp(frames[latest],frames[back],FRAME_BYTES));
 fill_piece(&display,(lv_area_t){10,13,20,31},0x1234,false);
 fill_piece(&display,(lv_area_t){1150,620,1210,681},0xabcd,true);
 assert(publications==2&&pending&&latest==2&&vsync_sequence==0);
 assert(finish_present(NULL)==ESP_OK&&back==0&&vsync_sequence>=2);
 assert(!memcmp(frames[latest],frames[back],FRAME_BYTES));
 assert(((uint16_t*)frames[latest])[(1279-10)*720+13]==0x1234);
 // Dirty history must survive repeated recycling and rectangle overflow.
 for(int pass=0;pass<9;pass++){
  for(int i=0;i<80;i++)fill_piece(&display,(lv_area_t){i*3,90+i*2,i*3,90+i*2},100+i+pass*100,i==79);
  assert(finish_present(NULL)==ESP_OK);assert(!memcmp(frames[latest],frames[back],FRAME_BYTES));
  for(int i=0;i<80;i++)assert(((uint16_t*)frames[latest])[(1279-i*3)*720+90+i*2]==100+i+pass*100);
 }
 // Repeated folder viewport: the old viewport must NOT be copied and then
 // immediately overwritten. All surrounding pixels must remain unchanged.
 const lv_area_t viewport={239,125,1046,584};
 uint16_t *oracle=malloc(FRAME_BYTES);memcpy(oracle,frames[latest],FRAME_BYTES);
 for(int frame=0;frame<32;frame++){
  uint64_t before=copied_pixels;
  uint64_t sync_before=present_sync_pixels;
  fill_piece(&display,viewport,0x4100+frame,true);
  for(int y=viewport.y1;y<=viewport.y2;y++)for(int x=viewport.x1;x<=viewport.x2;x++)oracle[(1279-x)*720+y]=0x4100+frame;
  assert(!memcmp(oracle,frames[latest],FRAME_BYTES));
  assert(present_first_row==1279-viewport.x2&&present_last_row==1279-viewport.x1);
  assert(present_sync_pixels-sync_before==(uint64_t)lv_area_get_width(&viewport)*720);
  if(frame>2)assert(copied_pixels==before);
 }
 puts("PASS: 32 folder frames preserve every pixel; steady viewport repair copies zero pixels (previously 371680/frame).");
 puts("PASS: folder presentation visits 581760 cache pixels instead of 921600, while flipping the complete pixel-identical framebuffer.");
 // Alternating overlapping and disjoint rectangles exercise every subtraction
 // edge against an independently updated full-frame reference.
 for(int n=0;n<200;n++){
  int x=(n*173)%1100,y=(n*97)%550,w=1+(n*19)%179,h=1+(n*23)%169;
  lv_area_t area={x,y,x+w-1,y+h-1};
  fill_piece(&display,area,0x6200+n,true);
  for(int yy=area.y1;yy<=area.y2;yy++)for(int xx=area.x1;xx<=area.x2;xx++)oracle[(1279-xx)*720+yy]=0x6200+n;
  assert(!memcmp(oracle,frames[latest],FRAME_BYTES));
 }
 free(oracle);
 // Backlight off pauses only the LVGL refresh timer. Touch and VSYNC stay live.
 screen=&display;original_touch_read=host_touch;
 test_async_ownership(&display);test_full_refresh_pipeline(&display);test_drains(&display);test_first_visible_frame(&display);
 for(unsigned cycle=0;cycle<5;cycle++){
  assert(tab5_display_sleep(true,73)==ESP_OK);assert(asleep&&backlight==0&&display_on&&refresh_timer.paused);
  raw_touch=1;lv_indev_data_t t={0};touch_read(NULL,&t);assert(!t.state&&tab5_display_wake_requested());
  assert(tab5_display_sleep(false,73)==ESP_OK);assert(!asleep&&backlight==73&&display_on&&!refresh_timer.paused);
  touch_read(NULL,&t);assert(!t.state);raw_touch=0;touch_read(NULL,&t);raw_touch=1;touch_read(NULL,&t);assert(t.state);
 }
 brightness_error=true;assert(tab5_display_sleep(true,73)==ESP_FAIL);assert(!asleep&&!refresh_timer.paused);brightness_error=false;
 assert(tab5_display_sleep(true,73)==ESP_OK);brightness_error=true;assert(tab5_display_sleep(false,73)==ESP_FAIL);assert(asleep&&refresh_timer.paused&&backlight==0);brightness_error=false;assert(tab5_display_sleep(false,73)==ESP_OK);
 uint16_t *snap=malloc(FRAME_BYTES);assert(tab5_display_snapshot_rgb565(snap,NATIVE_W*NATIVE_H)==ESP_OK);assert(snap[13*1280+10]==((uint16_t*)frames[latest])[(1279-10)*720+13]);free(snap);
 if(argc>1&&!strncmp(argv[1],"first-frame-",12)){
   assert(bsp_display_brightness_set(0)==ESP_OK);host_refresh=queue_refresh;
   if(!strcmp(argv[1],"first-frame-timeout")){
     stop_vsync=true;const uint64_t started=fake_time;
     assert(tab5_display_reveal(70)==ESP_ERR_TIMEOUT);
     assert(fake_time-started>=150000&&fake_time-started<500000);
   }else if(!strcmp(argv[1],"first-frame-ppa")){
     ppa_error_next=true;assert(tab5_display_reveal(70)==ESP_ERR_INVALID_STATE);
   }else if(!strcmp(argv[1],"first-frame-backlight")){
     brightness_error=true;assert(tab5_display_reveal(70)==ESP_FAIL);brightness_error=false;
   }else assert(false);
   assert(backlight==0&&!transfer_pending&&!host_queue.queued);
   for(unsigned i=0;i<3;i++)free(frames[i]);
   printf("PASS: %s leaves startup dark and returns a bounded error, never exposing an incomplete frame.\n",argv[1]);return 0;
 }else if(argc>1&&!strcmp(argv[1],"wake")){
   assert(tab5_display_sleep(true,73)==ESP_OK);
   host_refresh=queue_refresh;ppa_error_next=true;
   assert(tab5_display_sleep(false,73)==ESP_ERR_INVALID_STATE);
   assert(failed&&asleep&&backlight==0&&refresh_timer.paused&&!transfer_pending&&!host_queue.queued);
   host_refresh=NULL;
 }else if(argc>1){
   if(!strcmp(argv[1],"ppa"))ppa_error_next=true;
   else if(!strcmp(argv[1],"publish"))publish_error_next=true;
   else if(!strcmp(argv[1],"queue"))queue_error_next=true;
   else assert(false);
   unsigned before=publications;fill_piece(&display,(lv_area_t){100,100,101,101},0xf00d,true);
   assert(failed&&!transfer_pending&&!host_queue.queued&&publications==before);
 }else{
   stop_vsync=true;
   for(unsigned i=0;i<5&&!failed;i++)fill_piece(&display,(lv_area_t){100,100,101,101},0xf00d,true);
 }
 assert(failed);unsigned stopped=publications;fill_piece(&display,(lv_area_t){0,0,1279,79},0xffff,true);assert(publications==stopped);
 assert(!transfer_pending&&!host_queue.queued);assert(tab5_display_sleep(true,73)==ESP_OK&&backlight==0);
 if(argc>1)printf("PASS: %s failure releases pending work, preserves last published image, suppresses later publications and permits shutdown.\n",argv[1]);
 for(unsigned i=0;i<3;i++)free(frames[i]);
 puts("PASS: triple buffers render ahead; two-edge retirement, no writes to scanout; dirty repair/overflow; repeated backlight-off touch wake, LCD stays on; 16ms polling enabled; snapshot; timeout fails closed");return 0;
}
