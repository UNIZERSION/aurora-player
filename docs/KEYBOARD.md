# M5Stack Tab5 Keyboard (A164)

Aurora works with the optional M5Stack Tab5 Keyboard (A164). This page lists every key as of
version 1.3.21. The keyboard was added in 1.3.10; the hardware side (I2C protocol, timing,
recovery) is described in [HARDWARE-KEYBOARD-1.3.10.md](HARDWARE-KEYBOARD-1.3.10.md).

## Connecting

- Plug the keyboard into **Ext.Port1**. Aurora finds it by itself within about 2 seconds, also
  when it is attached or reattached while Aurora runs. There is no pairing and no USB or
  Bluetooth keyboard mode.
- There is no on-screen "keyboard connected" indicator; the keys simply start working.
- Layout: US, ASCII only (no Polish letters). **Aa** gives capitals and **Sym** the symbols
  printed on the keys; both are handled inside the keyboard. Aa, Sym, Ctrl or Alt pressed alone
  send nothing.
- Holding a key repeats it after 450 ms, then every 60 ms: printable characters (letters,
  digits, symbols, Space), Backspace, Delete, the arrows and Home / End / PgUp / PgDn. Enter, Esc,
  Tab and every combination with Ctrl or Alt never repeat. A held key stops repeating after 10 s.
  Where a repeat makes no sense, Aurora ignores it: a held Space toggles play / pause only once
  (it repeats only as text in Search), and on the PIN lock screen and the lock background list a
  held key acts only once.
- **Alt** blocks the shortcuts: Alt + a letter, digit, Space, arrow or Ctrl shortcut does
  nothing. Exceptions: Esc and Enter in Search, and Esc in dialogs, still work while Alt is
  held.
- Aurora also handles Home, End, PgUp, PgDn and Shift + Tab if the keyboard sends them. The A164
  has no dedicated keys for them, so they may not be available on it.

## Main screen

These work when no dialog, Settings or menu is open.

| Key | Action |
|---|---|
| Space | Play / pause. If nothing is loaded, plays the current track (or the first one), from the saved position if it was the last track played. While a track is still loading, pauses it. |
| Ctrl + ← | Previous: more than 3 s into a track, restarts it; otherwise the previous track (shuffle history first, then the queue). On the first track it wraps to the last only with Repeat All, otherwise it restarts the first track. |
| Ctrl + → | Next track (follows Shuffle). At the end of the queue it wraps only with Repeat All. |
| ← / → | Seek 5 s back / forward. Hold to keep seeking. Works on a playing or paused track; otherwise shows *Start playback before seeking*. |
| Ctrl + ↑ / Ctrl + ↓ | Volume +5 / −5 (0–100) and mute off. With Bluetooth headphones connected it changes their volume. |
| Ctrl + M | Mute on / off. |
| Tab | Next library tab: Queue → Files → Favorites. |
| Shift + Tab | Previous library tab. |
| ↑ / ↓, Enter, Esc | List navigation, see below. |
| A letter, digit or symbol | Opens Search with that character as the new query. |
| Ctrl + F | Opens Search with the current query. |
| Ctrl + , | Opens Settings (without Aa: Shift turns `,` into `<`). |
| Ctrl + B | Opens the Bluetooth window. |

Next and Previous keep the player's state: if it is paused, stopped (nothing played yet since
start) or finished, the new track is loaded paused and Space starts it. Repeat One applies only
when a track ends by itself.

## Lists (Queue, Files, Favorites)

| Key | Action |
|---|---|
| ↓ / ↑ | Moves a lime highlight one row. The first press starts next to the playing track (Queue, Favorites) or the selected folder (Files), or at the first row. The music does not change. |
| Home / End | First / last row. |
| PgUp / PgDn | 4 rows (Queue, Favorites) or 6 rows (Files). |
| Enter | Queue or Favorites: plays the highlighted track from the start (from Favorites, the favorites become the queue). Files: opens the folder in Queue and clears the search filter; the current track keeps playing. Next and Previous now follow the new folder, except when playback was started from Favorites: then they keep following the favorites until you play a track from the Queue. |
| Esc | Removes the highlight (touching the screen does too). |

With no highlight, Enter acts on the playing track or the selected folder, so in Queue it
**restarts the playing track** from 0:00. ←, →, Space, Tab and the Ctrl shortcuts work as on the
main screen while a row is highlighted.

## Search

| Key | Action |
|---|---|
| Letters, digits, symbols, Space | Type at the cursor (up to 96 characters). |
| Backspace / Delete | Delete left / right of the cursor. |
| ← / → | Move the cursor. Home / End: start / end. |
| Enter | Apply the search and close: the list is filtered (*N results*). An empty query removes the filter. |
| Esc | Close without applying; the previous filter stays. |
| Ctrl + U | Clear the text (then Enter removes the filter). |

Player shortcuts do not work while Search is open. The on-screen keyboard and the CLEAR FILTER /
SEARCH buttons stay available for touch.

