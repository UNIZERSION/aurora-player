# Changelog

All notable changes to Aurora Player, newest first. Unless stated otherwise, each update keeps the partition layout, settings, favourites, PIN storage and library cache format of the previous version.

## 1.3.21

Enclosure tap controls are back, with a new sensor path and a new detector. Built on 1.3.20.

- With the screen off and the PIN lock active, 3 taps on the enclosure pause the current track and 2 taps resume it. The screen stays off and the PIN stays locked. A gesture fires about 0.65 s after the last tap; tap intervals of 235-600 ms are accepted.
- New sensor path: the BMI270 accelerometer writes 400 Hz frames into its own FIFO, paced by the sensor clock, and the worker drains it about every 20 ms. After every restart a 1 s frame-count check against the CPU clock (about +-6 %) must pass before any frame is used. A FIFO backlog, a FIFO error, an I2C error or a failed check restarts the stream and resets the detector. (1.3.18 polled the latest sample every 5 ms, which gave an irregular 145-196 Hz.)
- New detector (detector v2): features are energies over the first 40 ms of an impulse, and 16 independent rules decide. Every recorded event that must stay silent (knocks on the table, objects put down, picking the device up, changing volume, pocket or bag, 4-tap series, finger drumming) is stopped by at least two rules. Knocks on the table are recognised by their ringing; a finger damps a tap on the enclosure within about 40 ms.
- Tap on the side, the edge or clearly on the top of the frame, not on the glass. A light tap on the top is ignored on purpose, because to the accelerometer it looks like a knock on the table. While music plays, at least one tap of the pause series must be clear, so the speaker cannot fake a pause. Taps on a device held in the hand are ignored.
- Known limits: a single clear tap followed within 0.6 s by a cup put down next to the device can count as a double; an extra tap lighter than about 15 % of the others may go unnoticed (four taps could then count as three); the detector was tuned on one device and one user, so another stand, case or table surface can change tap strength and ringing.
- Validation: the evaluation harness in [tools/tap-eval](tools/tap-eval) reports 0 false gestures and 0 wrong commands in each of 23 perturbation families of the recordings; all 41 host test suites pass. On the author's device, 6 pauses and 6 resumes were recognised with the screen staying off, and hard knocks on the table, a 4-tap series, a single tap and a cup put down gave no command. The full recordings are not published; the host tests replay excerpts of them from [tests/fixtures/tap_recordings.h](tests/fixtures/tap_recordings.h).

Details: [docs/TAP-CONTROLS.md](docs/TAP-CONTROLS.md).

## 1.3.20

Bug-fix release on top of 1.3.19. Tap controls stay disabled.

- The PIN lock now also engages while the screen stays lit: during a library scan (after the dim time) and with "Screen off after: Never" (after 5 minutes idle). Previously both cases left a PIN-protected device unlocked indefinitely. No screen capture is taken during a scan.
- Next/Previous pressed while a new track was still loading or seeking, or right after a track ended, no longer leaves the following track paused (`main/transport_policy.h`).
- Seeking while paused now updates and saves the new position immediately; the slider no longer jumps back and repeated keyboard seeks add up.
- Fixed an EQ/effect settings race at track start: the first ~24 ms of a new track could use the previous track's EQ curve (possible click or level dip).
- UPDATE LIBRARY on very large libraries (about 6.5-9k tracks): a refused update now schedules a full scan at the next startup; if that is still too large, the saved library is kept and the device says so.
- Tests: 4 stale host tests repaired; 14 host tests had their `assert()` checks compiled out because Zig predefines `NDEBUG` at `-O1/-O2` and now run with checks active; new tests for transport policy, the keyboard HAL and seeking while paused.

## 1.3.19

Stability release on top of 1.3.18.

