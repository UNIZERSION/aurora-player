# Aurora 1.3.11 — 11-band equalizer

The former Retro button opens the equalizer. Its original position, size, border and pressed/selected states are retained. The former Space effect is labeled Reverb, and the Bluetooth status has an updated icon; the main-panel layout and approved artwork remain intact. Since 1.3.12, use the main-panel EQ button: the duplicate Settings → Audio → EQ settings shortcut has been removed.

## Controls

- Eleven bands: 31, 63, 125, 250, 500 Hz and 1, 2, 4, 8, 16, 20 kHz. Each adjusts from −12 to +12 dB in 1 dB steps.
- Listening volume: use the main volume knob. There is no PREAMP slider or Headroom button. The original 0 dB base level is restored in every mode, including Normal and EQ OFF; the earlier 12 dB attenuation is removed. Moving a band never changes this base gain.
- LIMIT: the live amount of gain reduction from the emergency peak limiter. If it stays active, reduce the band boosts. The main output-volume knob is downstream and cannot prevent overload inside the equalizer. It reports measured limiting, not a predicted attenuation calculated from slider positions.
- EQ ON/OFF: bypass or enable only the eleven bands, retaining the fixed base level. Normal, Bass+ and Reverb remain separate mutually exclusive effect choices.
- Flat: reset only the working band gains to zero. Keep the enabled state and saved presets intact; the common base level stays fixed.
- Presets: choose Flat, Bass Boost, Vocal, Rock, Pop, Jazz, Classical or Electronic. These factory starting curves apply immediately and enable EQ at the common fixed base level. Factory choices do not overwrite User 1–4 or change the selected User slot; use Save explicitly to store one there. The displayed factory name matches the band gains only; an edited curve with no exact match displays Presets.
- User 1–4: select a storage slot. Load recalls its curve and enabled state; a legacy saved PREAMP does not alter playback level. Save stores the current settings in that slot. Selecting a slot alone does not change the sound.
- Close or Escape returns to the existing player. Tab/Up/Down focuses controls; Left/Right adjusts a focused slider. With Presets focused, Enter opens its list, arrows preview a choice and Enter applies it. Escape first cancels an open list; a second Escape closes the equalizer. Tab or Shift+Tab cancels an unconfirmed choice and moves focus.

The Flat factory choice enables EQ like the other factory curves. The separate Flat button only zeroes band gains and preserves the existing enabled state. Factory names describe tonal starting points rather than a guaranteed correction for every recording.

Working settings and four presets persist on the SD card in `.tab5-player/eq-a.bin` and `eq-b.bin`. The newest valid record is selected by sequence and CRC. Saving writes the older slot, flushes and verifies it; errors retain the previous valid copy. Save status reports an unavailable card or deferred save during scanning.

The 13-byte EQ configuration and 97-byte storage record remain compatible. The storage layer reads legacy records without rewriting or deleting their four saved slots. During playback and when a curve is loaded, the controller/engine use the same original 0 dB base, ignoring legacy preamp values such as −12 or −24 dB. Saving a new working curve records that fixed value. No existing bands or saved enabled states are migrated away.

For a flat/boost comparison, keep the main volume fixed and allow any previous LIMIT reduction to recover to 0.0 dB. Normal, flat EQ and EQ OFF produce the same level for the same input once transitions/recovery complete. This is a fixed gain policy, not a loudness normalizer: Bass+ and Reverb intentionally alter sound and can change perceived loudness. Normal, flat EQ and EQ OFF restore source-level digital PCM after transitions and limiter recovery. At this original unity level, boosting already loud music can exceed the available headroom and engage limiting. Keeping the original loudness does not guarantee unchanged mids/highs during actual overload.

All bands are retained in presets. A band at or above 47.5% of the source sample rate is dimmed and bypassed, rather than shifted to another frequency. In particular, 20 kHz is available at 44.1/48 kHz and above, but not at 32 kHz. This does not imply that every speaker or listener reproduces that frequency.

## Audio and responsiveness

