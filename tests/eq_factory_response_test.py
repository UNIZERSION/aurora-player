"""Independent RBJ response check of the shipped factory curves (not DSP timing)."""
import argparse
import cmath
import hashlib
import json
import math
from pathlib import Path
import re

p = argparse.ArgumentParser()
p.add_argument('--output', required=True, type=Path)
args = p.parse_args()
source = Path(__file__).resolve().parents[1] / 'main/eq_presets.h'
presets = re.findall(r'\{"([^"]+)",\s*\{([^}]+)\}\}', source.read_text())
centers = [31, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000, 20000]
assert len(presets) == 8
results = []
for name, values in presets:
    gains = [int(v) for v in values.split(',')]
    assert len(gains) == 11 and all(-5 <= v <= 5 for v in gains)
    peaks = []
    for rate in [8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 96000]:
        filters = []
        for center, gain in zip(centers, gains):
            if not gain or center >= .475 * rate:
                continue
            a = 10 ** (gain / 40)
            w = 2 * math.pi * center / rate
            alpha = math.sin(w) / (2 * math.sqrt(2))
            filters.append((1+alpha*a, -2*math.cos(w), 1-alpha*a,
                            1+alpha/a, -2*math.cos(w), 1-alpha/a))
        peak = 0
        for i in range(4097):
            z = cmath.exp(-1j * math.pi * i / 4096)
            response = 1
            for b0, b1, b2, a0, a1, a2 in filters:
                response *= (b0+b1*z+b2*z*z)/(a0+a1*z+a2*z*z)
            peak = max(peak, 20*math.log10(abs(response)))
        # Factory selection ensures at least 12 dB fixed headroom. This bound
        # applies to stationary sinusoids, not every transient or edited curve.
        assert peak < 12, (name, rate, peak)
        peaks.append({'rate': rate, 'peak_boost_db': round(peak, 5)})
    results.append({'name': name, 'rates': peaks, 'worst_boost_db': max(x['peak_boost_db'] for x in peaks)})
report = {'result': 'PASS', 'curves': results,
          'preset_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
          'scope': '8 factory curves, 9 rates, 4097 frequency points per curve/rate',
          'limitations': ['Double precision RBJ oracle; production DSP is tested separately.',
                          'Headroom bound is a frequency-response bound, not a guarantee against all waveform transients.']}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(report, indent=2)+'\n')
for r in results:
    print(r['name'], 'maximum boost', r['worst_boost_db'], 'dB; fixed preamp -12 dB')
