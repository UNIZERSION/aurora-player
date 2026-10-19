#pragma once
#include "lvgl.h"
#include "ui_fonts.h"
// Keyboard defaults to BOTTOM_MID in LVGL. set_pos alone keeps that anchor
// and puts most rows below the dialog. Anchor explicitly before sizing.
static inline lv_obj_t *player_search_keyboard(lv_obj_t *parent,lv_obj_t *input) {
 auto *keyboard=lv_keyboard_create(parent);
 lv_obj_align(keyboard,LV_ALIGN_TOP_LEFT,12,217);
 lv_obj_set_size(keyboard,795,302);
 lv_obj_set_style_text_font(keyboard,&ui_font_montserrat_18,LV_PART_ITEMS);
 lv_keyboard_set_textarea(keyboard,input);
 return keyboard;
}
