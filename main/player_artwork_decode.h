#pragma once
#include "lvgl.h"
#include <stdbool.h>

enum { PLAYER_ARTWORK_EDGE = 312 };
typedef struct {
    lv_image_dsc_t image;
    void *allocation;
} player_artwork_image_t;
/* Called before SD operations and during decode. False cancels this request.
 * The worker's implementation also waits for sufficient audio buffering. */
typedef bool (*player_artwork_continue_cb)(void *context, bool will_read_sd);

/* JPEG/PNG in ID3v2.2 PIC, v2.3/v2.4 APIC (including WAV id3 chunks and
 * leading ADTS AAC tags), native FLAC PICTURE, then same-folder cover files.
 * Preference: front, Other, back/leaflet/media, other pictures, icons last.
 * JPEG baseline/scaled fast path plus bounded progressive/general fallback.
 * This is not an MP4/M4A atom parser. NULL includes absent/unsupported/cancelled.
 * Ordinary metadata/audio payloads are skipped by checked seeks; global
 * v2.2/v2.3 unsync keeps a bounded 2 MiB compatibility buffer. */
player_artwork_image_t *player_artwork_load(const char *track_path,
    player_artwork_continue_cb proceed, void *context);
void player_artwork_image_free(player_artwork_image_t *image);
player_artwork_image_t *player_artwork_image_clone(const player_artwork_image_t *image);