- Fixed a random restart (Guru Meditation, store access fault in the I2C interrupt, `i2c_master.c:752`). The A164 keyboard check ran every 2 s through `i2c_master_probe`, which in ESP-IDF 5.5.2 keeps the read state of an earlier failed read and points the interrupt at its own stack array. The keyboard is now detected with an ordinary register read, and keyboard transfers use a 50 ms timeout. The bug was present since keyboard support (1.3.10) and also occurred with no keyboard attached.
- The enclosure tap controls from 1.3.18 are disabled: on the device the 1.3.18 detector recognised 2 of 8 real tap gestures and paused or resumed playback on taps on the table and while the device was handled. The motion sensor is not powered in this build. Building with `-DAURORA_TAP_CONTROLS=1` enables the old behaviour for testing.
- BMI270 transfers use a 200 ms bus timeout (25 ms often expired while waiting for the shared bus and restarted the sensor).
- New calibration-only build option `-DAURORA_TAP_STREAM=1` logs every motion sample.
- Partition layout, settings, favourites, PIN storage, library cache and audio path are unchanged; flashing the application at 0x10000 keeps them.

## 1.3.18

- First version of enclosure tap controls: three light, rhythmic taps pause the current track, two taps resume it from the same position. They work only with the screen fully off and an active, correctly stored PIN lock.
- Tapping does not wake the screen or unlock the panel. Touch and the keyboard wake the screen as before; waking cancels a tap series in progress.
- Recognition waits about 0.4 s after the last tap. Ordinary movement, rebounds, wrong rhythm and series of four or more taps are filtered. The sensor runs in a separate task, without changes to the audio path or the main panel.
- Note: this detector proved unreliable on the device and was disabled in 1.3.19; it was replaced by detector v2 in 1.3.21.

## 1.3.17

- Fixed a restart when rescanning a large library (`bad_alloc` while growing the track array). The metadata lookup now uses compact entries instead of copies of all records, and track storage is reserved without geometric doubling. For a 4313-file test library, extra peak allocations dropped by 74.86 %. If free memory is insufficient (512 KiB working margin), the update is refused and the previous library is kept.
- All data Aurora creates on the SD card now lives under `/Aurora`: `Settings`, `Cache`, `Screenshots`, `Lockscreen` and `startup.jpg`/`startup.png`. An automatic migration keeps settings, presets, favourites and screenshots; conflicting files go to `/Aurora/Legacy`. Music stays in `/Music`, and the PIN stays in the device's private storage. See [docs/SD-STORAGE.md](docs/SD-STORAGE.md).
- The startup logo is the first visible frame: the backlight turns on only after the image has been prepared and flushed. Without a valid file the previous text screen remains.
- Raised the read priority of display memory transfers (DW-GDMA master 1 read priority 15), which removed a reported blue flash at the start of a scan. A full scan of 4313 cached tracks finished in 17.188 s without a restart.
- Files lists folders; Queue respects the selected File filter. If the queue looks empty, choose ALL. Filters never delete music.

## 1.3.16

- New Aurora wordmark replaces the M5Stack/Tab5 branding; demo labels removed and the motto centred. Button and panel geometry stay as in the approved template. The Bluetooth dialog has four equal-width actions, and EQ shows the selected curve name (Custom for an unmatched curve).
- Optional startup logo `/Aurora/startup.jpg` or `startup.png` on the SD card; missing or corrupt artwork falls back to the text screen. The panel and the mandatory PIN lock become available only after the library has loaded. See [docs/STARTUP-1.3.16.md](docs/STARTUP-1.3.16.md).
- Reverb gains Low cut (0-19.9 kHz) and High cut (100 Hz-20 kHz) on the wet signal only, full-rate processing up to 48 kHz, saved settings and migration of the old Tone setting. See [docs/REVERB_SETTINGS.md](docs/REVERB_SETTINGS.md).
- Battery percentage and charging bolt are stabilised (median/low-pass filtering with hysteresis; short load pulses are ignored). The percentage is still a voltage estimate, not a fuel gauge. See [docs/BATTERY_FILTER.md](docs/BATTERY_FILTER.md).
- Playback started from Favorites keeps the favourites queue for Next, Previous, Shuffle and the end of a track; an empty favourites queue never falls back to all tracks.
- Aurora runs standalone (no launcher) from a 15 MiB factory partition at 0x10000. An application-only update keeps the bootloader, partition table and PIN; a standalone image for a clean installation at 0x0 is also provided.

