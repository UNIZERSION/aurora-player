# Aurora Player 1.3.18 audit (17 Oct 2026)

> **Status (1.3.21):** this audit describes 1.3.18. The findings have since been addressed as follows:
>
> - **1.3.19:** I2C driver crash (keyboard presence check). Tap controls were disabled in this release.
> - **1.3.20:** PIN lock while the screen is lit (during a library scan and with "Screen off after: Never"), Next/Previous loading the track paused, seek while paused, the EQ settings race at track start, UPDATE LIBRARY on a large library, the 4 stale tests, and 14 tests whose `assert` checks were compiled out (Zig `-O1/-O2` defines `NDEBUG`).
> - **1.3.21:** new tap detector (BMI270 FIFO at 400 Hz, 16 independent rejection rules); tap controls are enabled again. See [TAP-CONTROLS.md](TAP-CONTROLS.md).
>
> The remaining low-severity findings are unchanged unless stated otherwise.

Source audited: Aurora Player 1.3.18. The full per-finding data (scenarios, verifier reasoning, proposed patches) and the test logs were kept outside this repository; the results are summarized below.

## Methods

| Method | Scope |
|---|---|
| ESP-IDF 5.5.2 build | 15 warnings, all in the generated font `main/aurora_fx_label12.h` (missing struct fields = zero). Harmless. |
| Tab5 over USB | Boot of 1.3.17 and 1.3.18, serial-port logs. |
| Host tests (Zig 0.13.0) | 38 suites from `tests/`. |
| Code review | 6 areas (taps, library, audio, UI/LVGL, PIN, inputs/peripherals). Every finding was checked by a separate verifier that tried to disprove it. 27 agents in total. |

## Tab5 — on-device results

| | 1.3.17 | 1.3.18 |
|---|---|---|
| Boot to ready panel | 10.6 s | 10.6 s |
| Index of 4313 tracks | 3144 ms | 3154 ms |
| Free memory after boot | 9,147,660 B | 9,138,384 B (−9 KB: tap task) |
| Crashes / restarts | none | none |
| Errors in the log | `gpio isr service already installed` (harmless, keyboard driver) | same |

Tap controls (new in 1.3.18) only start with the screen off and a PIN set, so they need a manual test.

## Crash found on the Tab5: hang in the I2C driver (also affects 1.3.17)

On 17 Oct 2026 the Tab5 restarted (`Guru Meditation Error: Core 0 panic'ed (Store access fault)`, `i2c_master.c:752`; the I2C interrupt wrote data to address 0x000c7f24). No keyboard was connected.

- Cause (analysis plus independent verification, medium confidence): the A164 keyboard handling (`components/tab5_hal/tab5_keyboard.c`) checks every 2 s whether the keyboard is present using `i2c_master_probe` from ESP-IDF 5.5.2. That function does not clear its state after an earlier failed read and points the interrupt at an operation array on its own stack; the interrupt then writes through a stale register (value = a timestamp). The registers in the crash dump (A3 = A5 = 0x000c7f25) match this path. Without a keyboard this needs a spurious response on the empty connector — rare, but it happened.
- Keyboard support exists since 1.3.10, so **1.3.10–1.3.18** are affected, including 1.3.17.
- The motion sensor errors `IMU read failed: ESP_ERR_TIMEOUT` at the same time are waits on the busy bus — harmless; they only appear under the same CPU load.
- The bug in ESP-IDF itself appears to be unfixed in Espressif's latest code as well — worth reporting upstream.
- **Fix (1.3.19):** keyboard detection uses a plain register read instead of `i2c_master_probe`, with a 50 ms timeout instead of 10 ms; the motion sensor timeout is 200 ms instead of 25 ms. The keyboard HAL test (previously not wired to any script) was updated and added as `tests/run_keyboard_hal_tests.ps1`.

## Tap test on the Tab5 — DOES NOT WORK (needs calibration)

Manual test on 17 Oct 2026 (Tab5 on a table, PIN enabled, screen off, serial log recorded): neither 3 nor 2 taps did anything.

- The sensor works correctly: armed after the screen turns off, ~148 samples/s, taps visible as 0.75–2.9 g.
- Every tap was rejected as a **pulse that is too long** (`REJECT width`, 91–101 ms against the limit `max_pulse_ms = 90` in the 1.3.18 `main/tap_gesture.h:63`). The Tab5 enclosure rings for 144–221 ms after a tap (until it falls below 0.12 g). In addition, the intervals between taps, 375–384 ms, exceeded the 360 ms limit.
- The comment in the 1.3.18 `tap_gesture.h` itself says the thresholds are "conservative, to be calibrated on the assembled device" — that calibration had not been done (`hardware_tested: false`).
- While arming, one IMU read timed out (`IMU read failed: ESP_ERR_TIMEOUT`) during the lock-screen wallpaper write — shared I2C bus; the sensor recovered on its own.

