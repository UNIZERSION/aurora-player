"""Regenerate original 440 Hz test tones; requires the optional lameenc wheel."""
from pathlib import Path
import math
import struct
import wave
import lameenc

out = Path(__file__).resolve().parent / "fixtures"
out.mkdir(exist_ok=True)
rate = 44100
pcm = bytearray()
for i in range(rate * 2):
    value = round(16000 * math.sin(2 * math.pi * 440 * i / rate))
    pcm.extend(struct.pack("<hh", value, value))
encoder = lameenc.Encoder()
encoder.set_bit_rate(128)
encoder.set_in_sample_rate(rate)
encoder.set_channels(2)
encoder.set_quality(2)
(out / "sine-440hz.mp3").write_bytes(encoder.encode(bytes(pcm)) + encoder.flush())
with wave.open(str(out / "sine-440hz-mono24.wav"), "wb") as f:
    f.setnchannels(1)
    f.setsampwidth(3)
    f.setframerate(48000)
    raw = bytearray()
    for i in range(96000):
        value = round(4000000 * math.sin(2 * math.pi * 440 * i / 48000))
        raw.extend((value & 0xffffff).to_bytes(3, "little"))
    f.writeframes(raw)
print("Generated original two-second test tones.")