## 1.3.15

- Queue and Favorites keep a ready image of the eight visible, recycled rows, reducing text redraws while scrolling with animations running.
- Panel look, thumbnails, the live playback indicator and keyboard selection are kept. The dragon and the visualiser keep running during touch.
- The row cache uses at most about 403 KiB regardless of the number of tracks; if it cannot be created, the previous drawing path is used.
- No changes to the audio path, reverb, PIN settings or the Bluetooth bridge.

## 1.3.14

- Separate durable PIN storage with write verification and access recovery if the partition entry is lost; the redraw area of a single PIN dot is 75 % smaller.
- The visualiser and the dragon are no longer paused during continuous finger scrolling.
- Settings -> Audio -> Reverb settings: Amount, Decay and Tone, with smooth changes and A/B storage on the SD card.
- Bluetooth bridge: detection of a stalled stream and limited retries; compatible with Pico companion firmware 0.9.2/0.9.3. The AirPods panel requires the experimental Pico 0.9.4 and does not show unconfirmed states (see [docs/AIRPODS_CONTROLS.md](docs/AIRPODS_CONTROLS.md)). AAC is not sent to the Pico.
- The main panel and the base volume level are unchanged.

## 1.3.13

- Settings -> Display -> Screen lock: an optional four-digit PIN, entered and confirmed directly on the device. Change PIN and turning the lock off require the current PIN. The lock is off on a new installation, and there is no default code.
- After the screen turns off, touch wakes it to the PIN keypad; the wake gesture does not press a key. An enabled lock also applies after a restart, while music and headset buttons keep working in the background. Five wrong attempts cause a 30 second cooldown.
- Lock background picks JPG/JPEG/PNG files from `/Aurora/Lockscreen` on the SD card. The default background is a frosted capture of the main panel. Images are decoded once, outside the UI thread, into a native 1280x720 background.
- The PIN is stored in internal NVS as a salted verifier, not in plain text; missing records mean disabled, damaged records fail closed. The PIN and background choice do not depend on the SD card.
- The main panel, EQ, Reverb and audio levels are unchanged from 1.3.12.

Details: [docs/SCREEN_LOCK.md](docs/SCREEN_LOCK.md).

## 1.3.12

- Reverb has a longer decay and more reflected energy: integrated wet T60 rises from 0.539-0.562 s to 1.320-1.399 s in digital measurements. Normal keeps the original 0 dB base level; EQ and saved presets stay compatible.
- A stereo-linked envelope gain protects only the wet path on strong resonant signals; it does not change the direct signal or the EQ/base gain.
- Settings -> Audio keeps output information and Turntable speed; duplicated Mute, Bluetooth and FX/EQ controls were removed (they remain on the main panel).
- Settings -> Library keeps Search, Update library, Sort by and Cover artwork. Folders are chosen through Files and formats through the main File filter buttons. The Audio and Library pages fit without scrolling.
- The main panel layout is unchanged.

## 1.3.11

- The EQ button replaces Retro in the same place and size and opens an 11-band equaliser (31, 63, 125, 250, 500 Hz, 1, 2, 4, 8, 16 and 20 kHz, -12 to +12 dB each); the same screen is in Settings -> Audio -> EQ settings. Space is renamed to Reverb, and the Bluetooth symbol is refreshed.
- The original 0 dB base level is restored in all modes; there is no separate PREAMP slider or Headroom button. Flat resets only the bands.
- Four User 1-4 slots with separate Load and Save, plus eight factory presets (Flat, Bass Boost, Vocal, Rock, Pop, Jazz, Classical, Electronic). EQ state and presets are stored on the SD card in two files with a generation number and CRC.
- The preset list and controls work with the keyboard (Enter, arrows, Escape, Tab/Shift+Tab, left/right for sliders).
- Reverb has a clearer tail: 61 % direct and 38 % diffused signal with high-frequency damping.
- The LIMIT indicator shows the real gain reduction of the shared stereo limiter. It is not a true-peak limiter.
- The charging bolt uses time-based confirmation and ignores single current drops. See [docs/CHARGING-1.3.11.md](docs/CHARGING-1.3.11.md).