Host replay: 3 recordings from the device (`TAP_TRACE`) run through the real 1.3.18 `tap_gesture.cpp` give exactly the same rejections as on the Tab5. With the thresholds `release_g = 0.20`, `max_pulse_ms = 250`, `max_tap_interval_ms = 450` the same recordings correctly give **TRIPLE (pause)** and **DOUBLE (resume)**.

Note: these were only 3 recordings and no negative trials (picking up, carrying, loud music). Looser thresholds without such recordings risk accidental pauses — a calibration session with positive and negative trials was needed.

### Calibration on 17 Oct 2026 — 17 guided trials

A calibration build (`-DAURORA_TAP_STREAM=1`) records every sensor sample. 23 labelled segments: 8 gestures (triples: natural ~0.39 s, fast ~0.26 s, on the side, in the hand; doubles: ~0.34 s and slow ~0.54 s) and 13 negative trials (1 and 4 taps, picking up, putting down, putting down hard, walking, sliding, tapping the table, loud music, volume change).

| Detector | Gestures detected | False alarms |
|---|---|---|
| 1.3.18 as shipped | 2 / 8 | 2 (tapping the table → pause, holding in the hand → resume) |
| Best thresholds for the 1.3.18 algorithm | 5 / 8 | 0 |
| Design A (bounce fix, smallest change) | 8 / 8 | 0 |
| Design B (impulse features + direction relative to gravity) — chosen by the judge | 8 / 8 | 0 |
| Design C (envelope + many rules) | 8 / 8 | 0 |

The verifier (perturbation tests on the real recordings) judged design B **not yet releasable**:

- with different sample timing (100–150 Hz, dropped samples), holding the device during a volume change gives a false resume, and a series of 4 taps can give a pause;
- the margins are narrow — the 8/8 result is fitted to these particular samples;
- almost all recordings were made with the Tab5 lying flat; the stand position and carrying in a pocket or bag were not covered.

Conclusions: 1.3.18 as shipped is worse than every design (false pauses). A release needed: fixed-rate sensor readout (BMI270 FIFO, ≥200 Hz), features independent of the sampling phase, a second independent check for every type of false alarm, tuning with robustness to perturbations as the criterion, recordings on the stand and in a pocket or bag, and new tests.

**Outcome:** tap controls were disabled in 1.3.19. The 1.3.21 detector implements these requirements (FIFO at 400 Hz, energy-based impulse features, 16 independent rules, each negative event stopped by at least 2 of them); see [TAP-CONTROLS.md](TAP-CONTROLS.md) and the evaluation harness in [tools/tap-eval](../tools/tap-eval/README.md).

### 1.3.20 test on the Tab5 (17 Oct 2026)

Confirmed on the author's device: two quick Next presses → the following track plays; seeking while paused keeps the new position and Play starts from it; with "Screen off after: Never" the PIN locked after 5 min with the screen lit (log: `LOCK flow=7 locked=1`, then unlocked). No restarts in about 13 min. One observation: once `pi4ioe5v6408: read_input_reg failed` (I/O expander on the BSP bus) while the lock wallpaper was loading with the screen lit — the next read succeeded, no effect; not seen before, to be watched.

## Host tests: 33 / 38 PASS

| Result | Tests |
|---|---|
| PASS (33) | accessories, assets, audio_eq, bluetooth, core, display, eq_store, folder_render, keyboard_ui, library_scan_memory, lock_ui, motion_hal, particles_response, power, queue_cache, reverb_settings, screen_lock, storage_layout, **tap_gesture**, visual_scroll, artwork_stop, font, artwork_decode, lock_wallpaper, py: audio_eq_boot_probe, charging_status, folder_navigation, keyboard_controller, shutdown_controller, **tap_controller**, eq_dialog_lvgl, fx_skin_lvgl, eq_factory_response |
| Stale tests (4) | exfat (does not link `audio_eq.c`), py_artwork_controller, py_eq_controller, py_search_keyboard_lvgl — they cut fragments out of `main.cpp`, which has changed since (e.g. no `sleep_tap_arm`, `lock_job`). A test defect, not a firmware defect. Fixed in 1.3.20. |
| Flaky (1) | audio (`audio_eq_engine_test.c:98`) — passes 1 run in 4. Analysis below. |

## Confirmed bugs — medium severity

All four were fixed in 1.3.20.

1. **UPDATE LIBRARY stops working with a large library** — `main/music_library.cpp:277`, `main/main.cpp:734`.
   The scan builds a second full library next to the one on screen. At ~9k tracks (and after adding new music already at ~6.5–7k) memory runs out, the scan is refused with "restart and retry", but after the restart the same old index is loaded — the library cannot be refreshed from the device.
   Proposed fix: on refusal, invalidate `catalog.bin.index` (the next boot does a full scan without a second copy), or free the current list before the scan.