The filter response follows the [RBJ peaking EQ equations](https://www.w3.org/TR/audio-eq-cookbook/), realized with a topology-preserving state-variable filter for low-frequency precision. Processing uses a fixed 1,228-byte DSP state with no per-block allocation, SD access or UI work. Filter coefficients update only when the configuration changes. The response scan and its automatic attenuation are removed. A 20 ms crossfade handles filter, preset and bypass changes. Fresh-track and seek resets configure the actual fixed gain before the first sample, so they do not apply an unintended attenuation or startup gain ramp.

The signal stays floating-point through the filters and crossfade. One stereo-linked sample-peak limiter follows the crossfade, before PCM16 conversion. It responds to an actual over-limit signal, not to the mere presence of a boosted band. Its envelope survives filter changes. A 50 ms hold and 250 ms exponential release time constant avoid repeatedly restoring gain between adjacent bass peaks; attenuation can continue after a peak has passed, and complete recovery takes longer than one time constant. No extra fixed headroom is inserted; reduce the boosts if the limiter remains active.

When the limiter is inactive, the requested filter response and the fixed base gain determine the level. At genuine overload, the limiter reduces both channels and the entire mix, including mids and highs, to preserve stereo balance. It cannot add arbitrary bass boost to an already full-scale signal while simultaneously keeping every other frequency unchanged and avoiding clipping. This is an emergency sample-peak guard, not a true-peak/lookahead mastering limiter or a guarantee against analog speaker/headphone distortion. Its release also affects recovery after bypassing a heavily limited signal.

Reverb uses 61% direct signal and 38% genuine diffuse tail, without separate early-reflection taps or dynamic ducking. The dry path stays at source rate; the warm wet path runs at no more than 24 kHz with bounded input filtering, interpolation and output reconstruction filtering. The absolute mix sum is 0.99; fixed state is 38,908 bytes. The tested complete wet response reaches −60 dB integrated energy in 0.5386–0.5624 seconds. The direct component is about 4.3 dB below Normal, so perceived loudness need not match: the restored 0 dB player base is not an equal-loudness processor.

The same DSP output feeds local playback and the Pico Bluetooth route. The fixed-gain EQ stage precedes Bass+/Reverb and the existing turntable-speed stage. The visualizer follows the same 0 dB policy, retaining its original display range without changing output PCM. Input events use a bounded ordered ring: consecutive slider edits coalesce, while Save/Load/Flat and other buttons preserve their position in the event sequence. UI refresh waits for newer queued edits; no preset file access occurs while holding the LVGL lock.

LIMIT updates separately from configuration controls, so metering does not reset sliders or write presets. It samples the largest reduction in the latest PCM block about every 100 ms when the display lock is available; it is not a latched history of every intervening overload. It continues to show measured limiter recovery after EQ OFF and clears stale readings when playback pauses or stops. The signal can undergo further processing in Bass+/Reverb, speed and the Bluetooth path; the EQ meter describes this EQ stage, not a measurement of final analog output or intersample peaks.

## Why no setting-derived automatic compensation

FabFilter Pro-Q documents Auto Gain as an optional compensation inferred from EQ settings, separate from its Output Gain. Its internal floating-point headroom does not remove the need to manage level before subsequent output stages. [FabFilter Pro-Q output options](https://prod.fabfilter.com/help/pro-q/using/output)

Ableton EQ Eight likewise offers a global Gain control to manage the increased level caused by boosts. Ableton's Limiter distinguishes actual ceiling crossings and shows gain reduction. These support separating equalization, level management and measured overload protection. Aurora keeps the explicitly requested original 0 dB level and one listening-volume control; it does not add automatic gain compensation merely because a band is boosted. [Ableton EQ Eight and Limiter](https://www.ableton.com/en/manual/live-audio-effect-reference/)

Short-lookahead or no-lookahead protection has tradeoffs: strong limiting can distort or pump, and a release envelope can attenuate later material. Aurora's bounded sample-peak guard is not an implementation of a commercial mastering limiter. [FabFilter Pro-L advanced settings](https://www.fabfilter.com/help/pro-l/using/advancedsettings)

## Flash layout

The installed Tab5 keeps M5Launcher at `0x10000` and Aurora at `0x1a0000`. Aurora's partition expands from `0x5f0000` to `0xc00000` (12 MiB), ending at `0xda0000`. Only the size field and partition-table MD5 change; other partition addresses, sizes and flags remain unchanged. ESP-IDF's parser verifies the resulting table. The device's protected system is verified before and after installation.

Application BIN is the Aurora program for the existing launcher slot. Standalone BIN includes a separate bootloader/partition layout and is not the image installed in the tested M5Launcher configuration. The per-device migration table and recovery backups remain in the development workspace, not in the distributable package.

## Verification

The revised DSP passes 170 measured PCM response cases with maximum error 0.00117 dB and worst measured THD+N −73.46 dB. In 27 two-tone cases, +6 dB at 63 Hz leaves the 2/4 kHz component within 0.00319 dB of its manual-preamp level without limiter activation. The actual unity-level engine measured +6.0000 dB bass, +0.0010 dB high-band change and −0.0019 dB high-band change for a quiet two-tone signal with LIMIT 0. Normal/flat EQ/EQ OFF and legacy preamps 0/−24 produced source-identical PCM. Fresh starts and seek resets retained unity from the first sample. All eight actual factory curves and Bass+/Reverb were checked for legacy-preamp independence, and Reverb changed the output PCM. At real overload, the hot bass-boost case measured LIMIT 2.160 dB and high-band −2.162 dB: restoring original loudness does not remove that protection. Separate passing tests cover overload and persistent limiter recovery, the live meter in real LVGL, corrupted/interrupted preset saves and ordered controller events.

The factory-curve response check evaluates eight curves at nine sample rates and 4,097 frequency points per curve/rate using an independent double-precision RBJ model. It checks steady-state response headroom, not every waveform transient. The real-LVGL fixture exercises every factory choice through 44-pixel touch rows, Flat behavior, absence of PREAMP/Headroom controls, preservation of User memories, and the extracted production keyboard dispatcher. Controller tests separately cover factory/Save ordering, invalid factory indexes, fixed-gain behavior when loading legacy PREAMP values and save failure/retry handling. These are host tests, not new device measurements.

These finite host tests do not establish analog quality or hardware deadlines. The original candidate and revised image are distinguished by hash; listening and preset confirmations for the first do not validate the second. Final hardware timing and device checks were recorded separately and are not published.