Details: [docs/EQUALIZER-1.3.11.md](docs/EQUALIZER-1.3.11.md).

## 1.3.10

- Support for the physical M5Stack Tab5 Keyboard A164 on Ext.Port1: automatic detection at startup and on hot-plug, text entry in search, and navigation of folders, tracks and settings.
- Space pauses/resumes; Ctrl+Left/Right changes track; Ctrl+Up/Down changes volume by 5 points; Left/Right seeks by 5 seconds. Tab switches Queue/Files/Favorites, Ctrl+F opens search. In search, Space types a space, Enter confirms, Escape closes and Ctrl+U clears the text.
- Up/Down and Enter operate lists; focus is independent of the playing track. A key press wakes the screen without also running a command. Touch still works and removes the focus frame.
- The keyboard has its own I2C controller; its handling does not touch the SD card or block the internal codec/touch bus. On disconnect, pending keys and key repeat are cleared.

Details: [docs/KEYBOARD-1.3.10.md](docs/KEYBOARD-1.3.10.md) and [docs/HARDWARE-KEYBOARD-1.3.10.md](docs/HARDWARE-KEYBOARD-1.3.10.md).

## 1.3.9

- Files shows folders inside the right panel. Tapping a folder switches to Queue with its tracks; All music shows the whole library. Browsing does not interrupt the current track, and shuffle stays within the chosen folder and its subfolders. The old large Select folder window is gone.
- The folder list uses eight shared rows with a cache, creates no objects per folder and keeps its scroll position.
- The headphone volume slider was removed from the Bluetooth window; volume stays on the main knob. The link to the Pico is kept during pause, with I2S transfer stopped.
- The dots visualiser reacts more clearly to real audio, with a short pulse on bass hits; volume does not affect visualisation data.
- Fixed a race where quickly changing folders while a track was loading replaced the requested track with the first song of the new folder.
- Three small font styles share identical bitmap bytes, removing 35,824 B of duplicated data with pixel-identical output.

## 1.3.8

- The charging bolt is based on the measured current flowing into the battery; CHG_STAT no longer overrides a discharge reading, which used to leave the icon on after unplugging.
- On: at least +20 mA for 250 ms. Off: the first reading of +5 mA or less, a sensor error or no battery. USB presence and a full battery alone do not mean charging.

## 1.3.7

- Fixed cover art from an MP3 whose valid JPEG used component sampling `0x12`, rejected by the previous decoder. A private fallback decoder handles this case and progressive JPEG.
- JPEG/PNG art from PIC/APIC in ID3v2.2/2.3/2.4 (also in WAV and before AAC ADTS) and native FLAC PICTURE. Front cover is preferred, then Other, icons last. MP4/M4A containers are not supported.
- Real 1/2, 1/4 and 1/8 JPEG scaling, bounded memory for the fallback decoder and cancellable work. Eight visible 56x56 thumbnails load once scrolling settles.
- All, MP3, WAV, FLAC and AAC filters fit in two rows.
- The control atlas is packed losslessly (APZ1): 270,946 instead of 622,343 bytes, decoded once at startup by the ROM decoder.

Details: [docs/ARTWORK-1.3.7.md](docs/ARTWORK-1.3.7.md).

## 1.3.6

- Bass+ uses a linear bass filter with shared stereo peak control instead of distorting soft clipping. Retro no longer reduces resolution to 10 bits. Presets cross-fade over 10 ms; settled Normal is bit-transparent.
- Smoother scrolling: dialogs belong to the active screen so LVGL skips the covered panel, only changed rows are synchronised, and PPA transfers run in a separate task.
- Power off releases a retained RTC interrupt that blocked the shared E_TRG line, and follows the official sequence of up to three 100 ms HIGH / 100 ms LOW cycles.
- HAL startup retries up to three times (50/100 ms delays, at most one bus reset) before creating the display and LVGL.
- FFT no longer goes silent on out-of-phase stereo: it picks the dominant channel with hysteresis.
- Lossless asset packing saves another 36,559 bytes with identical pixels.

