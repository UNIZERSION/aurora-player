#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLAYER_UI_TRACK_SPEED, PLAYER_UI_PLAY_PAUSE, PLAYER_UI_SCREENSHOT, PLAYER_UI_PREVIOUS, PLAYER_UI_NEXT,
    PLAYER_UI_SEEK_MS, PLAYER_UI_VOLUME, PLAYER_UI_MUTE,
    PLAYER_UI_SHUFFLE, PLAYER_UI_REPEAT, PLAYER_UI_SELECT_TRACK,
    PLAYER_UI_FOLDER, PLAYER_UI_SORT, PLAYER_UI_VIEW,
    PLAYER_UI_VISUAL_MODE, PLAYER_UI_SETTINGS, PLAYER_UI_WIFI,
    PLAYER_UI_BLUETOOTH, PLAYER_UI_FILE_INFO, PLAYER_UI_SEARCH,
    PLAYER_UI_FAVORITE, PLAYER_UI_LIBRARY_TAB, PLAYER_UI_FORMAT_FILTER, PLAYER_UI_EFFECT,
    PLAYER_UI_DRAGON_TYPE, PLAYER_UI_DRAGON_SPEED, PLAYER_UI_DRAGON_COLORS,
    PLAYER_UI_BRIGHTNESS, PLAYER_UI_ANIMATIONS, PLAYER_UI_CLOCK_FORMAT,
    PLAYER_UI_RESET_VISUAL, PLAYER_UI_ADVANCED, PLAYER_UI_DIM_TIME, PLAYER_UI_COVERS, PLAYER_UI_RESCAN, PLAYER_UI_SAVE, PLAYER_UI_POWER_OFF, PLAYER_UI_SET_CLOCK, PLAYER_UI_SORT_SET, PLAYER_UI_FOLDER_SELECT,
    PLAYER_UI_LOCK_ENABLE, PLAYER_UI_LOCK_CHANGE, PLAYER_UI_LOCK_DISABLE,
    PLAYER_UI_LOCK_BACKGROUND, PLAYER_UI_LOCK_BACKGROUND_SELECT, PLAYER_UI_LOCK_INPUT,
    PLAYER_UI_REVERB_SETTINGS
} player_ui_action_t;
typedef void (*player_ui_action_cb)(player_ui_action_t action, int32_t value, void *context);

typedef struct {
    const char *title;
    const char *artist;
    uint32_t duration_ms;
    bool favorite;
    const char *path; /* Used only to match already decoded thumbnail cache. */
} player_ui_track_t;

typedef struct {
    const char *title, *artist, *album, *file_name, *format, *modified;
    const char *clock_text, *date_text, *firmware;
    uint32_t position_ms, duration_ms, sample_rate;
    uint64_t file_bytes, sd_used_bytes, sd_total_bytes;
    size_t track_index, track_count;
    uint8_t bit_depth, volume, repeat, visual_mode, speed_percent;
    int16_t battery_percent; /* -1 = unavailable */
    bool playing, shuffle, muted, wifi_connected, grid_view, sd_free_known;
    bool battery_charging, battery_estimated, bluetooth_available, bluetooth_enabled, bluetooth_connected;
    uint32_t bitrate_kbps;
    uint8_t effect;        /* 0 Normal, 1 Bass+, 2 Reverb, 3 Custom EQ */
    uint8_t library_tab;   /* 0 Queue, 1 Folders on SD, 2 Favorites */
    uint8_t format_filter; /* 0 All, 1 MP3, 2 WAV, 3 FLAC, 4 AAC */
    bool favorite;
    const char *artwork_key; /* Set to current path before swapping its cover. */
    uint8_t dragon_type, dragon_speed, brightness; /* 0 living / 1 flight; speed in tenths */
    bool dragon_colors, animations, clock24;
    uint32_t dim_seconds;uint16_t battery_mv;int16_t battery_ma;
    uint8_t sort;bool covers,headphones,headphones_known,battery_charging_known;
    const char *folder_label; /* Queue header; copied during set_state. NULL uses SD COLLECTION. */
} player_ui_state_t;

typedef struct {
    uint32_t frames, last_ms, max_ms;
    uint64_t total_ms;
    uint32_t spectrum_updates, skipped_spectrum_updates;
    uint32_t scroll_frames, slow_frames, stall_frames;
    uint32_t scroll_slow_frames, scroll_stall_frames, interval_max_ms;
    uint64_t scroll_total_ms;
} player_ui_render_stats_t;

/* All calls must run on the LVGL thread or hold the LVGL port lock.
 * The UI copies label strings synchronously; callers retain ownership.
 * The playlist copies title, artist, duration and favorite; it creates only
 * eight visible row widgets even for a 10000-track library.
 * Native layout targets the Tab5's 1280 x 720 landscape display.
 */
void player_ui_create(lv_obj_t *parent, player_ui_action_cb callback, void *context);
void player_ui_set_state(const player_ui_state_t *state);
void player_ui_set_playlist(const player_ui_track_t *tracks, size_t count, size_t current);
/* Prepare/free outside the display lock. Apply swaps RAM ownership under the
 * lock and only updates the eight visible widgets; the handle then owns old rows. */
