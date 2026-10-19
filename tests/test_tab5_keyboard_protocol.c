#include "tab5_keyboard_protocol.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>

static tab5_keyboard_protocol_t state;
static tab5_keyboard_event_t event;
static void key(uint8_t mod, uint8_t usage, uint32_t expected)
{
    assert(tab5_keyboard_protocol_hid(&state, mod, usage, 100, &event));
    assert(event.key == expected && !event.repeat);
}
int main(void)
{
    tab5_keyboard_protocol_reset(&state);
    for (unsigned i=0; i<26; ++i) { key(0, 4+i, 'a'+i); key(2, 4+i, 'A'+i); }
    const char *digits="1234567890", *shifted="!@#$%^&*()";
    for (unsigned i=0; i<10; ++i) { key(0, 0x1e + i, digits[i]); key(2, 0x1e + i, shifted[i]); }
    const char *symbols="-=[]\\#;'`,./", *symshift="_+{}|~:\"~<>?";
    for (unsigned i=0; i<12; ++i) { key(0, 0x2d+i, symbols[i]); key(2, 0x2d+i, symshift[i]); }
    key(0,0x2c,' '); key(0,0x28,TAB5_KEY_ENTER); key(0,0x29,TAB5_KEY_ESC);
    key(0,0x2a,TAB5_KEY_BACKSPACE); key(0,0x2b,TAB5_KEY_TAB); key(0,0x4c,TAB5_KEY_DELETE);
    key(0,0x50,TAB5_KEY_LEFT); key(0,0x4f,TAB5_KEY_RIGHT);
    key(0,0x52,TAB5_KEY_UP); key(0,0x51,TAB5_KEY_DOWN);
    key(0,0x4a,TAB5_KEY_HOME); key(0,0x4d,TAB5_KEY_END);
    key(0,0x4b,TAB5_KEY_PAGE_UP); key(0,0x4e,TAB5_KEY_PAGE_DOWN);
    /* Aa (including one-shot/caps) and Sym arrive as the device's HID Shift.
     * Exact printed secondary choices: ? / < > { } | : double-quote , =. */
    key(2,0x38,'?'); key(0,0x38,'/'); key(2,0x36,'<'); key(2,0x37,'>');
    key(2,0x2f,'{'); key(2,0x30,'}'); key(2,0x31,'|'); key(2,0x33,':');
    key(2,0x34,'"'); key(0,0x36,','); key(0,0x2e,'=');
    key(1,9,'f'); assert(event.modifiers==TAB5_KEY_MOD_CTRL && !state.repeating);
    key(4,9,'f'); assert(event.modifiers==TAB5_KEY_MOD_ALT && !state.repeating);
    key(0x70,9,'F'); assert(event.modifiers==7 && !state.repeating);
    key(0,4,'a');
    assert(!tab5_keyboard_protocol_repeat(&state,549,&event));
    assert(tab5_keyboard_protocol_repeat(&state,550,&event) && event.repeat && event.key=='a');
    assert(!tab5_keyboard_protocol_repeat(&state,609,&event));
    assert(tab5_keyboard_protocol_repeat(&state,610,&event));
    assert(!tab5_keyboard_protocol_hid(&state,0xff,0xff,620,&event));
    assert(tab5_keyboard_protocol_repeat(&state,670,&event));
    assert(!tab5_keyboard_protocol_hid(&state,0,0,680,&event));
    assert(!tab5_keyboard_protocol_repeat(&state,1000,&event));
    key(0,4,'a'); key(0,5,'b'); /* Conservative cancellation on any release. */
    assert(!tab5_keyboard_protocol_hid(&state,0,0,110,&event));
    assert(!tab5_keyboard_protocol_repeat(&state,800,&event));
    key(0,4,'a');
    assert(tab5_keyboard_protocol_repeat(&state,5000,&event));
    assert(!tab5_keyboard_protocol_repeat(&state,5001,&event)); /* no catch-up */
    assert(!tab5_keyboard_protocol_repeat(&state,10100,&event)); /* lost release cap */
    key(0,0x28,TAB5_KEY_ENTER); assert(!tab5_keyboard_protocol_repeat(&state,1000,&event));
    key(0,0x29,TAB5_KEY_ESC); assert(!tab5_keyboard_protocol_repeat(&state,1000,&event));
    key(0,0x2b,TAB5_KEY_TAB); assert(!tab5_keyboard_protocol_repeat(&state,1000,&event));
    for (unsigned usage=0; usage<256; ++usage) {
        key(0,4,'a');
        if (usage<4 || usage>0x52) {
            assert(!tab5_keyboard_protocol_hid(&state,0,usage,110,&event));
            assert(!state.repeating);
        }
    }
    assert(tab5_keyboard_protocol_hid(&state,0,4,UINT32_MAX-100,&event));
    assert(!tab5_keyboard_protocol_repeat(&state,348,&event));
    assert(tab5_keyboard_protocol_repeat(&state,349,&event));
    tab5_keyboard_protocol_reset(&state);
    assert(!tab5_keyboard_protocol_repeat(&state,1000,&event));
    puts("PASS: A164 HID ASCII/Aa/Sym/modifiers, navigation, release/rollover, repeat timing/wrap/stall/disconnect reset");
    return 0;
}
