"""Re-derive detector streams from raw 400 Hz FIFO frames (decode_fifo.py raw400 CSV).

usage: python rebox.py raw400-bootN.csv OUT.csv MODE
MODE ph0  pairs (0,1),(2,3).. per segment run (= all-bootN.csv; a perturbation, the firmware feeds raw)
     ph1  pairs (1,2),(3,4)..: the same signal at the other box phase
     raw  every frame at 400 Hz (integer ms, so 2-3 ms steps)
Output: t_ms,x_mg,y_mg,z_mg (old TAP_STREAM layout).
"""
import csv, sys
src, out, mode = sys.argv[1], sys.argv[2], sys.argv[3]
runs, cur, last = [], [], None
for r in csv.DictReader(open(src)):
    key, idx = r['seg'], int(r['idx'])
    if last is None or key != last[0] or idx != last[1] + 1:
        if cur: runs.append(cur)
        cur = []
    cur.append((float(r['t_ms']), int(r['x']), int(r['y']), int(r['z'])))
    last = (key, idx)
if cur: runs.append(cur)
rows = []
for run in runs:
    if mode == 'raw':
        rows += [(round(t), round(x * 1000 / 4096), round(y * 1000 / 4096), round(z * 1000 / 4096)) for t, x, y, z in run]
        continue
    start = 0 if mode == 'ph0' else 1
    for j in range(start + 1, len(run), 2):
        a, b = run[j - 1], run[j]
        rows.append((round(b[0]), *(round((a[c] + b[c]) * 0.5 * 1000 / 4096) for c in (1, 2, 3))))
rows.sort()
clean = [r for i, r in enumerate(rows) if i == 0 or r[0] > rows[i - 1][0]]
with open(out, 'w') as f:
    f.write('t_ms,x_mg,y_mg,z_mg\n')
    f.writelines(f'{t},{x},{y},{z}\n' for t, x, y, z in clean)
print(f'{out}: {len(clean)} samples ({len(rows) - len(clean)} dropped for equal timestamps)')
