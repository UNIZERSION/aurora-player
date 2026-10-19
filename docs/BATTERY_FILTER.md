# Battery indication

The battery icon and Settings percentage use a **smoothed voltage estimate**,
not an exact fuel gauge. The INA226 measures pack voltage and shunt voltage;
it does not supply an accurate remaining-capacity percentage. The existing
M5Unified-style 2S voltage mapping is retained (6.6 V → 0%, 8.2 V → 100%). No
assumed battery capacity or invented load compensation is applied.

## Percentage

- Median of the latest five voltage samples rejects isolated load spikes.
- An 8 s low-pass and 1.25 percentage-point hysteresis suppress voltage ripple.
- A change must persist for 3 s. The display then moves at most one point per
  5 s downward, or one point per 10 s upward. Large real changes therefore take
  longer to settle. This intentional display smoothing is not used for power
  shutdown or battery protection.
- A newly detected pack initializes from its actual voltage. A polling stall
  does not reset the displayed level or count as observed confirmation time.
- `millivolts`, `milliamps` and `raw_estimated_percent` preserve the unfiltered
  diagnostics. `filtered_millivolts` records the low-pass result.

## Charging lightning symbol

Positive battery current uses the Tab5 shunt polarity (2.5 µV / 5 mΩ; the
sensor sign is inverted for current into the battery). The symbol means
**battery charging**, not merely USB attached.

- Turn on after at least +20 mA persists for 750 ms.
- Once on, current above +5 mA cancels pending turn-off.
- Turn off after current at or below −20 mA persists for 1 s.
- Near-zero current must persist for 3 s before turn-off.
- At the normal 250 ms polling cadence, sustained discharge after USB removal
  is detected within approximately 1.25 s plus controller/render latency.
- A short scheduling gap preserves the confirmed state. Gaps over 1 s restart
  transition confirmation; gaps over 2 s expire the previous charging state.
- One failed sensor read may reuse the last real sample for at most 1 s.
  `sample_age_ms` explicitly reports its age. After expiry the status is
  unavailable; an absent pack is reported immediately. Failed observations
  never count toward an unconfirmed charging transition.
- CHG_STAT is not used as a charging override: it remained high on the author's
  device while the measured battery current was −202 mA after unplugging.

A battery reaching full charge with near-zero current correctly has no bolt.
Intermittent physical charging lasting longer than these thresholds can still
change the icon; smoothing cannot substitute for unavailable cable telemetry.

## Validation

`tests/charging_status_test.py` compiles the actual production helper functions
and reader against simulated register reads and time. It covers pulse loads,
UI scheduling gaps, stale data, I2C failures, unplugging, signed register
extrema, missing battery, noisy voltage and sustained voltage changes. There
is no USB access or invented on-device validation in this test.

Primary references: [TI INA226 datasheet](https://www.ti.com/lit/ds/symlink/ina226.pdf)
(bus register 1.25 mV/LSB, shunt register 2.5 µV/LSB, conversion averaging), and
[M5Stack Tab5 hardware documentation](https://docs.m5stack.com/en/core/Tab5)
(INA226 monitoring, IP2326 charging, removable NP-F550 pack).