## Settings

| Key | Action |
|---|---|
| ↓ or Tab / ↑ or Shift + Tab | Next / previous control; wraps around. Disabled controls are skipped, and the Back button is not in the order (use Esc). |
| ← / → | On a slider: change the value (Brightness ±5 %, 10–100 %; Dragon speed ±0.1×, 0.4–1.6×; Turntable speed ±1 %, 75–125 %). On a button: move like ↑ / ↓. |
| Home or PgUp / End or PgDn | First / last control. |
| Enter | Press the highlighted control. A category opens its page and highlights its first control. With nothing highlighted, the first Enter highlights **and presses** the first control. |
| Esc | From a page, back to the categories; from the categories, close Settings. |

With nothing highlighted, any navigation key highlights the first control (End the last).
**Enter on Power off shuts the Tab5 down at once**, with no confirmation. Space, letters and the
player shortcuts do nothing while Settings is open.

## Dialogs

Bluetooth, AirPods, EQ, Reverb settings, Set date and time, File / Output information:

| Key | Action |
|---|---|
| ↓, → or Tab / ↑, ← or Shift + Tab | Next / previous control; wraps around. |
| Home / End | First / last control. |
| ← / → on a slider | One step: EQ band ±1 dB (−12 to +12), Reverb amount ±1 %, decay ±0.1 s, high / low cut ±100 Hz. ↑ / ↓ never change a slider, not even the vertical EQ bands: they move to the previous / next control. |
| ← / → on a number field | −1 / +1 (Set date and time: year, month, day, hour, minute). |
| Enter | The first Enter only shows the highlight on the first control and presses nothing. After that it presses the highlighted button or opens the highlighted list. |
| Esc | Close the dialog (an open list closes first). The power-off dialogs cannot be closed. |

- In the Bluetooth, date-and-time and information dialogs the first control is CLOSE, so
  Enter, Enter closes them. In Reverb settings it is the Reverb ON/OFF button, in EQ the preset
  list.
- EQ preset list: the arrows move the selection, Enter applies the preset, Esc closes the list
  without a change.
- BT ON/OFF and SCAN rebuild the Bluetooth window, so the highlight is lost; a scan that finds
  new devices also rebuilds the device rows. Navigate again before pressing Enter.

## Track menu

The menu opened by a long press on a track row (touch only) can be used with the keys: the
arrows, Tab and Shift + Tab move (wrapping), Home or PgUp go to the first item, End or PgDn to
the last, Enter runs the item and Esc closes the menu. With nothing highlighted, the first Enter
runs the first item (Add or Remove favorite).

## PIN lock screen

| Key | Action |
|---|---|
| 0–9 (without Aa) | Type a digit. The fourth digit is checked at once; there is no confirm key. |
| Backspace / Delete | Delete the last digit. |
| Esc | Cancel when creating, confirming or changing the PIN or disabling the screen lock. On the normal unlock screen it does nothing. |
| Enter | Nothing. |

A wrong PIN clears the dots and shows *Incorrect PIN. Try again.*; after 5 wrong PINs digits are
ignored for 30 s (*Too many attempts*). All other keys and every player shortcut are ignored on
the lock screen. While the screen is locked, the music can still be controlled with headphone
buttons, AirPods ear detection and, with the screen off, by tapping the enclosure (3 taps pause,
2 taps resume; see [TAP-CONTROLS.md](TAP-CONTROLS.md)).

**Lock background** list (*Settings > Display > Lock background*): the arrows and Tab move,
Home / End / PgUp / PgDn jump, Enter selects the highlighted background and Esc returns to
Settings. Selecting rebuilds the list and removes the highlight, so move it again before the next
Enter (an Enter with nothing highlighted selects *Frosted glass (default)*).

## Screen off, startup and power-off

- When the screen is off, any key (a letter, Space, Enter, Esc, an arrow…) turns it on. That key
  does nothing else; with the PIN lock on, the keypad appears and the key is not typed as a
  digit. Holding it does not repeat: release and press again. Aa, Sym, Ctrl or Alt alone do not
  wake the screen. The key also cancels a series of enclosure taps in progress.
- Every key that reaches the player restarts the idle timer behind *Screen off after* and the
  idle PIN lock.
- Keys pressed during the start-up logo and library loading are discarded.
- After Power off has been requested every key is ignored.

## Changes since 1.3.10

- 1.3.11: sliders, number fields and the EQ preset list in dialogs.
- 1.3.13: PIN lock screen and lock background list.
- 1.3.18 (and again in 1.3.21, when tap controls returned): a key that wakes the screen cancels
  a series of enclosure taps in progress.
- 1.3.19: the keyboard is detected with an ordinary register read; the earlier
  `i2c_master_probe` caused random restarts in 1.3.10–1.3.18, even with no keyboard attached.
- 1.3.20: repeated seeks add up, also while paused; key presses also delay the idle PIN lock
  while the screen stays lit.
