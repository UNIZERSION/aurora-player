#include "ui_fonts.h"
#include <algorithm>

extern "C" {
LV_FONT_DECLARE(ui_montserrat_latin_14);
LV_FONT_DECLARE(ui_montserrat_latin_16);
LV_FONT_DECLARE(ui_montserrat_latin_18);
LV_FONT_DECLARE(ui_montserrat_latin_20);
LV_FONT_DECLARE(ui_montserrat_latin_24);
LV_FONT_DECLARE(ui_montserrat_latin_32);
}

namespace {
lv_font_t with_latin(const lv_font_t &base, const lv_font_t &latin) {
    lv_font_t result = base;
    result.fallback = &latin;
    // The ASCII subset has a shorter bounding box than accented capitals.
    // Preserve all glyph widths, outlines, kerning and icon data, but reserve
    // the union of both vertical extents to avoid clipping accents/ogoneks.
    const auto ascent = std::max(base.line_height - base.base_line,
                                 latin.line_height - latin.base_line);
    result.base_line = std::max(base.base_line, latin.base_line);
    result.line_height = ascent + result.base_line;
    return result;
}
}

const lv_font_t ui_font_montserrat_14 = with_latin(lv_font_montserrat_14, ui_montserrat_latin_14);
const lv_font_t ui_font_montserrat_16 = with_latin(lv_font_montserrat_16, ui_montserrat_latin_16);
const lv_font_t ui_font_montserrat_18 = with_latin(lv_font_montserrat_18, ui_montserrat_latin_18);
const lv_font_t ui_font_montserrat_20 = with_latin(lv_font_montserrat_20, ui_montserrat_latin_20);
const lv_font_t ui_font_montserrat_24 = with_latin(lv_font_montserrat_24, ui_montserrat_latin_24);
const lv_font_t ui_font_montserrat_32 = with_latin(lv_font_montserrat_32, ui_montserrat_latin_32);

const lv_font_t *ui_font_for(const lv_font_t *base) {
    if(base == &lv_font_montserrat_14) return &ui_font_montserrat_14;
    if(base == &lv_font_montserrat_16) return &ui_font_montserrat_16;
    if(base == &lv_font_montserrat_18) return &ui_font_montserrat_18;
    if(base == &lv_font_montserrat_20) return &ui_font_montserrat_20;
    if(base == &lv_font_montserrat_24) return &ui_font_montserrat_24;
    if(base == &lv_font_montserrat_32) return &ui_font_montserrat_32;
    return base;
}