2. **Next/Previous pressed while a track is loading or seeking loads the track paused** — `main/main.cpp:840`.
   Two quick Next presses (touch, keyboard autorepeat, BT headphones) → track N+2 sits paused.
   Proposed fix: derive "stay paused" from the engine's pause intent, not from "state ≠ PLAYING".

3. **Seeking while paused does not change the position** — `main/audio_engine.c:251`.
   The slider jumps back to the old position, several keyboard seeks do not add up, and on power-off the old resume position is saved.
   Proposed fix: in `seek_to`, set `snapshot.position_ms` to the target.

4. **The screen does not PIN-lock during a library scan** — `main/main.cpp:359`.
   For the whole scan (and with a hung SD card, until restart) a device with a PIN enabled stays unlocked.
   Proposed fix: PIN-lock during the scan as well (the scan itself does not depend on the screen).

## Confirmed bugs — low severity (selection)

- **"Screen off after: Never" disables the PIN lock** for the whole session, although the settings show it as enabled — `main/aurora_settings.inc:34`. Fixed in 1.3.20.
- **The row menu after a library update** points at a different track (e.g. adds the wrong one to favorites) — `main/player_ui.cpp:203`.
- **Progressive JPEG artwork** (e.g. 1300×1300) uses almost all free PSRAM, is not displayed, and the attempt repeats every 30 s — `main/player_artwork_decode.cpp:29`.
- **Seeking in FLAC/AAC decodes from the start of the file** — silence for a few seconds on long tracks, and near the end of a long mix up to a minute — `main/audio_engine.c:256`.
- **AVRCP Play can pause** (during a silent FLAC/AAC seek) — `main/main.cpp:1092`.
- **A flat battery** powers the Tab5 off without saving: resume up to ~30 s earlier, recent favorites changes may be lost — `main/main.cpp:1750`.
- **Taps (1.3.18 detector):** the debug output `AURORA_TAP_TRACE` was enabled in the release build (`main/sleep_tap_input.cpp:11`); a transient screen-lock timeout cleared a tap series in progress (`main/main.cpp:385`); a gesture was lost when the main loop stalled for >250 ms (`main/sleep_tap_input.cpp:123`). At worst each of these meant tapping again. The 1.3.18 detector was replaced in 1.3.21.
- **A directory read with an SD error** is treated as the end of the directory — some albums disappear from the library until a manual UPDATE LIBRARY — `main/music_library.cpp:289`.
- **Power loss during the one-time settings migration** (update from ≤1.3.16) resets the settings — a window of a few ms — `main/storage_layout.cpp:279`.
- **A corrupted shared NVS** (e.g. by M5Launcher) can lock the PIN permanently until NVS is erased from a PC — `main/screen_lock_storage.cpp:180`.
- The Pico BT bridge may be in an unknown state after boot (likely) — `main/bluetooth_bridge.cpp:383`.

Line numbers refer to the 1.3.18 sources.

## EQ test (flaky) — a real race in the engine, unlikely on the Tab5

`main/audio_engine.c`, `output_task`: the EQ, effect and reverb settings are read at the top of the loop (lines 529–536), then the task waits up to 10 ms for a block (line 573). If that block starts a new track, the filters are reset with the **old** settings (line 607). The first block of the new track (~24 ms) goes through the previous track's EQ curve/effect; the next block transitions smoothly to the correct one.

- On the PC the test fails from 1/28 (no load) to 3/4 (loaded CPU) — the threads really run in parallel there.
- On the Tab5 the decoder and output run on the same core and the output preempts the decoder, so in practice the settings are read again. A simulation with Tab5-like scheduling: 0/10 and 0/42 failures. The race is theoretically possible (e.g. a slow `output.volume()` through the Bluetooth bridge at the moment of an EQ change and a track start).
- Effect if it happened: a short "click" at the start of the track (limiter at 32700) and ~1 dB quieter for about half a second.
- Seek is not affected (0 failures in ~200 trials). The test had last been run in the 1.3.11/1.3.12 audits, before the engine change in 1.3.14.

Fix (verified on the PC: 30/30 PASS without load, 0/20 failures under load, other audio tests unchanged): re-read the settings under the lock at lines 580–585, for the block actually being processed. Applied in 1.3.20.

## Proposed fix order (as written for 1.3.18)

1. PIN lock during the scan + "Never" (security). — done in 1.3.20
2. Next/Previous and seeking while paused (daily use). — done in 1.3.20
3. UPDATE LIBRARY with a large library. — done in 1.3.20
4. Tap calibration on Tab5 recordings (positive and negative trials), then `AURORA_TAP_TRACE` off in release + robustness to the screen-lock timeout. — replaced by the new detector in 1.3.21
5. EQ settings race at track start (`audio_engine.c:580`). — done in 1.3.20
6. JPEG artwork: memory limit and no retry every 30 s.
7. Repair of the 4 stale tests. — done in 1.3.20
