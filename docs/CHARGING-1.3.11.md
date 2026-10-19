# Charging indicator stability — Aurora 1.3.11

The lightning bolt still means measured current entering the battery. It does not mean that a USB cable is attached. A full battery with approximately zero current, including a reading such as −1 mA, correctly shows no bolt. CHG_STAT and battery voltage cannot override measured discharge.

The former filter switched OFF on a single reading at or below +5 mA. A brief load pulse or near-zero reading could therefore make the icon blink, even when subsequent readings returned to charging. The revised filter retains current hysteresis and adds asymmetric time confirmation:

| Condition | Decision |
| --- | --- |
| OFF, current at least +20 mA | Confirm for 500 ms before switching ON. A lower current restarts confirmation. |
| ON, current above +5 mA | Keep ON and cancel pending OFF confirmation. |
| ON, current at or below +5 mA | Switch OFF after 750 ms of consecutive low-current observations. |
| ON, current at or below −20 mA | Switch OFF faster, after 250 ms of consecutive clear-discharge observations. A single brief negative sample does not blink the icon. |
| Voltage/current read error, unavailable sensor or absent battery | Immediately clear charging and report it as unknown. No stale true state or pin-based fallback. |
| More than 750 ms between valid samples, or clock reversal | Discard previous confirmation and evaluate the new sample from OFF. A sampling gap cannot count as evidence of continuous charging. |

At the existing 250 ms polling cadence, ordinary charging takes three observations to confirm; a sustained −202 mA discharge takes two observations to turn OFF. Including the existing panel refresh interval, normal visible unplug response is designed to remain within approximately one second. This is a timing bound from the polling design, not a new physical measurement or an instantaneous cable detector. Near-zero charge completion deliberately takes longer than clear discharge.

The change uses a few integer timestamps. It adds no timers, tasks, allocations, delays, retries or I²C transactions: one voltage and one current read remain sufficient for a normal poll. The displayed current itself is not averaged or altered. Charger enable, charging current, INA226 configuration, UI artwork and audio processing are unchanged. Read errors can still hide the icon immediately; concealing a sensor fault by retaining a charging claim is not part of the debounce.

## Verification

`tests/charging_status_test.py` extracts the production state and reader/helpers from `components/tab5_hal/tab5_hal.c`; controlled register/time stubs then execute that exact code. The updated suite passes 985 current/time samples plus raw-register and fault checks. It includes a three-minute sequence of isolated zero/negative load pulses without flicker, sustained unplug at −202 mA, exact current/time boundaries, interrupted confirmation, repeated timestamps, long polling gaps, clock reversal, high uptime, full-battery noise, signed raw extrema, absent battery and I²C failures during both pending and confirmed charging.

The test checks that CHG_STAT is never read and that valid polls do not add register I/O. The host result demonstrates the specified filter behavior; physical current noise and visible timing on the updated BIN require a separate device check. The 1.3.8 diagnosis and original cable-disconnect verification are historical and not part of this repository.

The unchanged INA226 conversion uses the Tab5's 5 mΩ shunt and opposite sign for current entering the pack. TI documents 2.5 µV shunt resolution and a maximum 10 µV input offset; the software thresholds keep a margin above that offset, while the new time confirmation handles brief load changes. [TI INA226 datasheet](https://www.ti.com/lit/ds/symlink/ina226.pdf)
