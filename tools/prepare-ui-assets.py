"""Build native-size RGB565 firmware assets from the unchanged design reference.

This is an asset encoding step, not an alternative UI renderer. The original
reference.png remains in the source package; the firmware embeds only these two
decorative regions. Requires Pillow. Run from any directory.
"""
from pathlib import Path
from PIL import Image
import struct

root = Path(__file__).resolve().parents[1]
source = Image.open(root / "main/reference.png").convert("RGB")
# The previous LVGL images used a scale of 196/256 and these exact viewports.
scaled = source.resize((round(source.width * 196 / 256), round(source.height * 196 / 256)), Image.Resampling.BILINEAR)
for name, box in (("ui_art", (10, 86, 405, 454)), ("ui_logo", (12, 14, 184, 73))):
    region = scaled.crop(box)
    stride = (region.width * 2 + 3) & ~3
    output = bytearray(stride * region.height)
    for y in range(region.height):
        for x in range(region.width):
            r, g, b = region.getpixel((x, y))
            struct.pack_into("<H", output, y * stride + x * 2, ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    target = root / "main" / (name + ".rgb565")
    target.write_bytes(output)
    print(f"{target.name}: {region.width}x{region.height}, stride={stride}, {len(output)} bytes")
