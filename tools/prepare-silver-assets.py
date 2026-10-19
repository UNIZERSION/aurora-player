"""Encode unchanged reference pixels for the native LVGL skin.

The source PNG is preserved. These viewports are the native UI's clipping and
scaling, not a generated replacement design; all live controls are LVGL objects.
Requires Pillow. Do not run this work in a framebuffer refresh callback.
"""
from pathlib import Path
from PIL import Image
import struct

root = Path(__file__).resolve().parents[1]
source = Image.open(root / 'main/reference-silver.png').convert('RGB')
shell = source.crop((28, 28, 1642, 907)).resize((1280, 720), Image.Resampling.LANCZOS)
thumb = source.crop((90, 138, 533, 615)).resize((56, 56), Image.Resampling.LANCZOS)
for name, region in [('ui_shell', shell), ('ui_dragon_thumb', thumb)]:
    stride = (region.width * 2 + 3) & ~3
    output = bytearray(stride * region.height)
    for y in range(region.height):
        for x in range(region.width):
            r, g, b = region.getpixel((x, y))
            struct.pack_into('<H', output, y * stride + x * 2,
                             ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    path = root / 'main' / f'{name}.rgb565'
    path.write_bytes(output)
    print(f'{path.name}: {region.width}x{region.height}, stride={stride}, {len(output)} bytes')
