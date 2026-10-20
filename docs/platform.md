# Platform and reproducible builds

The project targets the M5Stack Tab5 (ESP32-P4, 16 MB flash, 32 MB PSRAM). The main dependencies are pinned in `main/idf_component.yml`: ESP-IDF 5.5.2, the official Espressif M5Stack Tab5 BSP 1.3.2, ESP LVGL Port 2.8.0 and LVGL 9.3.0. The full dependency resolution result is recorded in `dependencies.lock`. Port 2.8.0 is pinned on purpose: 2.9.0 uses a new DPI callback that the ESP-IDF 5.5.2 headers do not provide yet.

The CPU runs at 360 MHz, the ESP-IDF 5.5.2 default for the ESP32-P4 family before rev. 3. Version 1.0.0 forced 400 MHz; a log from a Tab5 rev. 1.3 confirmed a halt in `esp_clk_init` on a frequency conversion assertion, before the application was even entered. Version 1.0.1 removes that override. PSRAM stays at 200 MHz, matching the official BSP configuration.

## Display and audio

1.3.5 turns the backlight down to zero and stops the LVGL render timer. Touch is still read every 16 ms; the controller and the hardware DPI scan stay active to keep the wake-up path reliable. Animations, FFT and new artwork jobs are paused. Confirmation on the device is still required.

The panel has a native 720 × 1280 layout. Version 1.3.5 keeps three full DPI buffers in PSRAM and two partial LVGL buffers of 1280 × 480 RGB565. The PPA rotates the changed areas into a free buffer. The last chunk publishes the whole frame. Two VSYNCs after its successor is published protect an old buffer from being reused; the third buffer allows work to continue before the previous one is released. A separate dirty-area list per buffer keeps the image consistent. Touch keeps the 90° rotation and the 1280 × 720 coordinate space. This is a custom presentation path; the BSP's standard avoid-tear mode disables the rotation needed here.

A 150 ms synchronisation timeout stops publishing instead of allowing a potentially visible buffer to be overwritten. The `PRESENT` log reports the number of frames, copied pixels, VSYNC signals and the minimum free stack. A host test exercises the real flush function against a PPA/VSYNC model; on the author's device the 1.1.0 display was confirmed to work clearly better. This is not an oscilloscope measurement of panel timing, nor a guarantee of equal draw times for all screens.

The LVGL task on core 0 has a 16 KiB stack. In the first-image trial of 1.1.0 an 8 KiB stack was too small: LVGL's JPEG recognition callback reserves about 4 KiB even while checking an image of a different format. The final build removes this fault, and the device log showed 11,484 B of stack headroom during the trial. Static RGB565 decorations take 311,752 B instead of a full embedded PNG; they need no decoding or scaling of a large image when displayed.

Initialisation writes BOOT stages to the USB log and checks the result of setting the backlight. If the BSP created the LCD but returned a touch initialisation error, the HAL keeps the display so that the application can show a diagnostic message.

The ES8388 codec receives 16-bit stereo PCM over I2S1 at the source sample rate. The HAL uses the official esp_codec_dev, switches the format between files, and handles hardware volume and mute. 24/32-bit WAV is converted by the audio engine to 16-bit output; the source parameter shown in the UI is not a promise of a 24-bit output path.

Since version 1.0.4 streaming uses the file descriptor directly (`read`/`lseek`). The C library that was actually linked performed `fread` with `_IONBF` one byte per VFS call, despite the large buffer requested by the application. The metadata parser stays on buffered stdio; handing the stream over to the descriptor is synchronised before playback.

An aligned 16 KiB buffer in internal DMA memory collects whole-sector reads. The prefix up to the sector boundary and the short tail are read separately, so that FatFs does not shift the address of the large transfer off the required alignment. This avoids the SDMMC driver falling back to one transaction per sector. The data then goes into the audio buffers in PSRAM. The current P4 profile uses a 64 B cache line; the path has separate handling for `read` errors, short reads and EINTR interruptions.

## exFAT

ESP-IDF 5.5.2 has a hard-coded `FF_FS_EXFAT=0` in `ffconf.h`. An sdkconfig entry alone does not enable exFAT. [`cmake/exfat.cmake`](../cmake/exfat.cmake) copies the FatFS component from the SDK into the build directory and sets `FF_FS_EXFAT=1` in that copy. It then registers the copy as a component that overrides the built-in FatFS. All components receive the same public headers and ABI. The original SDK stays unmodified. Static assertions in the HAL require exFAT, LFN and a 64-bit `FSIZE_t` to be active. `CONFIG_FATFS_USE_LABEL=y` is also enabled, as required by the exFAT build path in this version of Espressif's FatFS configuration.

The card is mounted in 4-bit SDMMC mode at 20 MHz. This frequency leaves a large margin for audio without needlessly forcing the card's maximum clock. Format-on-failure is disabled both in the configuration and in the code. The HAL has no format operation. exFAT and FAT32 are accepted; the type of the file system actually mounted is returned to the application.

Since version 1.0.2 the capacity reading uses the mounted volume's metadata. It does not call `f_getfree`, which, with an unknown free-cluster count, would scan the entire exFAT allocation bitmap. Free space is reported only when FatFs already holds a valid count in memory; otherwise the UI marks the value as unknown. A test on real FatFs with a RAM disk confirmed that reading the root directory metadata performs no sector reads. The HAL log contains the time of display initialisation, SD mounting and metadata retrieval. These times should not be confused with the track scan time.

