#pragma once
#include <stddef.h>
#include <stdint.h>

// All calls run on a worker, outside the LVGL lock. Pixels are immutable after
// publication and remain owned by the caller until lock_wallpaper_image_free.
enum { LOCK_WALLPAPER_WIDTH=1280, LOCK_WALLPAPER_HEIGHT=720,
       LOCK_WALLPAPER_MAX_FILES=128, LOCK_WALLPAPER_NAME_BYTES=241 };
enum lock_wallpaper_error_t {
    LOCK_WALLPAPER_OK=0, LOCK_WALLPAPER_INVALID_NAME, LOCK_WALLPAPER_UNAVAILABLE,
    LOCK_WALLPAPER_TOO_LARGE, LOCK_WALLPAPER_UNSUPPORTED, LOCK_WALLPAPER_CORRUPT,
    LOCK_WALLPAPER_NO_MEMORY, LOCK_WALLPAPER_CANCELLED, LOCK_WALLPAPER_INVALID_FRAME
};
struct lock_wallpaper_image_t {
    uint16_t *pixels=nullptr;
    unsigned width=0, height=0;
    lock_wallpaper_error_t error=LOCK_WALLPAPER_OK;
};
struct lock_wallpaper_list_t {
    char names[LOCK_WALLPAPER_MAX_FILES][LOCK_WALLPAPER_NAME_BYTES]{};
    size_t count=0;
    bool truncated=false;
    lock_wallpaper_error_t error=LOCK_WALLPAPER_OK;
};
// Return false to cancel. will_read_sd lets the controller defer disk reads
// until audio buffering is healthy. The helper also yields periodically on ESP.
using lock_wallpaper_continue_cb=bool (*)(void *context, bool will_read_sd);
bool lock_wallpaper_valid_name(const char *name);
void lock_wallpaper_list(lock_wallpaper_list_t *out,
    lock_wallpaper_continue_cb proceed=nullptr, void *context=nullptr);
// Reads only /sdcard/Aurora/Lockscreen/<validated basename>. Up to 8 MiB per
// file: baseline JPEG <=4096 per edge / 16 Mi pixels; PNG <=1 Mi pixels and
// <=8-bit channels. Decode admission reserves 2 MiB of free PSRAM for playback.
// Progressive JPEG returns UNSUPPORTED. PNG inflation yields in bounded chunks;
// its bounded unfiltering runs at the worker's low scheduler priority.
lock_wallpaper_image_t lock_wallpaper_load(const char *name,
    lock_wallpaper_continue_cb proceed=nullptr, void *context=nullptr);
// Optional startup artwork uses the same bounded, yielding decoder, but keeps
// the original colours. Only /sdcard/Aurora/startup.jpg and startup.png are read;
// a missing or invalid JPG may fall back to PNG. No image is embedded in flash.
lock_wallpaper_image_t startup_artwork_load(
    lock_wallpaper_continue_cb proceed=nullptr, void *context=nullptr);
// The input must be a complete 1280x720 RGB565 main-screen capture. Output is
// downsampled, blurred, tinted and darkened once. Every returned image is native
// 1280x720 RGB565 with the 0x050909 / opacity-90 veil already composited. The UI
// displays it at 1:1 without scaling, antialiasing or a translucent full-screen
// overlay; all expensive preparation remains on this cancellable worker.
lock_wallpaper_image_t lock_wallpaper_frosted(const uint16_t *snapshot,
    size_t stride_pixels=1280, lock_wallpaper_continue_cb proceed=nullptr,
    void *context=nullptr);
void lock_wallpaper_image_free(lock_wallpaper_image_t *image);
const char *lock_wallpaper_error_text(lock_wallpaper_error_t error);
