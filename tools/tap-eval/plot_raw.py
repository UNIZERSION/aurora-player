"""Plot short windows of the raw 400 Hz FIFO frames, one strip per window.

usage: python plot_raw.py raw400-bootN.csv out.png START_MS:END_MS[:label] ...
Each strip shows x (red), y (green), z (blue) minus their median over the
first 100 ms of the strip, in g, with a 100 ms grid. Pillow only.
"""
import csv
import sys

from PIL import Image, ImageDraw

W, H, PAD = 1400, 220, 30


def main():
    path, out = sys.argv[1], sys.argv[2]
    wins = []
    for spec in sys.argv[3:]:
        parts = spec.split(':')
        wins.append((float(parts[0]), float(parts[1]), parts[2] if len(parts) > 2 else spec))
    lo = min(w[0] for w in wins)
    hi = max(w[1] for w in wins)
    rows = []
    with open(path) as f:
        for r in csv.DictReader(f):
            t = float(r['t_ms'])
            if lo <= t <= hi:
                rows.append((t, int(r['x']) / 4096, int(r['y']) / 4096, int(r['z']) / 4096))
    img = Image.new('RGB', (W, H * len(wins)), 'white')
    d = ImageDraw.Draw(img)
    for k, (a, b, label) in enumerate(wins):
        top = k * H
        sel = [r for r in rows if a <= r[0] <= b]
        if not sel:
            continue
        base = []
        for c in (1, 2, 3):
            head = sorted(r[c] for r in sel if r[0] <= a + 100) or [0]
            base.append(head[len(head) // 2])
        span = max(0.1, max(abs(r[c] - base[c - 1]) for r in sel for c in (1, 2, 3)))
        sx = (W - 2 * PAD) / (b - a)
        sy = (H / 2 - PAD) / span
        mid = top + H / 2
        for g in range(int(a // 100 * 100), int(b) + 1, 100):
            x = PAD + (g - a) * sx
            if PAD <= x <= W - PAD:
                d.line([(x, top + 15), (x, top + H - 5)], fill=(225, 225, 225))
        d.line([(PAD, mid), (W - PAD, mid)], fill=(180, 180, 180))
        for c, col in ((1, (220, 0, 0)), (2, (0, 150, 0)), (3, (0, 0, 230))):
            pts = [(PAD + (r[0] - a) * sx, mid - (r[c] - base[c - 1]) * sy) for r in sel]
            d.line(pts, fill=col, width=1)
        d.text((PAD, top + 2), f'{label}  [{a:.0f}..{b:.0f} ms]  full scale +-{span:.2f} g  grid 100 ms', fill='black')
        d.line([(0, top + H - 1), (W, top + H - 1)], fill='black')
    img.save(out)


if __name__ == '__main__':
    main()