The global `CONFIG_FATFS_USE_FASTSEEK` is disabled. In ESP-IDF 5.5.2 this option builds a map of the whole cluster chain each time a file is opened for reading, including short tag reads. For many tracks this would mean needless reads of entire chains while scanning the library. Normal seeking stays available; a long seek in a fragmented file may need to walk its cluster chain only at the moment of seeking.

Version 1.0.3 builds the base catalogue from the names and information returned by enumeration. It does not open every track to read its headers: `fopen` searches the FatFs directory for the name again, which with thousands of files in a single folder leads to quadratic growth in the number of reads. Tags and more precise parameters are filled in from the decoder of the selected track and go into the cache. An unknown duration stays explicitly unknown.

On the tested Tab5 the 1.0.3 log confirmed mounting an exFAT card in 294 ms. A scan of 4037 tracks took 10.905 s, with 4036 hits in the existing cache and no tracks opened by the parser during the scan. This is not a measurement of a first scan without cache, nor a confirmation of smooth playback.

Version 1.1.0 stores an additional catalogue index tied to the music root directory and the card identifier. The HAL computes the identifier from the CID fields and the card geometry, without scanning sectors. A valid index and CRC let the library be restored without walking the directories again and checking every file. After files are added, removed, replaced or renamed externally, choose `SETTINGS > UPDATE LIBRARY`. A missing or invalid index triggers a normal scan. Parameters and tags of an unknown track are still filled in only once it is selected.

A 64-bit size in FatFS does not automatically mean a 64-bit `long`/`off_t` in the C library. The audio engine must reject files exceeding its supported seek range; an exFAT card can still have a capacity larger than 32 GB.

## Clock, battery, connectivity

The RX8130 RTC is read over I2C as local time; readings are validated, and an invalid clock returns an error. The INA226 at address 0x41 measures the 2S pack voltage and the current through a 5 mΩ shunt. The HAL first checks the manufacturer and die identifiers; the earlier address 0x40 was the ES7210's address. The charging signal comes from input 6 of the 0x44 expander, with the current measurement as a fallback source. The charger configuration is not changed.

The battery percentage is an approximation from voltage, following the approach used in M5Unified, not a coulomb counter measurement; the UI marks it with `~`. The voltage range check is not an independent sensor of the battery's physical presence. A Tab5 log confirmed identification of the INA226 at 0x41 and a reading of 8018 mV (estimated 88%); no full discharge cycle or percentage calibration has been carried out.

This project does not initialise ESP-Hosted or a network stack. Wi-Fi/Bluetooth are not faked as connected. The ESP32-C6 supports BLE but not Classic Bluetooth A2DP; playback on A2DP headphones requires an additional transmitter. Since 1.1.0 an optional bridge to an external ESP32-PICO-D4 is available: PCM over I2S0 and control over UART2. The local ES8388 stays on I2S1. The connection requires seven wires and companion firmware running on the ESP32-PICO-D4 (an M5Stamp Pico); that firmware, Pico Bluetooth 0.9.5, is in [pico-bridge/](../pico-bridge/README.md) together with the wiring and flashing guide. Real BT pairing and playback trials are a separate part of validation; a working local output does not confirm the A2DP output.

## Official sources checked during implementation

- BSP and API: https://github.com/espressif/esp-bsp/tree/master/bsp/m5stack_tab5
- Device specifications: https://docs.m5stack.com/en/core/Tab5
- ESP-IDF FatFS: https://github.com/espressif/esp-idf/tree/v5.5.2/components/fatfs
- PPA and LVGL: https://github.com/espressif/esp-bsp/tree/master/components/esp_lvgl_port
- RTC/voltage registers: https://github.com/m5stack/M5Unified/tree/master/src/utility
- DPI buffer publishing: https://github.com/espressif/esp-idf/blob/v5.5.2/components/esp_lcd/dsi/esp_lcd_panel_dpi.c

Building does not program the hardware. From an ESP-IDF terminal: `idf.py build`. Programming requires a separate command, `idf.py -p COMx flash monitor` or `tools/build.ps1 -Action flash -Port COMx`. On Windows a short path to the SDK and the project is recommended to avoid the tools' path length limit.

After a successful build, `python tools/package_firmware.py --build build` packages the ready binaries. The script rejects a binary older than the sources, creates a SHA-256 manifest and reads the flash addresses from the ESP-IDF output. `tools/flash_prebuilt.ps1 -Port COMx` verifies the file checksums and programs only the explicitly given port. It requires `python -m pip install esptool==4.12.0`. It does not use `--force` or `erase_flash`; esptool checks the chip and the required processor revision.

The default configuration is for the original ESP32-P4: minimum revision 0.1, maximum 1.99, matching the default configuration of the official Tab5 BSP. The ESP32-P4 rev. 3 variant requires a separate build with the corresponding revision family selected in menuconfig. The firmware manifest states the exact range from the final configuration. The merged file goes at address 0x0; the separate images go at 0x2000 (bootloader), 0x8000 (partition table) and 0x10000 (application). The script uses the separate images by default.
