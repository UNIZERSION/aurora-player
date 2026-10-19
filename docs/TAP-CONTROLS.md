# Enclosure tap controls (Aurora 1.3.21)

## How to use

1. In Settings → Display → Screen lock, set a PIN and wait for the screen to turn off. The Tab5 must lie or stand still.
2. Tap the enclosure with a finger, not the screen: on a long side, or clearly from above on the frame. Tap the same spot at a calm, even pace. The gap between taps must be 0.24–0.6 s; a natural pace is about 0.3–0.45 s. The Tab5 must not be held in the hand.
   - **3 taps** pause the playing track.
   - **2 taps** resume the track from the same position.
3. The command runs about 0.65 s after the last tap. The screen stays off and the PIN lock stays on.

- A tap on a long side can be light. Light taps on the short edges are rejected, just like presses of the side buttons. A tap from above (towards the table) must be clear, because to the sensor a light tap from above looks the same as a knock on the table next to the device. When in doubt, tap the side.
- While music plays, at least one of the 3 pause taps must be clear.
- A single tap does nothing. A series of 4 or more taps is cancelled.
- Touching the screen wakes it as before and interrupts a series in progress, so do not tap the glass.
- The gesture is ignored while the screen is on, when no PIN is set, when saving the PIN failed, and during shutdown. It never starts a stopped track, never changes the track or the volume, and never unlocks the device.

## What is rejected (measured on the device)

Among other things, the detector ignores:
- knocking on the table next to the Tab5 (from 10–15 cm away, and hard from 5 cm away),
- putting an object down on the table,
- picking the device up, changing the volume, and adjusting it on its stand,
- series of 4 taps,
- carrying it in a pocket or bag (more than 2 minutes of recording),
- fast finger drumming (every 100–215 ms, 3–8 hits),
- single hard hits.

No motion sensor can prove that a movement was intentional. The rules are set so that, when in doubt, a gesture is skipped rather than executed.

## How it works

- **Sampling.** The BMI270 writes samples into its own FIFO buffer at 400 Hz, paced by the sensor clock. The `sleep_taps` task drains it about every 20 ms. Each sample reaches the detector with a timestamp computed from its index (whole ms, 2 and 3 ms steps, 2.5 ms on average), regardless of task latency.
- **Stream check.** In every 1 s window the number of samples is compared with the CPU clock (3 % plus 12 samples, about ±6 % at 400 Hz). On the author's device the sensor clock runs 0.6 % slow. After every restart the detector receives no data until the first window has passed; later windows are checked after the fact. A FIFO backlog above 0.64 s (a sample may have been lost), a FIFO error, an I2C error or a wrong sample count restarts the stream and resets the detector.
- **Impulse features.** The detector measures the motion energy in the first 40 ms after the onset of an impulse rather than a single peak, so the result depends only slightly on the sampling. From this energy it computes the strength (RMS), the fraction along gravity, the distribution over the device axes, and the share of "smooth" displacement (a push rather than a tap). The rebound and ringing after a tap (up to 230 ms) belong to the same tap.
- **Music.** While music plays, the speaker can shake the enclosure slightly, especially when the Tab5 stands upright. So during playback a series needs at least one clear tap. Light side taps are enough only while paused, that is, to resume.
- **Independent rules** (each one alone can stop a false gesture):

| Rule | What it checks |
|---|---|
| Smooth | the impulse is not a push or a displacement |
| Vibration | a weak impulse along gravity (table vibration) is not a tap |
| Axial | an impulse along the long axis of the enclosure (side buttons) is not a tap |
| LeadQuiet | the device was still before the first tap |
| Isolation | there was no motion at all for 0.65 s before the first tap |
| Interval | gaps of 235–600 ms |
| Split | a sharp impulse inside a tap window means drumming (cancels the series); exception: the rebound of a device rocking on its stand |
| Trailing | any impulse not counted as a tap during a series cancels it |
| Tilt | the orientation does not change during the series (≤ 8°) |
| Sustain | sustained motion during the series or at its end cancels it |
| StrongTap | a series with taps from above needs one clear tap |
| Consistency | all taps excite the same axes (the same spot) |
| Census | the number of impulses from 0.65 s before the series to its end equals the number of taps; strong motion outside the tap windows cancels the series |
| Settle | a later tap does not start from motion stronger than the earlier taps (device lifted or dropped) |
| Sharp | the impact is brief (≥ 10 Hz); a button press or a push lasts longer |
| Ringing | if every tap of the series still rings 50–150 ms after the impact, it was a knock on the table |

