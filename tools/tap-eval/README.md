# Tap detector evaluation (1.3.21)

Tools used to record, label and evaluate the enclosure tap detector
(`main/tap_gesture.*`). The recordings themselves (about 30 MB of CSV)
are not published. They were organized as:

| Data | Content |
|---|---|
| `tap-guided/`, `tap-guided2/`, `tap-stream/` | 2026-10-17 night sessions with 1.3.18 calibration builds: latest sample polled every ~5 ms (145-196 Hz, irregular) |
| `tap-fifo/raw400-boot{0,1}.csv` | 2026-10-17 evening session with the FIFO calibration build: every 400 Hz BMI270 frame (`seg,idx,t_ms,x,y,z`, 4096 LSB/g) |
| `tap-fifo/all-bootN-raw.csv` | the same frames as `t_ms,x_mg,y_mg,z_mg` (what the firmware feeds) |
| `tap-fifo/all-bootN.csv`, `-ph1.csv` | frame pairs box-averaged to 200 Hz at both pair phases (`rebox.py`) |

`labels.csv` lists every labelled window (`source,start_ms,end_ms,expected,name`;
expected 3 / 2 = triple / double, 0 = must stay silent, -1 = not scored).

## Recording

1. Build the calibration firmware: `idf.py -B build-calib -DAURORA_TAP_STREAM=1 build`, flash it.
   It logs every FIFO frame as `TAP_FIFO seg=.. n=.. t=.. hex=..` and keeps a PSRAM
   backlog while no USB host is connected (pocket trials on battery).
2. Capture: `python serial-capture-reconnect.py COM4 5400 tap-fifo.log`.
3. After each guided group: `checkpoint-fifo.ps1 -Name <group> -Log tap-fifo.log -Out tap-fifo`.
4. `decode_fifo.py LOG OUTDIR` writes the CSVs; `rebox.py` derives the 200 Hz and raw
   variants; `plot_raw.py` and `onsets.py` help labelling.

Known issue: with ESP-IDF 5.5.2 the USB-Serial-JTAG console did not resume
after the cable was unplugged and plugged in again (writes keep failing), so
the backlog could not be printed; it was read from PSRAM over JTAG (OpenOCD
`dump_image` of `fifo_log.ring_`) instead.

## Evaluation

```
zig c++ -std=c++17 -O2 -Wall -Wextra -Werror -I ../../main eval.cpp ../../main/tap_gesture.cpp -o eval.exe
eval.exe labels.csv <data-dir> --suite full --stress --ablate
```

- Objective families (all must give 0 false positives and 0 wrong commands):
  as recorded, 200 Hz box phases of the FIFO data, +-2 ms jitter, 200/150 Hz
  box/linear/Lanczos at 8 phases, dropping every 4th or 5th sample, random loss 1-5 %,
  amplitude 0.8-1.5x and sensor gain 0.8/1.2, rotations to 11 poses (false
  positives only), all sessions concatenated. Report-only: 100/125 Hz, 20 %
  loss, +-3 ms jitter with duplicates, 0.6x and 2x.
- `--stress` scales single negative windows 0.5-3x.
- `--ablate` disables each rule alone (on the suite given with `--suite`); a
  negative that then fires is stopped by that rule only. Goal:
  `single_point_rules=0` (1.3.21: 0 of 16 on the full suite).
- `--trace SRC FROM TO` prints every impulse decision; `key=value` overrides
  any field of `TAP_GESTURE_TUNABLES`; `--playing` evaluates with music
  playing.
- `features.cpp` dumps per-impulse features for every onset.
