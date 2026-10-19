"""Decode TAP_FIFO lines (calibration build, BMI270 FIFO at 400 Hz).

usage: python decode_fifo.py LOG OUTDIR

Each TAP_FIFO segment is a gap-free run of sensor frames. Frame times come
from the frame index (sensor clock); one linear fit per segment against the
console read times (t=) maps them to device ms. Writes per boot b:
  raw400-boot<b>.csv  seg,idx,t_ms,x,y,z        raw frames (4096 LSB/g)
  all-boot<b>.csv     t_ms,x_mg,y_mg,z_mg       a perturbation variant: frame
                      pairs box-averaged to 200 Hz, stamped at the 2nd frame
and all.csv (the latest boot) in the old TAP_STREAM layout, so tap_segments,
checkpoint and the replay harness read it unchanged.
"""
import re
import struct
import sys
from pathlib import Path

SESSION = re.compile(r'TAP_STREAM session=(\d+) armed=(\d)')
START = re.compile(r'TAP_FIFO seg=(\d+) start reason=(\w+)')
DATA = re.compile(r'TAP_FIFO seg=(\d+) n=(\d+) t=(\d+) hex=([0-9a-f]+)')
STAMP = re.compile(r'^[IWE] \((\d+)\) ')
RATE = re.compile(r'TAP_FIFO rate frames=(-?\d+) expected=(-?\d+) ok=(\d)')
NOMINAL_MS = 2.5


def fit(lines):
    """Least-squares slope of read time vs last frame index, then the lower
    envelope as intercept: a frame cannot be read before it was sampled."""
    pts = [(n + len(fr) - 1, t) for n, t, fr in lines if fr]
    slope = NOMINAL_MS
    if len(pts) >= 20 and pts[-1][0] - pts[0][0] >= 800:
        mi = sum(p[0] for p in pts) / len(pts)
        mt = sum(p[1] for p in pts) / len(pts)
        var = sum((p[0] - mi) ** 2 for p in pts)
        slope = sum((p[0] - mi) * (p[1] - mt) for p in pts) / var
    intercept = min(t - slope * i for i, t in pts)
    return intercept, slope


def main():
    log, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    boots, boot = {}, 0
    rates = []
    last_stamp = None
    for line in log.read_text(encoding='utf-8', errors='replace').splitlines():
        # The ROM banner is usually lost while USB re-enumerates, so a boot is
        # also recognised by the console timestamp jumping back. Lines
        # recovered from the device's backlog over JTAG carry a RING prefix.
        if 'rst:0x' in line or 'ESP-ROM:' in line:
            boot += 1
            last_stamp = None
            continue
        if s := STAMP.match(line):
            stamp = int(s[1])
            if last_stamp is not None and stamp + 5000 < last_stamp:
                boot += 1
            last_stamp = stamp
        if m := START.search(line):
            boots.setdefault(boot, {}).setdefault(int(m[1]), {'reason': m[2], 'lines': []})
        elif m := DATA.search(line):
            seg = boots.setdefault(boot, {}).setdefault(int(m[1]), {'reason': '?', 'lines': []})
            # A line cut by interleaved console output keeps its whole frames only.
            raw = bytes.fromhex(m[4][:len(m[4]) // 12 * 12])
            frames = [struct.unpack_from('<hhh', raw, i) for i in range(0, len(raw), 6)]
            if frames:
                seg['lines'].append((int(m[2]), int(m[3]), frames))
        elif m := RATE.search(line):
            rates.append((int(m[1]), int(m[2]), m[3] == '1'))
    for b, segs in sorted(boots.items()):
        raw_rows, rows = [], []
        for sid in sorted(segs):
            seg = segs[sid]
            if not seg['lines']:
                continue
            # Backlog lines arrive late and interleaved: order by index, once.
            seen, ordered = set(), []
            for ln in sorted(seg['lines'], key=lambda ln: ln[0]):
                if ln[0] not in seen:
                    seen.add(ln[0])
                    ordered.append(ln)
            seg['lines'] = ordered
            # A lost line leaves a hole in the index: split there.
            runs, run, expect = [], [], None
            for n, t, fr in seg['lines']:
                if expect is not None and n != expect:
                    runs.append(run)
                    run = []
                run.append((n, t, fr))
                expect = n + len(fr)
            runs.append(run)
            for k, run in enumerate(r for r in runs if r):
                a, slope = fit(run)
                frames = [(n + j, f) for n, _, fr in run for j, f in enumerate(fr)]
                base = frames[0][0]
                # Pairs restart with the run, as the firmware restarts them.
                for idx, (x, y, z) in frames:
                    raw_rows.append(f'{sid}.{k},{idx},{a + slope * idx:.1f},{x},{y},{z}')
                for j in range(1, len(frames), 2):
                    (_, f0), (i1, f1) = frames[j - 1], frames[j]
                    if (i1 - base) % 2 == 0:
                        continue
                    t = int(round(a + slope * i1))
                    mg = [round((f0[c] + f1[c]) * 0.5 * 1000 / 4096) for c in range(3)]
                    rows.append((t, *mg))
                span = (frames[-1][0] - base) * slope / 1000
                rate = (len(frames) - 1) / ((frames[-1][0] - base) * slope / 1000) if span > 0 else 0
                print(f'boot {b} seg {sid}.{k} ({seg["reason"]}): {len(frames)} frames, {span:.1f} s, '
                      f'{rate:.1f} Hz by console clock, {slope:.4f} ms/frame')
        rows.sort()
        clean = [r for i, r in enumerate(rows) if i == 0 or r[0] > rows[i - 1][0]]
        if len(clean) != len(rows):
            print(f'boot {b}: dropped {len(rows) - len(clean)} samples with overlapping timestamps')
        (out / f'raw400-boot{b}.csv').write_text('seg,idx,t_ms,x,y,z\n' + '\n'.join(raw_rows) + '\n')
        text = 't_ms,x_mg,y_mg,z_mg\n' + '\n'.join(f'{t},{x},{y},{z}' for t, x, y, z in clean) + '\n'
        (out / f'all-boot{b}.csv').write_text(text)
        (out / 'all.csv').write_text(text)
        if clean:
            print(f'all sessions: boot {b}: {len(clean)} samples, device ms {clean[0][0]}..{clean[-1][0]} -> all-boot{b}.csv')
    if rates:
        bad = [r for r in rates if not r[2]]
        ratio = sum(r[0] for r in rates) / max(1, sum(r[1] for r in rates))
        print(f'rate windows: {len(rates)}, failed {len(bad)}, frames/expected {ratio:.4f}')


if __name__ == '__main__':
    main()