The thresholds and windows are the defaults of `TapGestureConfig` in [`main/tap_gesture.h`](../main/tap_gesture.h); the detector is in [`main/tap_gesture.cpp`](../main/tap_gesture.cpp) and the FIFO reading task in [`main/sleep_tap_input.cpp`](../main/sleep_tap_input.cpp).

## Validation

Data: 3 recording sessions from 2026-10-17, one device and one person.
- Two night sessions with the 1.3.18 calibration build (sampling about every 5 ms).
- One evening session with the 400 Hz FIFO calibration build. It covered: the Tab5 on its stand, triple series with music playing and double series while paused, knocks on the table from 10–15 cm and hard from 5 cm, putting objects down, picking the device up and changing the volume, more than 2 minutes in a pocket or bag without a cable, light series, and 2–3–2–3 control series.

Labels: `tools/tap-eval/labels.csv` — 27 gestures, 29 segments that must produce no reaction, and 5 ambiguous ones (not scored).

The evaluation tools are in [`tools/tap-eval`](../tools/tap-eval/). The recordings themselves are not published.

Results of the `tools/tap-eval` harness (1.3.21):
- **0 false gestures and 0 wrong commands** in each of 23 variants: the original recordings, 200 Hz by averaging sample pairs (both phases), ±2 ms timing jitter, resampling to 200/150 Hz (3 methods, 8 phases), loss of every 4th or 5th sample, random loss of 1–5 %, strength ×0.8–1.5, gain error ×0.8/1.2, 11 device orientations, and all sessions joined into one stream. Auxiliary variants (100/125 Hz, 20 % loss, strength ×0.6 and ×2) also give 0 false gestures.
- **Every negative event is stopped by at least 2 independent rules.** With any single one of the 16 rules disabled, there is still no false gesture in any perturbation variant.
- **Knocks on the table and objects put down** give no gesture even when amplified up to 3×. Above 1.5× they are stopped by the Ringing rule.
- Further attack attempts produced no command either: drumming with every recorded kind of tap, 4 taps with a light first or last tap (from 5 % strength), clinks during table knocks, and real taps inserted into the pocket and handling recordings.
- **Detection:**
  - recordings with 400 Hz FIFO reading (as the firmware works): all 10 normal series and a lighter series with music playing; of 5 light series, 2 were detected (light side taps);
  - older recordings (different sampling, different placement): 8 of 11 series.
  - The missed series are taps on a device held in the hand, two weak double series from above, and light taps from above and on an edge. A miss is safe: nothing happens.
- With perturbed data some series are missed (for example about 50 % at 150 Hz), but without false gestures. The firmware always delivers a steady 400 Hz.

Host tests:
- [`tests/tap_gesture_test.cpp`](../tests/tap_gesture_test.cpp) uses segments of real recordings ([`tests/fixtures/tap_recordings.h`](../tests/fixtures/tap_recordings.h)) at 400 Hz, 200 Hz and strength ×0.8–1.5. It adds series of 1–7 taps built from a recorded tap, drumming, 4 taps with a light first or last tap, timer wrap-around and stream errors. For each of the 16 rules there is a scenario that produces a command with all rules off and that this rule alone stops.
- [`tests/test_tab5_motion_hal.c`](../tests/test_tab5_motion_hal.c): FIFO configuration, reading complete frames, overflow, I2C errors at every point.
- [`tests/tap_controller_test.py`](../tests/tap_controller_test.py): the PIN, screen and playback-state gates, run against the real controller code.

Test on the device (2026-10-17, version 1.3.21, screen off, PIN locked, music playing, Tab5 on its stand):
- 6 pauses and 6 resumes recognised and executed; the screen stayed off the whole time;
- 3 hard knocks on the table 5 cm away, a series of 4 taps, a single tap and a mug put down: no command;
- taps on the Tab5 held in the hand: ignored (as intended);
- one transient I2C error when the screen turned off; reading resumed on its own.

## Limitations

- Light taps from above, towards the table, are deliberately skipped, because the sensor cannot tell them apart from a knock on the table.
- A Tab5 held in the hand is not still, so taps are ignored then. Supporting handheld use would need separate recordings and would raise the risk of false pauses in a pocket.
- A single clear tap followed within 0.6 s by a mug put down next to the device can count as 2 taps.
- An extra tap weaker than about 15 % of the others may go unnoticed. In that case 4 taps can be counted as 3.
- The data come from one device and one person. A different stand or case can change the tap strength.

Data and tools: `tools/tap-eval` (harness, labels), `tests/tap_gesture_test.cpp` and `tests/fixtures/tap_recordings.h`. Sensor: [Bosch BMI270 datasheet](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi270-ds000.pdf).
