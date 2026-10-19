#pragma once
#include "lvgl.h"
#include "lock_wallpaper.h"
#include <atomic>
#include <cstdio>
#include <cstring>

namespace player {
// The player is built on a separate, inactive screen. This screen remains
// active until its settings, catalog and mandatory PIN view are prepared.
// Boot keeps the backlight off until optional artwork has been resolved, then
// tab5_display_reveal() publishes this complete screen before enabling light.
// All methods except restart_requested() require the LVGL lock.
class StartupView {
    lv_obj_t *screen_=nullptr,*caption_=nullptr,*image_=nullptr,*restart_=nullptr;
    lv_image_dsc_t descriptor_{};
    std::atomic_bool restart_requested_{false};
    char version_[32]{};
public:
    lv_obj_t *screen() const { return screen_; }
    bool has_artwork() const { return image_!=nullptr; }
    bool restart_requested() const { return restart_requested_.load(std::memory_order_relaxed); }
    void create(const char *version) {
        snprintf(version_,sizeof version_,"%s",version?version:"");
        screen_=lv_obj_create(nullptr);lv_obj_remove_style_all(screen_);
        lv_obj_set_style_bg_color(screen_,lv_color_hex(0x080913),0);
        lv_obj_set_style_bg_opa(screen_,LV_OPA_COVER,0);
        lv_obj_set_style_text_color(screen_,lv_color_hex(0xDBD8FF),0);
        lv_obj_remove_flag(screen_,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(screen_,LV_OBJ_FLAG_CLICKABLE);
        caption_=lv_label_create(screen_);lv_obj_set_style_text_align(caption_,LV_TEXT_ALIGN_CENTER,0);
        progress("Starting interface...");lv_screen_load(screen_);
    }
    void artwork(const lock_wallpaper_image_t &image,const lv_font_t *font) {
        if(!image.pixels||image.width!=1280||image.height!=720)return;
        descriptor_.header.magic=LV_IMAGE_HEADER_MAGIC;
        descriptor_.header.cf=LV_COLOR_FORMAT_RGB565;
        descriptor_.header.w=image.width;descriptor_.header.h=image.height;
        descriptor_.header.stride=image.width*2;
        descriptor_.data_size=image.width*image.height*2;
        descriptor_.data=reinterpret_cast<const uint8_t *>(image.pixels);
        image_=lv_image_create(screen_);lv_obj_set_pos(image_,0,0);
        lv_image_set_src(image_,&descriptor_);lv_image_set_antialias(image_,false);
        lv_obj_remove_flag(image_,LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_foreground(caption_);lv_obj_set_width(caption_,1100);
        lv_obj_set_style_text_color(caption_,lv_color_white(),0);
        lv_obj_set_style_text_font(caption_,font,0);
        lv_obj_set_style_bg_color(caption_,lv_color_black(),0);
        lv_obj_set_style_bg_opa(caption_,LV_OPA_COVER,0);
        lv_obj_set_style_pad_all(caption_,8,0);
        progress("Starting interface...");
    }
    void progress(const char *message) {
        if(!caption_)return;
        char text[256];
        if(image_)snprintf(text,sizeof text,"%s",message?message:"");
        else snprintf(text,sizeof text,"AURORA PLAYER %s\n%s",version_,message?message:"");
        if(strcmp(lv_label_get_text(caption_),text))lv_label_set_text(caption_,text);
        if(image_)lv_obj_align(caption_,LV_ALIGN_BOTTOM_MID,0,-19);
        else lv_obj_center(caption_);
    }
    void stalled() {
        progress("SD is taking longer to respond. You can wait or restart.");
        if(restart_)return;
        restart_=lv_button_create(screen_);lv_obj_set_size(restart_,230,54);
        lv_obj_align(restart_,LV_ALIGN_BOTTOM_MID,0,-88);
        auto *text=lv_label_create(restart_);lv_label_set_text(text,"RESTART PLAYER");lv_obj_center(text);
        lv_obj_add_event_cb(restart_,[](lv_event_t *e){
            static_cast<StartupView *>(lv_event_get_user_data(e))->restart_requested_.store(true,std::memory_order_relaxed);
        },LV_EVENT_CLICKED,this);
    }
    void resumed() {
        if(restart_){lv_obj_delete(restart_);restart_=nullptr;}
    }
    void destroy() {
        // Caller has loaded the complete player or PIN screen first.
        if(screen_){lv_obj_delete(screen_);screen_=caption_=image_=restart_=nullptr;}
        lv_image_cache_drop(&descriptor_);
    }
};
} // namespace player
