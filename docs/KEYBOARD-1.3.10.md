# Aurora 1.3.10 — M5Stack Tab5 Keyboard A164

Connect the keyboard to Ext.Port1. Aurora detects the default I²C address 0x6D automatically at boot or after connection. No pairing, USB keyboard mode or Pico update is required. The main player artwork and touch controls are preserved; a thin focus outline appears when navigating by keyboard and disappears on touch or Escape.

## Controls

| Key | Player behavior |
| --- | --- |
| Letters, digits, punctuation | Open Search and start a new query |
| Ctrl+F | Open Search with the current query |
| Space | Play/pause; holding does not toggle repeatedly |
| Ctrl+Left / Ctrl+Right | Previous / next track, once per press |
| Ctrl+Up / Ctrl+Down | Volume +5 / −5 percentage points, once per press |
| Left / Right | Seek backward / forward by 5 seconds where supported |
| Up / Down | Move focus in Queue, Files or Favorites |
| Enter | Select focused track/folder; selecting a folder opens its Queue |
| Tab | Cycle Queue → Files → Favorites |
| Escape | Clear list focus; go back/close a settings page or closable dialog |
| Ctrl+, | Open Settings |
| Ctrl+B | Open Bluetooth |
| Ctrl+M | Toggle mute |

While Search is open, letters/numbers/symbols and Space are text only. Backspace/Delete edit, Left/Right move the cursor, Enter applies the query, Escape closes without applying changes, Ctrl+U clears the text. The on-screen keyboard remains available for touch. The keyboard's Aa and Sym behavior follows its own firmware and the printed US layout; this update does not add a Polish AltGr layout.

In Settings, Tab or Up/Down moves between controls, Enter activates, and Left/Right changes the focused slider. Escape returns to the settings categories, then closes Settings. Other dialogs support arrow/Tab focus and Enter; the first Enter reveals focus without activating an unseen control. Player shortcuts are suppressed inside dialogs/settings and during shutdown.

The protocol also recognizes Home/End/PageUp/PageDown and Shift+Tab for compatible reports and tests. The A164 has no dedicated Home/End/Page keys; its Aa key is not a general Shift key for Tab, so these reports are not promised as physical A164 shortcuts.

## Wake and input timing

The first ordinary key (for example A, Space, Enter or an arrow) wakes an inactive display and is consumed. Aa, Ctrl, Alt and Sym alone do not wake it: the keyboard firmware consumes those modifier presses internally and sends no HID event. Repeats of the wake key are also consumed; release and press again to control the player. Every accepted key refreshes the inactivity timer. Text/navigation repeat starts at 450 ms, then 60 ms; Enter, Tab, Escape and Ctrl/Alt shortcuts do not repeat. Any release conservatively stops repeat because the keyboard's HID reports do not identify the released key. A 10-second bound prevents indefinite repeat after a lost release.

The controller consumes one queued key per iteration to preserve the order of UI/model changes and avoid a backlog monopolizing the player. It reads the queue before acquiring the display lock; only a pending key waits up to 100 ms for that lock. A busy search dialog cannot repeatedly starve the old 2 ms try-lock. A pending event is retained across retries, discarded on disconnect or after two seconds, and never fetched twice. The first eight non-repeated inputs in each Search dialog log modifier flags and before/after character counts, never the query text. List focus uses existing virtual rows, including 10,000-entry models, without creating one object per item. Disconnect, malformed FIFO data and queue overflow clear pending input.

## Verification scope

Host tests pass for the real HID parser, mocked production I²C/RTOS worker and extracted production controller routing: input/editing, modifiers, media shortcut isolation, modal focus, wake/repeat, shutdown guard, disconnected input, bounded probes, queue overflow and invalid reports.

A dedicated native LVGL test passed navigation of 10,000 Queue/Files/Favorites entries, model replacement, empty lists, boundaries, settings sliders, focus visibility and pixel restoration after Escape. Three additional focus-clear touch handlers and compiler-only brace fixes were added afterwards; that host executable was not rerun against those final edits. The complete existing 62-action native regression compiled, but Windows App Control blocked execution. This is recorded as unexecuted, not passed.

Physical installation/keyboard checks for the exact final Application hash were recorded separately and are not published. Historical 1.3.9 results are not claims that every feature was retested in 1.3.10. Standalone is packaged separately and is not the image layout tested in the existing M5Launcher slot.

See [hardware design and primary sources](HARDWARE-KEYBOARD-1.3.10.md) for register semantics, bus isolation and worker timing.
