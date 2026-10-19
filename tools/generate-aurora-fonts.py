from pathlib import Path
from PIL import ImageFont
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/"main"
# These bitmap glyphs use the same fonts as the approved browser on this PC.
# TTF files remain on the owner's machine and are not included in the package.
fonts=[('sans20','arialbd.ttf',20),('sans14','arialbd.ttf',15),('sans16','arialbd.ttf',16),('sans17','arialbd.ttf',17),('sans18','arialbd.ttf',18),('title31','arialbd.ttf',31),('mono10','consolab.ttf',14),('mono12','consolab.ttf',14),('mono13','consolab.ttf',14),('mono16','consolab.ttf',16),('mono19','consolab.ttf',19),('mono25','consolab.ttf',25),('mono29','consolab.ttf',29)]
c=['#include "lvgl.h"'];header=['#pragma once','#include "lvgl.h"','#ifdef __cplusplus','extern "C" {','#endif']
bitmap_owners={}
for name,file,size in fonts:
    font=ImageFont.truetype(str(Path('C:/Windows/Fonts')/file),size);asc,desc=font.getmetrics();bitmap=bytearray();glyphs=['{0,0,0,0,0,0}']
    for cp in list(range(32,384))+list(range(8192,8304)):
        ch=chr(cp);box=font.getbbox(ch,anchor='ls');x0,y0,x1,y1=box;w=x1-x0;h=y1-y0
        mask=font.getmask(ch,'L');pixels=list(mask);offset=len(bitmap)
        # getmask dimensions match getbbox, including accented glyph ascent.
        vals=[min(15,(p+8)//17) for p in pixels]
        if len(vals)%2: vals.append(0)
        bitmap.extend((vals[i]<<4)|vals[i+1] for i in range(0,len(vals),2))
        glyphs.append('{%d,%d,%d,%d,%d,%d}'%(offset,round(font.getlength(ch)*16),w,h,x0,-y1))
    # Enlarged small styles can use identical immutable pixels. Keep their own
    # metrics and descriptors, sharing only byte-identical bitmap storage.
    owner=bitmap_owners.setdefault(bytes(bitmap),name)
    if owner==name:c.append('static const uint8_t bitmap_'+name+'[]={'+','.join(map(str,bitmap))+'};')
    c+=['static const lv_font_fmt_txt_glyph_dsc_t glyphs_'+name+'[]={'+','.join(glyphs)+'};',f'static const lv_font_fmt_txt_cmap_t cmap_{name}[]={{ {{.range_start=32,.range_length=352,.glyph_id_start=1,.type=LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY}}, {{.range_start=8192,.range_length=112,.glyph_id_start=353,.type=LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY}} }};',f'static lv_font_fmt_txt_dsc_t dsc_{name}={{.glyph_bitmap=bitmap_{owner},.glyph_dsc=glyphs_{name},.cmaps=cmap_{name},.cmap_num=2,.bpp=4,.bitmap_format=0}};',f'const lv_font_t aurora_{name}={{.get_glyph_dsc=lv_font_get_glyph_dsc_fmt_txt,.get_glyph_bitmap=lv_font_get_bitmap_fmt_txt,.line_height={asc+desc},.base_line={desc},.dsc=&dsc_{name}}};']
    header.append(f'extern const lv_font_t aurora_{name};')
(OUT/'aurora_fonts.c').write_text('\n'.join(c));(OUT/'aurora_fonts.h').write_text('\n'.join(header+['#ifdef __cplusplus','}','#endif']))
print('Aurora font glyphs generated.')
