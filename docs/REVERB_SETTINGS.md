# Adjustable Reverb

The stereo room effect has four controls. The main
panel keeps its Reverb preset button; Settings → Audio → Reverb settings opens
the detailed controls. The preview switch selects Reverb or Normal. Reset
restores Amount 38%, Decay 1.5 s, Low cut 0 and High cut 1.4 kHz without changing the master volume.

| Control | Range | Default | Meaning |
| --- | --- | --- | --- |
| Amount | 0–60% | 38% | Blend between original audio and the room return. Zero is dry audio. |
| Decay | 0.5–4.0 seconds | 1.5 seconds | Nominal low-frequency 60 dB decay. Higher frequencies fade sooner. |
| High cut | 100–20000 Hz | 1400 Hz | Upper wet-signal cutoff and feedback damping: lower is warmer, higher is brighter. |
| Low cut | 0–19900 Hz | 0 Hz (off) | Lower wet-signal cutoff. Zero bypasses the low cut. |

The frequency controls use 100 Hz steps and maintain at least 100 Hz between the
lower and upper setting; dragging one through the other clamps the moved
control. The allowed band spans 0 Hz to 20 kHz. The real DSP upper cutoff is bounded
to 45% of the wet processing sample rate to stay below Nyquist; the lower cutoff
is then bounded to 99% of that upper cutoff. Consequently a low-rate source
cannot acquire frequencies it does not contain. At 44.1 kHz the upper guard is
19.845 kHz; at 48 kHz a requested 20 kHz is applied exactly.

Low cut is a two-pole high-pass (12 dB/octave, approximately −6 dB at its cutoff).
High cut includes a one-pole low-pass (6 dB/octave, −3 dB at its cutoff), plus the
existing frequency-dependent feedback damping. Both filters affect only the
reverb return; dry music is never filtered by these controls. Tone in older
saved files becomes High cut, and Low cut starts bypassed. The earlier 5 kHz
upper limit and fixed 5 kHz reconstruction bottleneck have been removed.

The measured integrated-energy decay at the default setting remains about
1.33–1.36 seconds across 8–96 kHz source rates. The number on the Decay control
is a filter design parameter, not a promise that every frequency or musical
passage remains audible for exactly that duration.

Slider changes only publish a small configuration to the audio engine. The
output task interpolates feedback, damping and amount with a 50 ms time
constant; the wider frequency sweeps and low-cut bypass use approximately 200 ms.
The existing room tail is retained. There is no allocation, file I/O,
delay-line resizing or per-sample coefficient calculation on slider events.
The stored setting applies immediately when a new track or seek resets the
DSP; it does not briefly use the default room configuration first.

The four-delay feedback network, wet-only linked peak protection and source-rate
dry path are retained. The same processed PCM feeds local audio and the Pico
Bluetooth bridge. Changing Bluetooth codec settings does not bypass Reverb.
At the default Amount the released mix remains 61% dry plus 38% wet. Increasing
Amount intentionally changes that blend. It is not a master volume control.

Feedback gains remain below one throughout interpolation. The protected wet
signal and dry/wet weight sum prevent output clipping at any permitted setting.
This is sample-peak protection, not a true-peak limiter. Extreme sustained
resonances can invoke wet gain protection; it never attenuates the separate dry
signal to control a wet peak.

The wet network now operates at source rate through 48 kHz. Sources above 48 kHz
use a divisor of two with anti-alias and reconstruction filtering. Fixed delay
storage grows from 4800 to 9600 floats per voice; the complete two-voice DSP state
is 77532 bytes, allocated once in PSRAM. Changing controls does not resize it.
The default room's filtering is now applied to early reflections as well as
feedback, so its PCM is not byte-identical to the previous engine. Its dry
level, nominal decay and mix are unchanged, and the prior audible-tail and
distortion thresholds still pass.

Configuration uses independent 32-byte versioned A/B records,
`/.tab5-player/reverb-a.bin` and `reverb-b.bin`, with sequence numbers and CRC32.
The normal controller save debounce handles slider changes; there is no SD
write on each touch event. Saves flush, sync, close and verify readback before
reporting success. A corrupted or incomplete newest record falls back to the
previous valid copy. Missing records use the defaults. The established general
Settings and user EQ-preset formats are unchanged.
Version 2 uses two previously reserved bytes for Low cut and retains the same
record size. Version 1 records are read without rewriting them during startup;
the next user change saves version 2 in the other slot. Invalid versions,
out-of-range or crossed cutoffs are rejected even with a valid CRC.

Validation covers the production DSP at eleven rates, parameter extremes with
full-scale inputs, continuous setting changes without clearing the tail,
block-boundary invariance, audible-tail measurements, original FX quality
regressions, production threaded-engine output and A/B corruption recovery.
The real LVGL modal test exercises touch sliders, bounds, held-slider refresh,
buttons, rendering consistency and widget teardown. Host timing is not an
ESP32-P4/PSRAM deadline measurement; device measurements and headphone listening
remain separate validation steps.

The expanded-band regression also checks all 88001 integer source rates 8–96 kHz,
analytic cutoff responses, 42 PCM maximum first-sample change across 660 extreme
parameter updates, and the dry contribution at every tested band. In the
actual room path, Low cut 1 kHz suppresses 63 Hz by 48.09 dB; widening High cut from
1.4 kHz to 20 kHz increases the 10 kHz wet response by 15.92–17.13 dB at 44.1/48 kHz.