typedef struct player_ui_playlist player_ui_playlist_t;
player_ui_playlist_t *player_ui_prepare_playlist(const player_ui_track_t *tracks,size_t count);
void player_ui_apply_playlist(player_ui_playlist_t *prepared,size_t current);
void player_ui_free_playlist(player_ui_playlist_t *prepared);
/* Embedded Files view. Prepare/free copy or release folder names outside the
 * display lock. Apply swaps ownership under the lock, updates at most eight
 * rows and preserves scrolling unless selection changes or the list shrinks.
 * A short row tap emits PLAYER_UI_FOLDER_SELECT with this array's index. */
typedef struct player_ui_folders player_ui_folders_t;
player_ui_folders_t *player_ui_prepare_folders(const char *const *names,size_t count);
void player_ui_apply_folders(player_ui_folders_t *prepared,size_t selected);
void player_ui_free_folders(player_ui_folders_t *prepared);
/* Update metadata discovered on playback without rebuilding the playlist.
 * Invalid indices/NULL are ignored; only an affected visible row is repainted. */
void player_ui_update_track(size_t index, const player_ui_track_t *track);
/* Normalized bands [0,1]. Waveform is signed PCM16; either input may be NULL. */
void player_ui_set_spectrum(const float *bands, size_t count, const int16_t *wave, size_t wave_count);
/* Optional real-time source sampled by the shared animation timer. Returns
 * true while audio is playing. Called under the UI lock; it must not block on I/O. */
typedef bool (*player_ui_visual_source_cb)(float bands[32],int16_t wave[128],void *context);
void player_ui_set_visual_source(player_ui_visual_source_cb callback,void *context);
void player_ui_set_notice(const char *message);
/* Under the UI lock, validate a queued main-cover result against the path key
 * currently published by set_state. Nonempty paths also require covers=ON.
 * NULL/empty means explicit clear and is allowed regardless of selected track.
 * Workers must defer a current-generation result until this returns true; a
 * NULL image for track B still needs to validate B, not a NULL path. */
bool player_ui_accepts_artwork(const char *path);
/* Native RGB565 cover, maximum312x312. The caller retains the descriptor and
 * pixels until replaced. NULL restores the original dragon artwork. */
void player_ui_set_artwork(const lv_image_dsc_t *image);
/* Visible playlist indices, not library IDs. Map/copy their paths before changing
 * the playlist and while holding the same UI lock. At most eight are returned;
 * duplicate paths, hidden/covered views, sleep and covers=OFF are skipped.
 * No file access or allocation occurs here. */
size_t player_ui_get_missing_thumbnails(size_t *indices,size_t capacity);
/* Copies/scales RGB565 pixels (maximum312x312) immediately; caller may release
 * image after returning. Updates rows for path independently of the playing
 * track. NULL caches a failed lookup for60s, then permits retry. */
void player_ui_set_thumbnail(const char *path,const lv_image_dsc_t *image);
/* Reset both positive and negative results after remount/library changes. */
void player_ui_clear_thumbnails(void);
/* Cumulative render timings. Read under the same LVGL lock as other UI calls.
 * Timings include refresh work and synchronous driver waits, not final DMA completion. */
void player_ui_get_render_stats(player_ui_render_stats_t *stats);
lv_obj_t *player_ui_root(void);
void player_ui_set_overlay(bool visible);
void player_ui_set_sleeping(bool sleeping);
void player_ui_open_settings(void);
void player_ui_open_display_settings(void);
/* Open Files from a menu/settings action, without emitting another action. */
void player_ui_open_files(void);
/* Lock flow is controller-owned. PIN input emits PLAYER_UI_LOCK_INPUT with
 * 0..9, -1 for backspace or -2 for Cancel (only when can_cancel is true).
 * These calls copy text and never retain or display the entered PIN. */
void player_ui_set_lock_preferences(bool enabled,const char *background_name);
void player_ui_set_lock_clock(const char *clock_text);
void player_ui_show_lock_pin(const char *title,const char *message,unsigned digit_count,bool can_cancel,bool input_enabled);
void player_ui_show_lock_backgrounds(const char *const *names,size_t count,int selected,const char *status);
void player_ui_close_lock_panel(void);
bool player_ui_lock_panel_visible(void);
/* Close settings/menu, hide the lock panel and freeze visual animations before
 * the controller captures the main panel. The controller must capture and
 * restore a lock panel under one LVGL lock, before allowing a display refresh. */
void player_ui_prepare_lock_capture(void);
/* Native 1280x720 RGB565 image with its veil already composited by the worker.
 * Drawn without scaling. Caller retains descriptor and
 * pixels until replaced. NULL selects the opaque loading/fallback background. */
void player_ui_set_lock_background(const lv_image_dsc_t *image);
/* Physical keyboard navigation. Call under the LVGL lock. Standard LV_KEY_*
 * values select/activate virtual rows and settings; these two values add paging.
 * Returns true only when the visible UI consumed the key. NEXT/PREV navigate
 * settings/menu controls and return false on the main library tabs. */
enum { PLAYER_UI_KEY_PAGE_UP=0x10001, PLAYER_UI_KEY_PAGE_DOWN=0x10002 };
bool player_ui_keyboard(uint32_t key);
/* Printable keys may open search only when no settings/menu/overlay is active. */
bool player_ui_keyboard_text_allowed(void);
lv_obj_t *player_ui_create_folder_list(lv_obj_t *parent,const char *const *names,size_t count,size_t selected);

#ifdef __cplusplus
}
#endif
