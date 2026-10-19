"""Extend only the native FILE FILTER region using its approved button skin.

The three original button states remain byte-identical. Two same-size skins
reuse WAV's border/gradient and add centered FLAC/AAC labels. An immutable small
region of the approved shell makes repeated runs idempotent; all pixels outside
that region are preserved, including later refinements elsewhere in the UI.
"""
from pathlib import Path
import re
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "main"
REF = ROOT / "tools/aurora-reference"
BASE_BYTES = 1004672
REGION = (768, 550, 1045, 684)


def decode(data, w, h, stride):
    a = np.frombuffer(data, dtype="<u2").reshape(h, stride // 2)[:, :w]
    return np.stack(((a >> 11) * 255 // 31, ((a >> 5) & 63) * 255 // 63,
                     (a & 31) * 255 // 31), -1).astype(np.uint8)


def encode(a, stride=None):
    a = np.asarray(a, dtype=np.uint16)
    rgb = ((a[..., 0] >> 3) << 11) | ((a[..., 1] >> 2) << 5) | (a[..., 2] >> 3)
    if stride is None:
        return rgb.astype("<u2").tobytes()
    padded = np.zeros((rgb.shape[0], stride // 2), dtype="<u2")
    padded[:, :rgb.shape[1]] = rgb
    return padded.tobytes()


def clear_text(a, box):
    x0, y0, x1, y1 = box
    old = a[y0:y1, x0:x1].copy()
    t = np.linspace(0, 1, x1 - x0)[None, :, None]
    background = a[y0:y1, x0-1:x0].astype(float) * (1-t) + a[y0:y1, x1:x1+1] * t
    a[y0:y1, x0:x1] = np.rint(background).astype(np.uint8)
    return old, background


def main():
    original = (OUT / "ui_shell.rgb565").read_bytes()
    shell = decode(original, 1280, 720, 2560)
    region_file = REF / "file-filter-original-region.png"
    x0, y0, x1, y1 = REGION
    if not region_file.exists():
        Image.fromarray(shell[y0:y1, x0:x1]).save(region_file)
    shell[y0:y1, x0:x1] = np.array(Image.open(region_file).convert("RGB"))

    # Reuse the original header's glyph coverage, lifting it above the two rows.
    old, background = clear_text(shell, (771, 565, 1040, 583))
    ink = np.array([39., 45., 39.])
    alpha = np.clip(np.mean((background-old) / np.maximum(1., background-ink), -1), 0, 1)[..., None]
    target = shell[550:568, 771:1040]
    target[:] = np.rint(target*(1-alpha) + ink*alpha).astype(np.uint8)
    clear_text(shell, (835, 652, 976, 671))
    raw = bytearray(original)
    # Do not re-quantize any pixel elsewhere in the shell.
    patch = encode(shell[y0:y1, x0:x1])
    for y in range(y1-y0):
        start = ((y+y0)*1280+x0)*2
        raw[start:start+(x1-x0)*2] = patch[y*(x1-x0)*2:(y+1)*(x1-x0)*2]
    (OUT / "ui_shell.rgb565").write_bytes(raw)

    atlas = bytearray((OUT / "aurora_controls.bin").read_bytes()[:BASE_BYTES])
    geometry = (OUT / "aurora_geometry.h").read_text()
    rects = [list(map(int, r.split(','))) for r in re.findall(r'\{([0-9,]+)\}', geometry)][:22]
    for i in (16, 17, 18):
        rects[i][1] = 567
    # Center of both banks is x=904.5. Their cropped native button sizes match.
    for caption, x in (("FLAC", 814), ("AAC", 906)):
        offset = len(atlas)
        rects.append([x, 627, 89, 58, 180, offset])
        for state in range(4):
            start = rects[18][5] + state*180*58
            image = decode(atlas[start:start+180*58], 89, 58, 180)
            clear_text(image, (19, 17, 71, 42))
            high = Image.fromarray(image).resize((267, 174), Image.Resampling.NEAREST)
            font = ImageFont.truetype("C:/Windows/Fonts/consolab.ttf", 43)
            color = (32, 37, 21) if state & 1 else (194, 199, 192)
            if state & 2:
                color = tuple(min(255, round(c*1.25)) for c in color)
            # CSS button text is centered; pressed states descend by1.2physicalpx.
            ImageDraw.Draw(high).text((133.5, (29+(1.2 if state&2 else 0))*3), caption,
                                      font=font, anchor="mm", fill=color)
            result = high.resize((89, 58), Image.Resampling.LANCZOS)
            atlas.extend(encode(result, 180))
    (OUT / "aurora_controls.bin").write_bytes(atlas)
    (OUT / "aurora_geometry.h").write_text(
        '#pragma once\nstruct AuroraRect{int x,y,w,h,stride,offset;};\n'
        'static constexpr AuroraRect aurora_controls[]={\n'+
        ',\n'.join('{'+','.join(map(str,r))+'}' for r in rects)+
        '\n};\nstatic constexpr int AURORA_CONTROLS_BYTES='+str(len(atlas))+';\n')
    print('FILE FILTER: two rows, unchanged original button pixels,', len(atlas), 'atlas bytes')


if __name__ == "__main__":
    main()
