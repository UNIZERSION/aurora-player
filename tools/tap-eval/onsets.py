"""List impact onsets in a 200 Hz CSV window with a low threshold.

usage: python onsets.py all.csv FROM_MS TO_MS [thr_g=0.08]
An onset is |a - gravity| >= thr after >= 60 ms below thr/2; its peak is the
max over the next 40 ms. Gravity: 600 ms one-pole filter.
"""
import csv, math, sys
f, a, b = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
thr = float(sys.argv[4]) if len(sys.argv) > 4 else 0.08
rows = [(int(r[0]), int(r[1]) / 1000, int(r[2]) / 1000, int(r[3]) / 1000) for r in list(csv.reader(open(f)))[1:]]
rows = [r for r in rows if a - 2000 <= r[0] <= b]
g = list(rows[0][1:]); run_start = rows[0][0]; quiet = True; pre = 0; armed = False; out = []
for i, (t, x, y, z) in enumerate(rows):
    d = [x - g[0], y - g[1], z - g[2]]; m = math.sqrt(sum(v * v for v in d))
    for k in range(3): g[k] += (5 / 605) * d[k]
    if m < thr / 2:
        if not quiet: run_start = t
        quiet = True; armed = False
        continue
    if quiet:
        pre = t - run_start; armed = True
    quiet = False
    if armed and m >= thr:
        armed = False
        if pre >= 60 and t >= a:
            pk = max(math.sqrt(sum((rows[j][k + 1] - g[k]) ** 2 for k in range(3))) for j in range(i, min(i + 9, len(rows))))
            out.append((t, pk, pre))
prev = None
for t, pk, q in out:
    print(f'{t:8d}  +{(t - prev) if prev else 0:6d} ms  peak {pk:5.2f} g  quiet-before {q:6d} ms')
    prev = t
