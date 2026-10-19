#pragma once
#include <stddef.h>
#include <stdint.h>
struct lv_obj_t {};
struct lv_display_t {};
struct lv_image_header_t { unsigned magic=0,cf=0,w=0,h=0,stride=0; };
struct lv_image_dsc_t { lv_image_header_t header{}; size_t data_size=0; const uint8_t *data=nullptr; };
enum { LV_IMAGE_HEADER_MAGIC=1, LV_COLOR_FORMAT_RGB565=2 };
