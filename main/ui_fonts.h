#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Existing LVGL ASCII/FontAwesome glyphs plus matching Montserrat-Medium
 * Latin-1 and Latin Extended-A (U+00A0..U+017F). No heap allocations.
 * Line metrics include room for capital accents and descending ogoneks;
 * callers should derive label height from the returned font. */
LV_FONT_DECLARE(ui_font_montserrat_14);
LV_FONT_DECLARE(ui_font_montserrat_16);
LV_FONT_DECLARE(ui_font_montserrat_18);
LV_FONT_DECLARE(ui_font_montserrat_20);
LV_FONT_DECLARE(ui_font_montserrat_24);
LV_FONT_DECLARE(ui_font_montserrat_32);

/* Map an original supported LVGL font to its immutable extended version.
 * Unknown fonts and NULL pass through unchanged. Already mapped fonts do too. */
const lv_font_t *ui_font_for(const lv_font_t *base);

#ifdef __cplusplus
}
#endif
