#pragma once
#include "tab5_keyboard.h"

namespace player {
enum class KeyboardShortcut {
    None, PlayPause, Previous, Next, VolumeUp, VolumeDown, SeekBack, SeekForward,
    Search, NextTab, PreviousTab, Settings, Bluetooth, Mute
};
// Called only outside dialogs, after the focused UI has handled navigation.
inline KeyboardShortcut keyboard_shortcut(const tab5_keyboard_event_t &event) {
    const uint32_t key=event.key;
    const bool ctrl=(event.modifiers&TAB5_KEY_MOD_CTRL)!=0;
    const bool shift=(event.modifiers&TAB5_KEY_MOD_SHIFT)!=0;
    if(event.modifiers&TAB5_KEY_MOD_ALT)return KeyboardShortcut::None;
    if(ctrl){
        if(key==TAB5_KEY_UP)return KeyboardShortcut::VolumeUp;
        if(key==TAB5_KEY_DOWN)return KeyboardShortcut::VolumeDown;
        if(event.repeat)return KeyboardShortcut::None;
        if(key==TAB5_KEY_LEFT)return KeyboardShortcut::Previous;
        if(key==TAB5_KEY_RIGHT)return KeyboardShortcut::Next;
        if(key=='f'||key=='F')return KeyboardShortcut::Search;
        if(key==',')return KeyboardShortcut::Settings;
        if(key=='b'||key=='B')return KeyboardShortcut::Bluetooth;
        if(key=='m'||key=='M')return KeyboardShortcut::Mute;
        return KeyboardShortcut::None;
    }
    if(key==TAB5_KEY_LEFT)return KeyboardShortcut::SeekBack;
    if(key==TAB5_KEY_RIGHT)return KeyboardShortcut::SeekForward;
    if(event.repeat)return KeyboardShortcut::None;
    if(key==' ')return KeyboardShortcut::PlayPause;
    if(key==TAB5_KEY_TAB)return shift?KeyboardShortcut::PreviousTab:KeyboardShortcut::NextTab;
    return KeyboardShortcut::None;
}
}
