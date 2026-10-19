#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Printable keys use their Unicode value (A164's US layout emits ASCII).
 * HID modifiers are translated by the module's firmware, including Aa/Sym. */
enum {
    TAB5_KEY_ENTER = 0x100, TAB5_KEY_ESC, TAB5_KEY_BACKSPACE, TAB5_KEY_DELETE,
    TAB5_KEY_TAB, TAB5_KEY_LEFT, TAB5_KEY_RIGHT, TAB5_KEY_UP, TAB5_KEY_DOWN,
    TAB5_KEY_HOME, TAB5_KEY_END, TAB5_KEY_PAGE_UP, TAB5_KEY_PAGE_DOWN
};
enum { TAB5_KEY_MOD_CTRL = 1, TAB5_KEY_MOD_SHIFT = 2, TAB5_KEY_MOD_ALT = 4 };
typedef struct {
    uint32_t key;
    uint8_t modifiers;
    bool repeat;
} tab5_keyboard_event_t;

/* Protocol state is single-worker owned. No allocation, UI or hardware access. */
typedef struct {
    tab5_keyboard_event_t held;
    uint32_t pressed_at, repeat_at;
    bool repeating;
} tab5_keyboard_protocol_t;

void tab5_keyboard_protocol_reset(tab5_keyboard_protocol_t *state);
bool tab5_keyboard_protocol_hid(tab5_keyboard_protocol_t *state, uint8_t modifier,
                               uint8_t usage, uint32_t now_ms, tab5_keyboard_event_t *event);
bool tab5_keyboard_protocol_repeat(tab5_keyboard_protocol_t *state, uint32_t now_ms,
                                  tab5_keyboard_event_t *event);

#ifdef __cplusplus
}
#endif
