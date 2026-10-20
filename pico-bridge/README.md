# Pico Bluetooth bridge

The Tab5's own radio (an ESP32-C6) has no Classic Bluetooth, so it cannot send music to
ordinary Bluetooth headphones (A2DP). This folder holds the firmware for a small helper board
that does it: an **M5Stack M5Stamp Pico** (ESP32-PICO-D4) wired to the Tab5. Aurora sends the
decoded music to the Pico over I2S and controls it over a UART; the Pico streams it to your
headphones or speaker and sends their Play/Pause/Next/Previous buttons back to Aurora.

- Firmware version: **0.9.5** (the version installed and used on the author's Tab5).
- Works with Aurora Player **1.3.14 or newer** (tested with 1.3.15; the bridge code in Aurora is
  unchanged up to 1.3.21).
- Audio: stereo, 16-bit, 44.1 kHz from the Tab5, sent to the headphones as **SBC** (no AAC,
  aptX or LDAC). Output only: no microphone or calls.
- Remembers up to 8 headphones or speakers and reconnects to the selected one by itself.
- Optional AirPods controls: battery, ear detection and listening mode
  (see [AirPods](#airpods)).

## Contents

1. [What you need](#what-you-need)
2. [Wiring](#wiring)
3. [Flashing the Pico](#flashing-the-pico)
4. [Using it with Aurora](#using-it-with-aurora)
5. [AirPods](#airpods)
6. [Troubleshooting](#troubleshooting)
7. [USB console and UART protocol](#usb-console-and-uart-protocol)
8. [Building from source](#building-from-source)
9. [Host tests](#host-tests)
10. [Folder layout](#folder-layout)
11. [Status, limits and license](#status-limits-and-license)

## What you need

- An M5Stack Tab5 running Aurora Player 1.3.14 or newer.
- An **M5Stack M5Stamp Pico** (ESP32-PICO-D4, 4 MB flash).
- For flashing: the **M5Stack ESP32 Downloader Kit (A105)**, a USB-serial adapter that plugs
  onto the Pico's 6-pin programming pads, or another 3.3 V USB-serial adapter with EN and IO0
  lines.
- Seven wires between the Tab5 and the Pico (soldering is needed).
- Bluetooth headphones or a speaker that supports A2DP with SBC (practically all do).
- A PC with Python 3 and esptool: `python -m pip install esptool==4.12.0`.
- The firmware files from the
  [latest Aurora release](https://github.com/UNIZERSION/aurora-player/releases/latest):
  `Pico-Bluetooth-0.9.5-Standalone.bin` and `Pico-Bluetooth-0.9.5-Application.bin`.

## Wiring

Seven wires, Tab5 GPIO numbers on the left. The arrows show the direction of the signal.

| # | Tab5 | Tab5 connector | | M5Stamp Pico | Signal |
|---|---|---|---|---|---|
| 1 | 5V | M5-Bus pin 28 | → | 5V pad | power for the Pico |
| 2 | GND | M5-Bus pin 1, 3 or 5 | — | GND | common ground |
| 3 | G5 | M5-Bus pin 11 | → | G26 | I2S bit clock (BCK) |
| 4 | G18 | M5-Bus pin 7 | → | G25 | I2S word select (WS / LRCK) |
| 5 | G19 | M5-Bus pin 9 | → | G22 | I2S audio data |
| 6 | G6 | M5-Bus pin 16 | → | G32 | UART commands, also wakes the Pico |
| 7 | G46 | not on the M5-Bus (see below) | ← | G33 | UART replies from the Pico |

- The M5-Bus pin numbers come from M5Stack's
  [Tab5 pinout](https://docs.m5stack.com/en/core/Tab5) (rear M5-Bus connector). **G46 is not on
  the M5-Bus**; M5Stack's pin list places it on the Tab5's internal STAMP expansion pads. Check
  the Tab5 schematic for the exact pad before soldering.
- The Tab5 is the I2S master: it drives the clock, word select and data lines. The Pico only
  listens on G26, G25 and G22.
- Wire 6 does two jobs: it carries Aurora's commands and, while the Pico sleeps, a 120 ms low
  pulse on it wakes the Pico up. It must go to the Pico's **G32**.
- The wires connect the two boards directly, with no level shifter; this is how the author's
  unit works.

**Safety**

- Connect the Tab5 5V wire **only** to the Pico's 5V pad, never to its 3V3 pad or to a signal
  pin. The ESP32 runs at 3.3 V and 5 V on a 3V3 or GPIO pin can destroy it.
- Never power the Pico from two sources. While the downloader is attached, disconnect the Pico
  from the Tab5 (at least the 5V wire, better all seven).
- Do not swap the two UART wires: Tab5 G6 goes to Pico G32, Pico G33 goes to Tab5 G46.
- Check the pads with a multimeter before soldering anything permanently.

## Flashing the Pico

1. Disconnect the Pico from the Tab5. Plug the ESP32 Downloader Kit onto the Pico's 6-pin
   programming pads. From top to bottom the pads are 3V3, TX0, RX0, EN, G0, GND; they meet the
   downloader as 3V3→3V3, RXD→TX0, TXD→RX0, EN→EN, BOOT→G0, GND→GND. Make sure the header is
   not shifted by one pin. Do not use the Pico's 5V pad while flashing.
2. Connect the downloader by USB. It shows up as a serial port (`COM5` in the commands below:
   use yours; `/dev/ttyUSB0` on Linux, `/dev/cu.usbserial-…` on macOS).
3. Check the connection. esptool resets the Pico into download mode by itself through EN and
   G0:

   ```
   python -m esptool --chip esp32 --port COM5 flash_id
   ```

4. Write the firmware.

   **First install** on a new Pico (bootloader, partition table and app; it also clears any
   saved headphones and settings):

   ```
   python -m esptool --chip esp32 --port COM5 --baud 460800 write_flash 0x0 Pico-Bluetooth-0.9.5-Standalone.bin
   ```

   **Update** a Pico that already runs this firmware (app only; keeps the saved headphones and
   the volume):

   ```
   python -m esptool --chip esp32 --port COM5 --baud 460800 write_flash 0x10000 Pico-Bluetooth-0.9.5-Application.bin
   ```

   If the transfer is unstable, use `--baud 115200`. Do not run `erase_flash`: it deletes the
   saved headphones, and neither image needs it.
5. Unplug the downloader and reconnect the seven wires to the Tab5.

Check a download against `SHA256SUMS.txt` from the release (`sha256sum -c SHA256SUMS.txt`, or
`Get-FileHash <file>` in PowerShell).

The Standalone image writes this partition table: `nvs` 0x9000 (20 KB, saved headphones and
volume), `otadata` 0xE000, `app0` 0x10000 (3 MB), `spiffs` 0x310000. An Application update keeps
the table that is already on the Pico. The update path (0.9.4 to 0.9.5, saved devices kept) is
the one tested on hardware; the Standalone image has the same app but has not been written to a
blank Pico yet.

## Using it with Aurora

1. Open the Bluetooth window: tap the **Bluetooth button in the top bar** (between the clock and
   the battery), or press **Ctrl + B** on the keyboard. The window is titled
   *PICO BLUETOOTH AUDIO*.
2. Tap **BT ON** (Bluetooth is off on a new install). Aurora wakes the Pico; the status line
   shows the state (*READY*, *CONNECTED*, *ASLEEP / NO LINK* or *BLUETOOTH OFF*), the output
   (*LOCAL OUTPUT ON* or *HEADPHONES ONLY*) and the Pico version.
3. Make sure no headphones are connected (switch the current ones off), put the new headphones
   in **pairing mode**, then tap **SCAN**. The Pico restarts and searches for 20 seconds:
   *Scanning for receivers (20 seconds)* shows briefly, then *READY | … Waiting for selected
   receiver* while the search runs, and *Scan finished; select a receiver* when it ends.
   If headphones are still connected when you tap SCAN, Aurora sees them drop, asks the Pico to
   reconnect, and that ends the scan after a second or two.
4. Tap your headphones in the list. Rows show the name (or the address), `[new]` or `[saved]`,
   and `> ` marks the selected device. The Pico pairs, saves the device and restarts to connect:
   *Receiver selected; connecting*, then *Connected to selected receiver* and
   *CONNECTED | HEADPHONES ONLY*.
5. Press **Play**. While headphones are connected, music goes only to them and the speaker and
   headphone jack are muted (*Settings > Audio* shows *Output: Bluetooth*). If you press Play
   before they connect, music starts on the speaker and moves to the headphones as soon as they
   connect.

Everyday use:

- **Volume:** the main volume knob, or Ctrl + ↑ / Ctrl + ↓. Bluetooth has its own saved level
  (default 60), separate from the speaker level. Volume buttons on the headphones do not change
  Aurora's level.
- **Headphone buttons:** Play, Pause, Next and Previous work like the on-screen controls, also
  while the PIN lock screen is shown.
- **Switch headphones:** open the Bluetooth window and tap another `[saved]` row. Saved devices
  appear even when they are off; pairing mode and SCAN are needed only for new ones.
- **RECONNECT** retries the selected headphones, for example after you switch them on.
- After a **SCAN**, pick a device or tap RECONNECT: during and after a scan the Pico does not
  reconnect by itself. Any later Pico restart (sleep and wake, BT OFF and ON) ends this state and
  automatic reconnect resumes.
- **Pause** keeps the headphones connected, so their Play button can resume. With no headphones
  connected, the Pico goes to sleep after about 2 minutes (*Pico sleeping; PLAY wakes it*);
  pressing Play or opening the Bluetooth window wakes it.
- If the headphones **disconnect**, Aurora pauses, switches to the speaker and asks the Pico to
  reconnect. Press Play when *Bluetooth connected. Audio switched to headphones.* appears.
- **BT OFF** puts the Pico to sleep and switches the output back to the speaker. If music was
  playing on the headphones, Aurora pauses it (*Headphones disconnected. Playback paused; Play
  uses the speaker.*); press Play to continue on the speaker. Powering off the Tab5 also puts the
  Pico to sleep first.
- Long-press the cover art to see *File / Output Information*; with Bluetooth it shows
  *Bluetooth: stereo PCM16 / 44.1 kHz*.

There is no "forget device" button. The Pico keeps up to 8 devices; to clear them all, flash the
Standalone image.

## AirPods

With Pico 0.9.5, once the Pico has answered, the Bluetooth window has an **AIRPODS** button. It opens a page with the
battery of the left and right earbud and the case, the listening mode (Off, Noise cancellation,
Transparency, Adaptive), **Auto pause** and **Connect**.

- Pair the AirPods like any headphones first (SCAN, then tap them).
- The control channel opens by itself about 2 s after the audio connection if the saved name
  contains "AirPods" (up to 3 tries, 30 s apart). Otherwise tap **Connect**.
- A listening mode is highlighted only after the AirPods confirm it. If they do not answer
  within 2 s, only the control channel closes; the music keeps playing.
- **Auto pause** (off by default, saved on the Pico): taking an earbud out pauses the music;
  putting both back resumes it, but only if that pause was automatic.
- Music, Play/Pause and the AirPods' own gestures are confirmed on hardware. Switching listening
  modes, long sessions and range are not confirmed yet, and some models or firmware versions may
  reject some modes.

The page and its rules are described in [docs/AIRPODS_CONTROLS.md](../docs/AIRPODS_CONTROLS.md).
The control protocol is implemented independently from the
[LibrePods](https://github.com/librepods-org/librepods) documentation. AirPods is a trademark of
Apple Inc.; this project is not affiliated with Apple.

## Troubleshooting

| What you see | Likely cause | What to do |
|---|---|---|
| *ASLEEP / NO LINK*, *Waking Pico* keeps coming back, version *Pico --* | The Pico never answers | Check 5V and GND, Tab5 G6 → Pico G32 and Pico G33 → Tab5 G46. Make sure the Pico is flashed. |
| *ASLEEP / NO LINK* with *Pico sleeping; PLAY wakes it* | Normal sleep: nothing played and no headphones were connected for 2 minutes | Press Play or open the Bluetooth window. |
| *READY … Waiting for selected receiver*, headphones never connect | The selected headphones are off, in their case, out of range or connected to a phone (during a SCAN this message is normal) | Switch them on, open the case or disconnect them from the phone; tap RECONNECT. |
| New headphones do not appear after SCAN | They were not discoverable during the 20 s scan, or other headphones were still connected, which ends the scan early | Switch the connected headphones off, put the new ones in pairing mode, then SCAN again. |
| *Saved-device list is full (8)* | The Pico keeps at most 8 devices and cannot forget one | Use a saved device, or flash the Standalone image to clear all. |
| *Audio output / I2S failed* about 2 s after Play | Pico 0.9.4 (it reports the audio state Aurora does not expect) | Update the Pico to 0.9.5 with the Application image; the saved devices stay. |
| *Pico I2S input error; check wiring* | The Pico's I2S input failed | Check G5 → G26, G18 → G25 and G19 → G22, then restart. |
| *Bluetooth stream stalled. Playback paused; press Play to retry.* | The Pico stopped receiving audio and Aurora's restarts did not help | Press Play; if it repeats, check the three I2S wires. |
| *Bluetooth transport stalled. Press Play to retry.* | The Bluetooth link stopped taking audio and 3 recoveries within a minute failed | Press Play; if it repeats, tap RECONNECT. |
| Music plays from the speaker although Bluetooth is on | The headphones are not connected yet | Connect them (select them or RECONNECT); the output switches by itself. |
| *volume awaiting ACK* after the status | Normal while Bluetooth is off or the Pico sleeps: Aurora sends the volume the next time the link is up | Nothing to do; it clears shortly after the Pico wakes. If *Waking Pico* keeps coming back, see the first row. |
| No AIRPODS button | Bluetooth is off, the Pico has not answered since the Tab5 started, or its firmware has no AirPods support (0.9.3 or older) | Tap BT ON and wait until the status shows a Pico version instead of *Pico --*. If a version is shown and the button is still missing, update the Pico to 0.9.5. |
| A saved device is called *Sparowane XX:XX:…* or *Zapisane urzadzenie* | Placeholder names (Polish for "paired" / "saved device") for devices imported from older versions | Run SCAN while the device is discoverable: the real name replaces the placeholder. |
| *AirPods controls are not connected. Tap Connect to try again.* | The control channel is not open: the name has no "AirPods" and Connect was never tapped, the 3 automatic tries are used up, or the channel closed | Tap Connect. |
| *No confirmation. Try Connect again.* | The headphones did not open the control channel within 15 s of Connect | Tap Connect again; if it keeps failing, reconnect the headphones in the Bluetooth window. |
| *AirPods control channel is not ready. Try Connect again.* | The channel closed just as a mode or auto-pause change was sent | Tap Connect, then choose the setting again. |
| The Pico restarts in a loop right after an Application update | Its saved-settings area (NVS) could not be opened; the firmware never erases it by itself | Flash the Standalone image (this clears the saved devices). |

## USB console and UART protocol

The Pico also accepts commands on its USB console: connect the downloader and open the port at
115200 baud (8N1). Commands are text lines ending with a newline. After a reset you have 15 s;
send `USE` at least every 15 s or the Pico goes back to deep sleep (serial text cannot wake it,
use its reset). `HELP` prints the command list and the banner
`TAB5 PICO BT BRIDGE 0.9.5 | ESP32-PICO-D4`.

Aurora uses the same commands on the UART (Tab5 G6 / G46, 115200 8N1):

| Command | Reply | Meaning |
|---|---|---|
| `USE` (or `WAKE`) | none | Keep awake for 15 s and start Bluetooth and I2S. Aurora sends it every 3 s. |
| `SLEEP` | `STANDBY` | Save the volume and go to deep sleep (woken only by a pulse on G32). |
| `LIST` | `DEVICE <n> <MAC> <rssi> <saved> <selected> <name>` … `END_LIST` | Saved devices first, then the devices found by the last scan. |
| `SELECT <n>` / `SELECTMAC <MAC>` | `SELECTED <name>; restarting to connect` | Select a device (a new one is saved, up to 8) and connect to it. |
| `SCAN` | `RESTART scanning; saved target preserved`, later `SCAN_DONE` | Restart into a 20 s discovery; no automatic reconnect until `SELECT`, `RECONNECT` or the next restart. |
| `RECONNECT` | `RESTART reconnecting to saved device` | Connect to the selected device again. |
| `STATUS` | `STATUS …`, then `AIRPODS …` and `CODEC …` lines | Full state and counters (connection, audio, volume, stream counters, version). |
| `VOLUME <0-100>` | `VOLUME <n>` | Digital volume on the Pico (quadratic curve, saved 1 s after the last change). |
| `STREAM <token>` | `STREAM_ACK <token>` | Start forwarding the I2S audio (Aurora sends a new token per start). |
| `STOP` | `SILENT` | Pause: mute, clear the buffer and suspend the stream; the headphones stay connected. |
| `TEST` | `TEST START: LEFT / silence / RIGHT` | 3 s test tone (left, silence, right) on connected headphones. |
| `AIRPODS CONNECT` / `DISCONNECT` / `STATUS` / `MODE <1-4>` / `AUTO_PAUSE <0\|1>` | `AIRPODS …`, `AIRPODS_PENDING …` or `AIRPODS_ERROR …` | AirPods control channel (modes: 1 Off, 2 noise cancellation, 3 Transparency, 4 Adaptive). |

Messages the Pico sends on its own:

- `READY version=0.9.5 cap_airpods=1` at every start.
- `STATE connection=<n> audio=<2|0>` when the connection or the stream changes
  (`connection=2` is connected, `audio=2` is streaming).
- `REMOTE PLAY|PAUSE|NEXT|PREVIOUS` for headphone buttons.
- `STREAM_FAULT <token> INPUT_STALLED` (no audio from the Tab5 for 1.5 s),
  `RECOVERY <token> TRANSPORT_STALLED` (the Pico is restarting the Bluetooth stream without
  disconnecting) and `STREAM_FAULT <token> TRANSPORT_FAILED` (3 recoveries within 60 s failed).
- `ERROR <text>` for a rejected command; Aurora shows the text in the Bluetooth window.

Lines are ASCII, at most 80 characters for commands; longer or non-printable input is rejected
with `ERROR Invalid or oversized command`.

## Building from source

The firmware is a native ESP-IDF project for the classic ESP32.

1. Install [ESP-IDF 5.5.2](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32/get-started/)
   with the `esp32` target (`install.ps1 esp32` / `install.sh esp32`) and open an ESP-IDF
   terminal (`export.ps1` / `. export.sh`). Use exactly 5.5.2: `sdkconfig.defaults` relies on its
   defaults.
2. From the repository root:

   ```
   idf.py -C pico-bridge/native-idf build
   ```

   On Windows, keep the repository in a short path (for example `C:\src\aurora-player`): the
   ESP-IDF build fails when its object paths exceed Windows' 260-character limit.
3. Flash with the downloader attached: `idf.py -C pico-bridge/native-idf -p COM5 app-flash`
   writes only the app (like the Application image), `flash` also writes the bootloader,
   partition table and otadata (it does not touch the saved devices).

The build writes, in `pico-bridge/native-idf/build/`: `pico_bt.bin` (app, 0x10000),
`bootloader/bootloader.bin` (0x1000), `partition_table/partition-table.bin` (0x8000) and
`ota_data_initial.bin` (0xE000). A Standalone image is these four merged (run in
`pico-bridge/native-idf`):

```
python -m esptool --chip esp32 merge_bin -o Pico-Bluetooth-Standalone.bin --flash_mode dio --flash_freq 40m --flash_size 4MB 0x1000 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0xe000 build/ota_data_initial.bin 0x10000 build/pico_bt.bin
```

A build of this folder has the same code as the released 0.9.5 images (every function and
variable has the same size), but it is not byte-identical: the images embed the build time and
two source-file names. The released images are the tested 0.9.5 build in which those two
absolute source paths were replaced by the relative names a build of this folder produces.

The same `src/main.cpp` reports version 0.9.5 with AirPods support only in this native build
(`PICO_NATIVE_IDF`); the app descriptor's version string is `0.9.5-airpods-candidate`.

## Host tests

The tests compile the real `src/main.cpp`, the modified ESP32-A2DP methods and the AirPods code
against mocks of the ESP-IDF and Bluetooth APIs, and run them on the PC. They need
[Zig 0.13.0](https://ziglang.org/download/) (as the C++ compiler) and Python 3, both on `PATH`
or given with `-ZigPath` / `-PythonPath`:

```
powershell -NoProfile -ExecutionPolicy Bypass -File pico-bridge\tests\run_all_tests.ps1
```

They use Aurora's protocol headers from this repository (`main/bluetooth_protocol.hpp` and
friends), so both sides of the UART protocol are tested together. Covered: stream start and
pause handshakes, buffer and session isolation, volume, headphone buttons, the A2DP media state
machine, stream health and recovery (including 12 hours of simulated audio), and the AirPods
parser and worker. Not covered: the radio, real headphones and timing on the hardware.
`tests/fixtures/` holds two older versions of `main.cpp` that reproduce fixed bugs when a test
is built with `-DBASELINE` or `-DAUDIO_WIRE_BASELINE`.

## Folder layout

```
pico-bridge/
├── src/               firmware logic: main.cpp and small headers, shared with the host tests
├── native-idf/        ESP-IDF 5.5.2 project (target esp32): CMake files, partitions.csv,
│   │                  sdkconfig.defaults
│   └── main/          app_main glue, Arduino-style UART/NVS shims, AirPods control worker
├── lib/ESP32-A2DP/    ESP32-A2DP 1.8.11 (Apache-2.0), partly modified, plus one added
│                      GPL-3.0-only file: see MODIFICATIONS.md
├── tests/             host tests, SDK mocks and fixtures
├── README.md          this guide
└── CHANGELOG.md       firmware history 0.1 – 0.9.5
```

## Status, limits and license

- Version 0.9.5 is used daily on the author's Tab5 with Aurora; it was released as
  "experimental" because of the new AirPods controls.
- SBC only. No forget command; at most 8 saved devices.
- Two placeholder device names in the firmware are Polish (*Sparowane*, *Zapisane urzadzenie*).
- The Pico's current draw was not measured.

The bridge firmware is part of Aurora Player and is licensed under the GNU General Public
License v3.0 only, with the additional permission for Espressif libraries described in the
top-level [NOTICE.md](../NOTICE.md). `lib/ESP32-A2DP` is © Phil Schatzmann and contributors
(with code from Espressif's examples) under the Apache License 2.0, except
`src/SourceMediaState.h`, which was added for this bridge and is GPL-3.0-only like the rest of
this project; see
[lib/ESP32-A2DP/LICENSE](lib/ESP32-A2DP/LICENSE) and
[lib/ESP32-A2DP/MODIFICATIONS.md](lib/ESP32-A2DP/MODIFICATIONS.md). The release images also
contain ESP-IDF 5.5.2 and its components (Apache-2.0 and others; notices under
[licenses/](../licenses)).
