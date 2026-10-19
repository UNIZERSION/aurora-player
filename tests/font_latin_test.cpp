#ifdef NDEBUG
#undef NDEBUG
#endif
#include "ui_fonts.h"
#include <cassert>
#include <cstdio>

static void same_glyph(const lv_font_t *base, const lv_font_t *extended,
                       uint32_t letter, uint32_t next) {
    lv_font_glyph_dsc_t a{}, b{};
    assert(lv_font_get_glyph_dsc(base, &a, letter, next));
    assert(lv_font_get_glyph_dsc(extended, &b, letter, next));
    assert(!a.is_placeholder && !b.is_placeholder);
    assert(a.adv_w == b.adv_w && a.box_w == b.box_w && a.box_h == b.box_h);
    assert(a.ofs_x == b.ofs_x && a.ofs_y == b.ofs_y && a.gid.index == b.gid.index);
    assert(a.resolved_font->dsc == b.resolved_font->dsc);
}

int main() {
    lv_init();
    const lv_font_t *bases[] = {&lv_font_montserrat_14, &lv_font_montserrat_16,
        &lv_font_montserrat_18, &lv_font_montserrat_20, &lv_font_montserrat_24,
        &lv_font_montserrat_32};
    const int sizes[] = {14,16,18,20,24,32};
    const int heights[] = {19,22,24,26,31,41};
    unsigned rendered = 0, preserved = 0;
    for(unsigned i = 0; i < 6; ++i) {
        const auto *base = bases[i];
        const auto *font = ui_font_for(base);
        assert(font && font != base && font->fallback && !base->fallback);
        assert(ui_font_for(font) == font);
        assert(font->line_height == heights[i]);
        for(uint32_t letter = 0x20; letter < 0x7f; ++letter) {
            for(uint32_t next = 0x20; next < 0x7f; ++next) {
                same_glyph(base, font, letter, next);
                ++preserved;
            }
        }
        // Exercise the existing icons, including power, music and battery.
        for(uint32_t letter = 0xf000; letter < 0xf900; ++letter) {
            lv_font_glyph_dsc_t icon{};
            if(base->get_glyph_dsc(base, &icon, letter, 0)) {
                same_glyph(base, font, letter, 0);
                ++preserved;
            }
        }
        unsigned glyph_count = 0;
        for(uint32_t letter = 0xa0; letter <= 0x17f; ++letter) {
            lv_font_glyph_dsc_t glyph{};
            assert(lv_font_get_glyph_dsc(font, &glyph, letter, 0));
            assert(!glyph.is_placeholder && glyph.resolved_font);
            ++glyph_count;
            if(!glyph.box_w || !glyph.box_h) continue; // NBSP is intentionally blank.
            const int top = font->line_height - font->base_line - glyph.box_h - glyph.ofs_y;
            assert(top >= 0 && top + glyph.box_h <= font->line_height);
            auto *bitmap = lv_draw_buf_create(glyph.box_w, glyph.box_h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
            assert(bitmap && lv_font_get_glyph_bitmap(&glyph, bitmap));
            bool ink = false;
            for(uint32_t y = 0; y < glyph.box_h; ++y) {
                const auto *row = static_cast<const uint8_t *>(lv_draw_buf_goto_xy(bitmap, 0, y));
                for(uint32_t x = 0; x < glyph.box_w; ++x) ink |= row[x] != 0;
            }
            assert(ink);
            ++rendered;
            lv_draw_buf_destroy(bitmap);
        }
        std::printf("Montserrat %d: %u Latin glyphs, height=%ld baseline=%ld PASS\n",
                    sizes[i], glyph_count, long(font->line_height), long(font->base_line));
    }
    lv_font_t unknown{};
    assert(ui_font_for(nullptr) == nullptr && ui_font_for(&unknown) == &unknown);
    std::printf("PASS: %u real Latin bitmaps; %u unchanged ASCII pairs/icons\n", rendered, preserved);
    lv_deinit();
}