## 1.3.5

- Phone-like screen off: backlight fully off (0 %), LVGL refresh, animations and FFT stopped while audio keeps playing. Touch stays powered; the first touch restores the screen and is consumed so it does not press a button underneath. This is not CPU deep sleep.
- Settings -> Display -> Screen off after: Never / 30 sec / 2 min / 5 min (default 30 s for a new configuration; a saved choice is kept).
- Select folder rows are cached as rendered RGB565 images while the window is open (about 0.9 MB, released on close).
- Image presentation no longer copies buffer areas that are about to be replaced (371,680 fewer copied pixels per frame for an 808x460 window, measured in a buffer model).
- Dots visualiser: three interleaved bands of coloured dots driven by real band energy, with bass pulse and sub-pixel drawing.
- With a dialog open, FFT, new artwork jobs and covered-panel updates are paused; music keeps playing.

## 1.3.4

- Touch is polled every 16 ms regardless of lost interrupts; the track list swaps only the incoming row; a larger partial draw buffer reduces PPA rotations.
- Flight: a separate wing surface folds along a line from the nape to the tail root; body, head and tail stay still. 60 phases / 1.65 s are kept.
- Power off holds the final HIGH for 2 s, matching M5Unified, and reports an error if power is not cut (no fake shutdown through deep sleep).
- EQ dots keep a fixed layout with smoothed, sub-pixel motion instead of bass-driven jumps.
- Search keyboard anchoring fixed; all rows fit in the window and the close key closes search.
- Screensaver behaviour from this version was superseded by screen off in 1.3.5.

## 1.3.3

- Bass+ boosts lows by about 6 dB without the previous fixed ~7 dB cut of the whole signal; soft limiting acts only on peaks near clipping.
- Settings -> Audio -> Turntable speed: 75-125 % in 1 % steps with Reset to 100 %. Tempo and pitch change together, before the local/Bluetooth split; the setting is saved, and settled 100 % is bit-exact.
- Cover art: the four most recently decoded covers are cached in RAM; Update library clears the cache.
- Screen off uses backlight 0 and Display Off without DCS Sleep In, which could also cut touch on the Tab5's integrated controller; a 50 ms input poll guards against lost interrupts.
- Headphone detection follows M5Unified (HP_DET without internal pull, 30 ms polling, amplifier cut on first detection).
- Three native display buffers let the next frame be prepared while the previous one is shown, still protected by two VSYNCs; LVGL refresh 16 ms, animation/visualisation 33 ms (settings, not a measured FPS).
- Bluetooth header shows only an enlarged symbol: green for available and enabled, grey for disabled or unavailable.
- Flight wing deformation corrected with a wider hinge blend and inverse mapping checked for fold-free sampling.

## Earlier versions (1.0-1.3.2)

Aurora Player started as a native ESP-IDF/LVGL port of the approved Aurora HTML template v1.5, running directly on the Tab5 without a browser or a computer connection (see [docs/AURORA-PORT.md](docs/AURORA-PORT.md)). The 1.0.x releases fixed startup and storage basics: 1.0.1 removed a forced 400 MHz CPU clock that halted ESP32-P4 chips before rev. 3, 1.0.2 reads SD capacity from volume metadata instead of scanning the exFAT bitmap, 1.0.3 builds the catalogue from directory enumeration without opening every track, and 1.0.4 streams audio through the file descriptor directly. 1.1.0 added a saved catalogue index tied to the card, a reworked display presentation path and the optional Bluetooth bridge to an external ESP32-PICO-D4 (PCM over I2S0, control over UART2) running separate companion firmware. Up to 1.3.1/1.3.2 the port gained full 1280x720 layout, CSS-accurate pressed states and knob behaviour, Flight and Living dragon animations, the gear settings categories (Dragon, Display, Audio, Library, Clock, Battery / Power), headphone-jack speaker muting, folder shuffle, repeat off/all/one, a fast saved library index, lossless asset packing and hidden screenshots to the SD card. Platform details: [docs/platform.md](docs/platform.md).
