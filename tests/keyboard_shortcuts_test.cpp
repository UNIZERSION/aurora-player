#ifdef NDEBUG
#undef NDEBUG
#endif
#include "keyboard_shortcuts.hpp"
#include <cassert>
#include <cstdio>

using Shortcut=player::KeyboardShortcut;
static Shortcut mapped(uint32_t key,uint8_t modifiers=0,bool repeat=false) {
    tab5_keyboard_event_t e{};e.key=key;e.modifiers=modifiers;e.repeat=repeat;
    return player::keyboard_shortcut(e);
}
int main() {
    // Printable letters have no media side effects. The controller may send
    // them to search; only the deliberate unmodified Space toggles playback.
    for(uint32_t key=33;key<=126;key++) {
        assert(mapped(key)==Shortcut::None);
        assert(mapped(key,TAB5_KEY_MOD_SHIFT)==Shortcut::None);
        assert(mapped(key,0,true)==Shortcut::None);
    }
    assert(mapped(' ')==Shortcut::PlayPause);
    assert(mapped(' ',0,true)==Shortcut::None);
    assert(mapped(TAB5_KEY_TAB)==Shortcut::NextTab);
    assert(mapped(TAB5_KEY_TAB,TAB5_KEY_MOD_SHIFT)==Shortcut::PreviousTab);
    assert(mapped(TAB5_KEY_TAB,0,true)==Shortcut::None);
    assert(mapped(TAB5_KEY_LEFT)==Shortcut::SeekBack);
    assert(mapped(TAB5_KEY_RIGHT,0,true)==Shortcut::SeekForward);
    assert(mapped(TAB5_KEY_UP)==Shortcut::None);
    assert(mapped(TAB5_KEY_DOWN)==Shortcut::None);
    assert(mapped(TAB5_KEY_ENTER)==Shortcut::None);
    assert(mapped(TAB5_KEY_ESC)==Shortcut::None);
    assert(mapped(TAB5_KEY_UP,TAB5_KEY_MOD_CTRL,true)==Shortcut::VolumeUp);
    assert(mapped(TAB5_KEY_DOWN,TAB5_KEY_MOD_CTRL,true)==Shortcut::VolumeDown);
    assert(mapped(TAB5_KEY_LEFT,TAB5_KEY_MOD_CTRL)==Shortcut::Previous);
    assert(mapped(TAB5_KEY_RIGHT,TAB5_KEY_MOD_CTRL)==Shortcut::Next);
    assert(mapped(TAB5_KEY_LEFT,TAB5_KEY_MOD_CTRL,true)==Shortcut::None);
    assert(mapped(TAB5_KEY_RIGHT,TAB5_KEY_MOD_CTRL,true)==Shortcut::None);
    assert(mapped('f',TAB5_KEY_MOD_CTRL)==Shortcut::Search);
    assert(mapped('F',TAB5_KEY_MOD_CTRL|TAB5_KEY_MOD_SHIFT)==Shortcut::Search);
    assert(mapped(',',TAB5_KEY_MOD_CTRL)==Shortcut::Settings);
    assert(mapped('b',TAB5_KEY_MOD_CTRL)==Shortcut::Bluetooth);
    assert(mapped('M',TAB5_KEY_MOD_CTRL|TAB5_KEY_MOD_SHIFT)==Shortcut::Mute);
    for(uint32_t key=0;key<512;key++) {
        assert(mapped(key,TAB5_KEY_MOD_ALT)==Shortcut::None);
        assert(mapped(key,TAB5_KEY_MOD_ALT|TAB5_KEY_MOD_CTRL)==Shortcut::None);
        if(key!=TAB5_KEY_UP&&key!=TAB5_KEY_DOWN)
            assert(mapped(key,TAB5_KEY_MOD_CTRL,true)==Shortcut::None);
    }
    puts("PASS keyboard shortcut policy: ordinary typing is not playback, bounded repeat policy, explicit Ctrl media/search/settings, Shift tab, Alt blocks shortcuts.");
}
