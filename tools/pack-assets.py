"""Lossless APK1 packing and APZ1 zlib packing for the control atlas.

Only the build-time packet boundaries change. The existing device decoder,
decoded frames, RGB565 pixels and alpha bytes are unchanged. APZ1 is unpacked
once at boot into the same control atlas allocation using the device ROM.
"""
from array import array
from collections import deque
import hashlib
import json
from pathlib import Path
import struct
import zlib
import argparse


def pack_zlib(data: bytes) -> bytes:
    """APZ1 stores an exact raw length and a checksum-protected zlib stream."""
    if not data or len(data) > 0xFFFFFFFF:
        raise ValueError("APZ1 needs a non-empty resource with a 32-bit length")
    return b"APZ1" + struct.pack("<I", len(data)) + zlib.compress(data, 9)


def pack(data: bytes, unit: int) -> bytes:
    """Return a minimum-byte APK1 stream, minimizing packet count on ties.

    DP[i] is the best encoding of the first i pixels. A literal ending at i
    costs DP[j] + 1 + unit*(i-j), with i-128 <= j < i. Its best predecessor is
    the sliding-window minimum of DP[j]-unit*j. A repeat costs DP[j]+1+unit
    within the same-color run and the same 128-pixel window. Two monotonic
    deques maintain these minima in O(N) time. Only the chosen signed packet
    length is stored per pixel (two bytes); no O(128*N) scan or full DP table.
    """
    if unit not in (1, 2, 3) or len(data) % unit:
        raise ValueError("APK1 needs complete 1-, 2- or 3-byte pixels")
    if len(data) > 0xFFFFFFFF:
        raise ValueError("APK1 length exceeds its 32-bit header")

    count = len(data) // unit
    choices = array("h", [0])
    literals, repeats = deque(), deque()
    cost = packets = 0
    previous = None
    for end in range(1, count + 1):
        start = end - 1
        pixel = data[start * unit:end * unit]
        if pixel != previous:
            repeats.clear()
            previous = pixel

        # Candidate tuples: (weighted byte cost, preceding packet count, index).
        literal_key = (cost - unit * start, packets)
        while literals and literals[-1][:2] >= literal_key:
            literals.pop()
        literals.append((*literal_key, start))
        repeat_key = (cost, packets)
        while repeats and repeats[-1][:2] >= repeat_key:
            repeats.pop()
        repeats.append((*repeat_key, start))
        oldest = end - 128
        while literals[0][2] < oldest:
            literals.popleft()
        while repeats[0][2] < oldest:
            repeats.popleft()

        literal = (literals[0][0] + unit * end + 1, literals[0][1] + 1)
        repeat = (repeats[0][0] + unit + 1, repeats[0][1] + 1)
        if literal <= repeat:
            cost, packets = literal
            choices.append(end - literals[0][2])
        else:
            cost, packets = repeat
            choices.append(-(end - repeats[0][2]))

    output = bytearray(9 + cost)
    output[:9] = b"APK1" + bytes([unit]) + struct.pack("<I", len(data))
    position, end = len(output), count
    while end:
        length = choices[end]
        start = end - abs(length)
        if length < 0:
            position -= 1 + unit
            output[position] = 128 + (-length) - 1
            output[position + 1:position + 1 + unit] = data[start * unit:(start + 1) * unit]
        else:
            position -= 1 + length * unit
            output[position] = length - 1
            output[position + 1:position + 1 + length * unit] = data[start * unit:end * unit]
        end = start
    assert position == 9
    return bytes(output)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--controls-only", action="store_true", help="Leave existing shell/dragon packs untouched")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1] / "main"
    resources = [
        ("ui_shell.rgb565", "ui_shell.pack", 2, 1280 * 720 * 2),
        ("aurora_controls.bin", "aurora_controls.pack", 2, 1088192),
        ("aurora_dragon.bin", "aurora_dragon.pack", 3, 160 * 132 * 125 * 3),
    ]
    report = []
    for source, dest, unit, expected in resources:
        data = (root / source).read_bytes()
        if len(data) != expected:
            raise ValueError(f"Incomplete resource {source}: {len(data)} instead of {expected}")
        if args.controls_only and source != "aurora_controls.bin":
            output = (root / dest).read_bytes()
        else:
            output = pack_zlib(data) if source == "aurora_controls.bin" else pack(data, unit)
            (root / dest).write_bytes(output)
        report.append(dict(source=source, packed=dest, raw_bytes=len(data), packed_bytes=len(output),
                           format=output[:4].decode("ascii"),
                           raw_sha256=hashlib.sha256(data).hexdigest(),
                           packed_sha256=hashlib.sha256(output).hexdigest()))
        print(source, len(data), "->", len(output), flush=True)
    (root.parent / "tools/aurora-reference/packed-assets.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
