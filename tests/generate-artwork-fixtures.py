"""Small synthetic JPEG/PNG/ID3 fixtures for the actual artwork loader."""
from pathlib import Path
from PIL import Image
import io
import struct
import sys
import zlib

root = Path(sys.argv[1])
root.mkdir(parents=True, exist_ok=True)

def picture(format, color=(240, 32, 16), size=(640, 400), **options):
    stream = io.BytesIO()
    Image.new("RGB", size, color).save(stream, format=format, **options)
    return stream.getvalue()

def ss(value):
    return bytes([(value >> 21) & 127, (value >> 14) & 127, (value >> 7) & 127, value & 127])

def tag(image, version=3, unsync=False):
    is_png = image.startswith(b"\x89PNG")
    payload = b"\0" + (b"PNG" if is_png else b"JPG") + b"\3\0" + image if version == 2 else b"\0image/" + (b"png" if is_png else b"jpeg") + b"\0\3\0" + image
    if unsync:
        payload = payload.replace(b"\xff", b"\xff\0")
    if version == 2:
        frame = b"PIC" + len(payload).to_bytes(3, "big") + payload
    else:
        frame = b"APIC" + (ss(len(payload)) if version == 4 else struct.pack(">I", len(payload))) + (b"\0\2" if unsync else b"\0\0") + payload
    return b"ID3" + bytes([version, 0, 0]) + ss(len(frame)) + frame

red = picture("JPEG", quality=95)
green = picture("PNG", color=(16, 240, 32), size=(320, 640))
cases = {
    "apic-jpeg": tag(red),
    "apic-png": tag(green),
    "v22-pic": tag(green, 2),
    "v24-unsync": tag(red, 4, True),
    "corrupt": b"ID3\3\0\0" + ss(10) + b"APIC\xff\xff\xff\xff\0\0",
    "missing": b"not an audio header",
    "folder": b"no embedded image",
    "too-large": b"ID3\3\0\0" + ss(2 * 1024 * 1024 + 1),
    "thin-jpeg": tag(picture("JPEG", size=(1, 4096), quality=95)),
    "thin-png": tag(picture("PNG", color=(16, 240, 32), size=(2048, 1))),
}
for name, data in cases.items():
    folder = root / name
    folder.mkdir(exist_ok=True)
    (folder / "track.mp3").write_bytes(data)
(root / "folder" / "COVER.PNG").write_bytes(green)
# Progressive embedded JPEG must fail safely and use the same-folder PNG.
folder = root / "progressive-fallback"
folder.mkdir(exist_ok=True)
(folder / "track.mp3").write_bytes(tag(picture("JPEG", progressive=True)))
(folder / "cover.png").write_bytes(green)
# WAV header ID3: cover before its audio payload.
folder = root / "wav-id3"
folder.mkdir(exist_ok=True)
id3 = tag(red)
chunks = b"id3 " + struct.pack("<I", len(id3)) + id3 + (b"\0" if len(id3) & 1 else b"") + b"data\0\0\0\0"
(folder / "track.wav").write_bytes(b"RIFF" + struct.pack("<I", len(chunks) + 4) + b"WAVE" + chunks)
# Valid CRC, enormous PNG dimensions: rejected before decompression/allocation.
folder = root / "png-bomb"
folder.mkdir(exist_ok=True)
(folder / "track.mp3").write_bytes(b"empty")
ihdr = b"IHDR" + struct.pack(">IIBBBBB", 50000, 50000, 8, 6, 0, 0, 0)
(folder / "cover.png").write_bytes(b"\x89PNG\r\n\x1a\n" + struct.pack(">I", 13) + ihdr + struct.pack(">I", zlib.crc32(ihdr)))
def flac_picture(image, kind=3):
    mime = b'image/png' if image.startswith(b'\x89PNG') else b'image/jpeg'
    return struct.pack('>II', kind, len(mime)) + mime + struct.pack('>I', 0) + struct.pack('>IIIII', 640, 400, 24, 0, len(image)) + image

def flac_block(kind, data, last=False):
    return bytes([kind | (128 if last else 0)]) + len(data).to_bytes(3, 'big') + data

folder = root / 'flac-front'
folder.mkdir(exist_ok=True)
# Prefer front (red) over a preceding generic picture (green).
(folder / 'track.flac').write_bytes(b'fLaC' + flac_block(0, bytes(34)) + flac_block(6, flac_picture(green, 0)) + flac_block(6, flac_picture(red), True))
folder = root / 'flac-invalid'
folder.mkdir(exist_ok=True)
(folder / 'track.flac').write_bytes(b'fLaC' + flac_block(6, struct.pack('>II', 3, 0xffffffff) + bytes(24), True))
folder = root / 'flac-folder'
folder.mkdir(exist_ok=True)
(folder / 'track.flac').write_bytes(b'fLaC' + flac_block(0, bytes(34), True))
(folder / 'cover.png').write_bytes(green)
print(f"Artwork fixtures: {root}")
