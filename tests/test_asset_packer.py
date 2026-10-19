"""Build-time APK1 optimizer: independent exhaustive shortest-path oracle."""
import importlib.util
import itertools
from pathlib import Path
import random
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("asset_packer", ROOT / "tools/pack-assets.py")
PACKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKER)


def unpack(stream):
    assert stream[:4] == b"APK1"
    unit = stream[4]
    declared = struct.unpack_from("<I", stream, 5)[0]
    result = bytearray()
    position, packets = 9, 0
    while position < len(stream):
        token = stream[position]
        position += 1
        count = (token & 127) + 1
        length = unit if token & 128 else unit * count
        payload = stream[position:position + length]
        assert len(payload) == length
        result += payload * count if token & 128 else payload
        position += length
        packets += 1
    assert len(result) == declared
    return bytes(result), packets


def oracle(data, unit):
    """Intentionally simple O(128*N) reference, separate from monotonic queues."""
    pixels = [data[i:i + unit] for i in range(0, len(data), unit)]
    best = [(0, 0)]
    for end in range(1, len(pixels) + 1):
        candidates = []
        same = True
        for count in range(1, min(128, end) + 1):
            start = end - count
            candidates.append((best[start][0] + 1 + unit * count, best[start][1] + 1))
            same = same and pixels[start] == pixels[end - 1]
            if same:
                candidates.append((best[start][0] + 1 + unit, best[start][1] + 1))
        best.append(min(candidates))
    return best[-1]


def check(data, unit):
    packed = PACKER.pack(data, unit)
    decoded, packets = unpack(packed)
    assert decoded == data
    assert (len(packed) - 9, packets) == oracle(data, unit)
    assert packed == PACKER.pack(data, unit), "non-deterministic packing"


def main():
    tests = 0
    for unit in (1, 2, 3):
        for length in range(11):
            for pattern in itertools.product((0, 1), repeat=length):
                check(b"".join(bytes([p]) * unit for p in pattern), unit)
                tests += 1
        for length in (1, 2, 3, 126, 127, 128, 129, 130, 255, 256, 257, 385):
            for pixels in ([3] * length, [n % 2 for n in range(length)],
                           [n // 2 % 2 for n in range(length)]):
                check(b"".join(bytes([p]) * unit for p in pixels), unit)
                tests += 1
    rng = random.Random(136)
    for _ in range(300):
        unit = rng.randrange(1, 4)
        pixels = []
        while len(pixels) < 400:
            pixel = bytes(rng.randrange(5) for _ in range(unit))
            pixels.extend([pixel] * rng.choice((1, 2, 3, 4, 16, 127, 128, 129)))
        check(b"".join(pixels[:rng.randrange(1, 401)]), unit)
        tests += 1
    for data, unit in ((b"x", 0), (b"x", 2), (b"xxx", 4)):
        try:
            PACKER.pack(data, unit)
        except ValueError:
            pass
        else:
            raise AssertionError("invalid input accepted")
    # Run of two 3-byte pixels must use four bytes, not a seven-byte literal.
    assert len(PACKER.pack(b"abcabc", 3)) == 13
    print(f"PASS: {tests} exhaustive/boundary/random cases; optimal bytes and packet count,")
    print("      deterministic output, unchanged decoded bytes, invalid input rejected.")
    ztests = 0
    for length in (1, 2, 31, 256, 32767, 32768, 32769, 100000):
        for data in (b"\xff" * length, bytes(rng.randrange(256) for _ in range(length))):
            packed = PACKER.pack_zlib(data)
            assert packed[:4] == b"APZ1"
            assert struct.unpack_from("<I", packed, 4)[0] == len(data)
            decoder = zlib.decompressobj()
            assert decoder.decompress(packed[8:]) + decoder.flush() == data
            assert decoder.eof and not decoder.unused_data and not decoder.unconsumed_tail
            assert packed == PACKER.pack_zlib(data)
            ztests += 1
    try:
        PACKER.pack_zlib(b"")
    except ValueError:
        pass
    else:
        raise AssertionError("empty APZ1 resource accepted")
    print(f"PASS: {ztests} APZ1 zlib round trips across dictionary boundaries, exact headers, deterministic bytes, empty input rejected.")


if __name__ == "__main__":
    main()
