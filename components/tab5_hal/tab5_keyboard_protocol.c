#include "tab5_keyboard_protocol.h"
#include <string.h>

void tab5_keyboard_protocol_reset(tab5_keyboard_protocol_t *state)
{
    memset(state, 0, sizeof(*state));
}

static uint32_t translate(uint8_t usage, bool shift)
{
    if (usage >= 0x04 && usage <= 0x1d) return (shift ? 'A' : 'a') + usage - 0x04;
    if (usage >= 0x1e && usage <= 0x27)
        return (uint8_t)(shift ? "!@#$%^&*()" : "1234567890")[usage - 0x1e];
    if (usage >= 0x2d && usage <= 0x38) {
        /* 0x32 is the non-US # key; A164 normally uses the US backslash. */
        return (uint8_t)(shift ? "_+{}|~:\"~<>?" : "-=[]\\#;'`,./")[usage - 0x2d];
    }
    switch (usage) {
    case 0x28: return TAB5_KEY_ENTER;
    case 0x29: return TAB5_KEY_ESC;
    case 0x2a: return TAB5_KEY_BACKSPACE;
    case 0x2b: return TAB5_KEY_TAB;
    case 0x2c: return ' ';
    case 0x4a: return TAB5_KEY_HOME;
    case 0x4b: return TAB5_KEY_PAGE_UP;
    case 0x4c: return TAB5_KEY_DELETE;
    case 0x4d: return TAB5_KEY_END;
    case 0x4e: return TAB5_KEY_PAGE_DOWN;
    case 0x4f: return TAB5_KEY_RIGHT;
    case 0x50: return TAB5_KEY_LEFT;
    case 0x51: return TAB5_KEY_DOWN;
    case 0x52: return TAB5_KEY_UP;
    default: return 0;
    }
}

bool tab5_keyboard_protocol_hid(tab5_keyboard_protocol_t *state, uint8_t modifier,
                               uint8_t usage, uint32_t now_ms, tab5_keyboard_event_t *event)
{
    /* Empty FIFO is not a release. Firmware sends usage0 for any ordinary
     * key release, without identifying the released key. Stop all repeats
     * conservatively, including when several keys were held simultaneously. */
    if (modifier == 0xff && usage == 0xff) return false;
    tab5_keyboard_protocol_reset(state);
    if (!usage || (modifier & 0x88)) return false; /* No GUI keys on A164. */
    uint8_t mods = 0;
    if (modifier & 0x11) mods |= TAB5_KEY_MOD_CTRL;
    if (modifier & 0x22) mods |= TAB5_KEY_MOD_SHIFT;
    if (modifier & 0x44) mods |= TAB5_KEY_MOD_ALT;
    const uint32_t key = translate(usage, (mods & TAB5_KEY_MOD_SHIFT) != 0);
    if (!key) return false;
    *event = (tab5_keyboard_event_t){ .key = key, .modifiers = mods, .repeat = false };
    state->held = *event;
    state->pressed_at = now_ms;
    state->repeat_at = now_ms + 450;
    state->repeating = !(mods & (TAB5_KEY_MOD_CTRL | TAB5_KEY_MOD_ALT)) &&
        ((key >= 32 && key <= 126) || key == TAB5_KEY_BACKSPACE || key == TAB5_KEY_DELETE ||
         (key >= TAB5_KEY_LEFT && key <= TAB5_KEY_PAGE_DOWN));
    return true;
}

bool tab5_keyboard_protocol_repeat(tab5_keyboard_protocol_t *state, uint32_t now_ms,
                                  tab5_keyboard_event_t *event)
{
    if (!state->repeating) return false;
    /* Bounds damage from a lost release/FIFO overflow. A fresh press restarts
     * normally; disconnect/error resets this state immediately in the worker. */
    if (now_ms - state->pressed_at >= 10000) {
        state->repeating = false;
        return false;
    }
    if ((int32_t)(now_ms - state->repeat_at) < 0) return false;
    *event = state->held;
    event->repeat = true;
    state->repeat_at = now_ms + 60; /* No catch-up burst after UI/CPU stalls. */
    return true;
}
