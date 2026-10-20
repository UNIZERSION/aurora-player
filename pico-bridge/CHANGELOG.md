# Pico Bluetooth bridge: changelog

Newest first. Dates are from the release packages. The wiring has been the same since 0.3.0
(seven wires, see [README.md](README.md#wiring)), and every update keeps the saved headphones
unless it says otherwise.

## 0.9.5 (2026-10-16)

The version installed and used with Aurora; published here as source and release images.

- Fixes the 0.9.4 regression: `STATUS` and `STATE` again report `audio=2` for a started stream,
  as up to 0.9.3, and now `audio=0` for every other state (up to 0.9.3 the raw ESP-IDF 4.4 value
  was sent, so a stopped stream showed `audio=1`), whatever value ESP-IDF 5.5 uses internally.
  With 0.9.4 Aurora showed *Audio output / I2S failed* about 2 s after Play.
- Pins, saved devices, audio processing and AirPods controls are unchanged from 0.9.4.
- Verified on the Pico as an update from 0.9.4: it boots, all five saved devices were kept, and
  music, pause and resume worked with Aurora 1.3.15 (rising audio counters, no output errors).
  Not yet verified: AirPods listening-mode switching, long sessions, range.
- Repository version (2026-10-20): the build maps the source paths so the images no longer embed
  local file paths; the release images are the tested build with those two strings replaced.

## 0.9.4 Experimental (2026-10-16)

Do not use with Aurora: its audio-state report makes Aurora stop playback after 2 s.

- Rebuilt on native ESP-IDF 5.5.2 (earlier versions used Arduino ESP32 2.0.17 / ESP-IDF 4.4),
  with all 0.9.3 fixes, the same pins and UART protocol, and an adapter that keeps the saved
  devices and volume.
- AirPods control channel (L2CAP PSM 0x1001, selected device only): battery for the left and
  right earbud and the case, ear detection, and Off / Noise cancellation / Transparency /
  Adaptive requests. A mode is reported only after the headphones confirm it, and a failed
  control channel never drops the music.
- New commands `AIRPODS CONNECT | DISCONNECT | STATUS | MODE <1..4> | AUTO_PAUSE <0|1>`; auto
  pause is off by default and saved on the Pico. `READY` and `STATUS` report `cap_airpods=1`.
- `STATUS` is followed by `AIRPODS` and `CODEC` lines (negotiated SBC rate, channels and bitpool
  range).

## 0.9.3 (2026-10-16)

Built and host-tested; never installed on the Pico (0.9.4/0.9.5 contain everything in it).

- Stream reliability: progress counters, idempotent tagged `STREAM` retries, correct handling of
  the SDK's flush request, and a SUSPEND, CHECK, START recovery that does not disconnect the
  headphones.
- Recovery runs only when audio keeps arriving from the Tab5 but Bluetooth stops taking it,
  never on silence or volume 0: at most 3 attempts per 60 s, at least 15 s apart, then
  `STREAM_FAULT <token> TRANSPORT_FAILED`. `STREAM_FAULT <token> INPUT_STALLED` tells Aurora its
  I2S output stopped.
- New `STATUS` fields: `rx_blocks`, `callbacks`, `consumed`, `recoveries`, `input_stalls`,
  `flushes`, `rx_age`, `cb_age`, `stream_token`, `codec=SBC`, `cap_airpods=0`.

## 0.9.2 (2026-10-12)

- Pause is a real A2DP SUSPEND and `STREAM` resumes it, so the headphones and their buttons stay
  connected during a pause (before, pause only sent silence).
- Media commands run one at a time on the Bluetooth task and survive late, busy or failed
  acknowledgements; a quick Pause/Play never sends START and SUSPEND together.
- `TEST` briefly resumes a paused stream and restores the previous state.
- Button diagnostics in `STATUS` (`remote_key`, `remote_presses`, …).

## 0.9.1 (2026-10-10)

- Fixes a race on a fast STOP followed by STREAM that could leak about 256 old frames into the
  new stream.
- First Standalone image for a blank Pico.

## 0.9.0 (2026-10-09)

First version for the new Tab5 player (Aurora 1.1.0, where the Bluetooth bridge first appears).

- Tagged handshake: `STREAM <n>` is answered with `STREAM_ACK <n>`, so Aurora can ignore a late
  acknowledgement.
- 768-frame start buffer that refills after an underrun; `underruns` counter.
- Robust UART parser (80-character lines; oversized or invalid commands are rejected).
- Faster volume at 0 and 100, no flash write for an unchanged volume, silence instead of a
  repeated buffer during pause.

## 0.8.0 (2026-10-08)

- Headphone buttons (PLAY, PAUSE, STOP, NEXT, PREVIOUS over AVRCP) are forwarded to the Tab5 as
  `REMOTE` lines, through a 16-entry queue that drops repeats and stale presses.
- Digital volume 0-100 from the Tab5 with a 256-frame ramp; volume changes made on the
  headphones no longer override it.

## 0.7.0 (2026-10-08)

- Power saving: Bluetooth and I2S start only when the Tab5 sends `USE`; with no `USE` for 15 s
  the Pico goes to deep sleep, and `SLEEP` sends it there at once.
- The Tab5 wakes the Pico with a pulse on the existing UART wire (G32); no extra wire.
- Scanning stops after about 20 s and the results stay selectable; new `SELECTMAC`.

## 0.6.0 (2026-10-08)

- Remembers up to 8 headphones and speakers; saved ones are listed even when they are off.
- Tapping another saved device switches the output.

## 0.5.0 (2026-10-08)

- More reliable reconnect to the saved device; `SCAN` no longer deletes it; new `RECONNECT`.

## 0.4.0 (2026-10-08)

- Connects straight to the saved headphones after a restart, without scanning or pairing mode.

## 0.3.0 (2026-10-08)

- Two UART wires (Tab5 G6 → Pico G32, Pico G33 → Tab5 G46) let the Tab5 scan, select and set
  the volume: seven wires in total. Volume 0-100 saved on the Pico.

## 0.2.0 (2026-10-08)

- First real bridge: the Pico receives I2S audio from the Tab5 (G26/G25/G22, 44.1 kHz, 16-bit
  stereo) and sends it to Bluetooth headphones.

## 0.1.x (2026-10-08)

- First Bluetooth A2DP test on the M5Stamp Pico: pairing and a left / silence / right test tone,
  with serial commands `LIST`, `SELECT`, `TEST`, `STOP`, `SCAN`.
