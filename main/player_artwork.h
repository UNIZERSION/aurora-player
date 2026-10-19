#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Called from the artwork task before SD I/O. Return false while audio is
 * loading/seeking or its PCM reserve is low. Must be thread safe and quick.
 * NULL allows I/O whenever the artwork task is scheduled. */
typedef bool (*player_artwork_io_allowed_cb)(void *context);
bool player_artwork_init(player_artwork_io_allowed_cb io_allowed,void *context);
/* Nonblocking, newest request wins. NULL cancels and restores the dragon art.
 * Call from the controller task; copied path is limited to511 UTF-8 bytes. */
bool player_artwork_request(const char *track_path);
/* Controller-thread hint: a delivered missing/transient result may be retried
 * after30s. A new request, a positive result or stop resets this deadline. */
bool player_artwork_needs_retry(void);
/* Low-priority viewport work, max8 paths of <=511 UTF-8 bytes. RAM-only copy,
 * safe under display lock. New viewports debounce180ms; identical/subset polls
 * don't restart work. count0 cancels background. Poll delivers copied56x56 UI
 * thumbnails without changing the main cover. Same controller thread as request. */
bool player_artwork_request_thumbnails(const char *const *paths,size_t count);
/* Cancel pending/active work and wait until the worker acknowledges that all
 * prior image loads have returned and closed their SD descriptors. False means
 * it is NOT safe to unmount yet. Call outside the LVGL lock, from the same
 * controller thread as request(); issue no new requests before unmounting.
 * The worker stays alive so a cancelled shutdown can resume normal requests.
 * This also invalidates decoded/negative RAM caches: call on library updates
 * or SD replacement. Positive cache hits intentionally perform no stat/SD I/O. */
bool player_artwork_stop_and_wait(uint32_t timeout_ms);
/* Call periodically while holding the LVGL/display lock. No disk I/O or
 * image decoding occurs here; it only swaps the prepared native RGB565 cover. */
void player_artwork_poll(void);

#ifdef __cplusplus
}
#endif
