from pathlib import Path
import struct
import sys
import zlib
from PIL import Image, ImageDraw

root = Path(sys.argv[1])
root.mkdir(parents=True, exist_ok=True)
Image.new('RGB', (1280, 720), (220, 32, 16)).save(root / 'red.jpg', quality=95)
Image.new('RGB', (1280, 720), (20, 220, 32)).save(root / 'green.png')
Image.new('RGB', (64, 36), (20, 220, 32)).save(root / 'icc-limit.png', icc_profile=b'X' * 65536)
Image.new('RGBA', (640, 360), (255, 0, 0, 128)).save(root / 'alpha.png')
Image.new('RGBA', (1280, 720), (255, 0, 0, 128)).save(root / 'full-alpha.png')
Image.new('1', (173, 107), 1).save(root / 'one-bit.png')
palette = Image.new('P', (117, 201), 1)
palette.putpalette([0, 0, 0, 0, 0, 255] + [0, 0, 0] * 254)
palette.save(root / 'palette.png', bits=2)
Image.new('L', (511, 300), 64).save(root / 'gray.png')
Image.new('LA', (511, 300), (64, 128)).save(root / 'gray-alpha.png')
Image.new('RGB', (640, 360), (220, 32, 16)).save(root / 'progressive.jpg', progressive=True)
Image.new('I;16', (10, 10), 32768).save(root / 'sixteen.png')
Image.new('RGB', (1, 4096), (220, 32, 16)).save(root / 'narrow.jpg', quality=95)
Image.new('RGB', (4096, 4096), (220, 32, 16)).save(root / 'sixteen-mp.jpg', quality=90)
Image.new('RGB', (4097, 16), (220, 32, 16)).save(root / 'too-wide.jpg')
wide = Image.new('RGB', (1600, 720), (255, 0, 0))
ImageDraw.Draw(wide).rectangle((160, 0, 1439, 719), fill=(0, 255, 0))
wide.save(root / 'wide.png')  # Too many PNG pixels; JPEG exercises the crop.
wide.save(root / 'wide.jpg', quality=100, subsampling=0)
portrait = Image.new('RGB', (360, 900), (255, 0, 0))
ImageDraw.Draw(portrait).rectangle((0, 349, 359, 550), fill=(0, 0, 255))
portrait.save(root / 'portrait.png')
portrait.save(root / 'portrait.jpg', quality=100, subsampling=0)
(root / 'corrupt.jpg').write_bytes(b'x' * 80)
(root / 'truncated.jpg').write_bytes((root / 'red.jpg').read_bytes()[:100])
png = bytearray((root / 'green.png').read_bytes())
png[16:24] = struct.pack('>II', 65535, 65535)
png[29:33] = struct.pack('>I', zlib.crc32(png[12:29]))
(root / 'bomb.png').write_bytes(png)
(root / 'corrupt.png').write_bytes((root / 'green.png').read_bytes()[:70])
def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
(root / 'inflate-bomb.png').write_bytes(
    b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 6, 0, 0, 0))
    + chunk(b'IDAT', zlib.compress(b'\0' * 1024 * 1024)) + chunk(b'IEND', b''))
with (root / 'large.jpg').open('wb') as f:
    f.seek(8 * 1024 * 1024)
    f.write(b'x')
with (root / 'memory.png').open('wb') as f:
    f.write((root / 'green.png').read_bytes())
    f.seek(8 * 1024 * 1024 - 1)
    f.write(b'\0')
listing = root / 'listing'
listing.mkdir(exist_ok=True)
for i in range(140):
    (listing / f'{i:03}.JPG').write_bytes(b'x')
(listing / 'invalid.txt').write_bytes(b'x')
(listing / 'directory.png').mkdir(exist_ok=True)
scan_limit = root / 'scan-limit'
scan_limit.mkdir(exist_ok=True)
for i in range(2050):
    (scan_limit / f'{i:04}.txt').write_bytes(b'x')
print('Generated wallpaper fixtures')
for scenario in ('startup-valid', 'startup-png', 'startup-corrupt', 'startup-missing', 'startup-fallback'):
    (root / scenario).mkdir(exist_ok=True)
(root / 'startup-valid' / 'startup.jpg').write_bytes((root / 'red.jpg').read_bytes())
(root / 'startup-valid' / 'startup.png').write_bytes((root / 'green.png').read_bytes())
(root / 'startup-png' / 'startup.png').write_bytes((root / 'green.png').read_bytes())
(root / 'startup-corrupt' / 'startup.jpg').write_bytes((root / 'truncated.jpg').read_bytes())
(root / 'startup-corrupt' / 'startup.png').write_bytes((root / 'corrupt.png').read_bytes())
(root / 'startup-fallback' / 'startup.jpg').write_bytes((root / 'truncated.jpg').read_bytes())
(root / 'startup-fallback' / 'startup.png').write_bytes((root / 'green.png').read_bytes())
