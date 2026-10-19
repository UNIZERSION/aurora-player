# M5Stack Tab5 Keyboard A164: hardware integration

The optional keyboard uses Ext.Port1: SDA GPIO0, SCL GPIO1, active-low interrupt GPIO50 and default address `0x6D`. Aurora reserves the other ESP32-P4 I2C controller (`1 - BSP_I2C_NUM`), so the existing internal GPIO31/32 touch, codec, battery and RTC bus is unchanged. The current build has BSP I2C1 and keyboard I2C0. Pico UART/I2S pins are separate.

## Protocol and compatibility

Aurora selects HID mode (`0x10 = 1`). Each FIFO entry read at `0x30` is exactly two bytes: modifier and HID usage. An ordinary key press has its usage; an ordinary key release has usage zero. Empty FIFO returns `FF FF`. This is the module's small I2C event format, not an eight-byte USB keyboard report. Aurora does not enable USB/BLE keyboard forwarding.

The STM32 firmware handles Aa (one-shot uppercase/caps/held case), Sym and Ctrl/Alt. Aurora translates the resulting US-layout HID usage/Shift combination into printable ASCII, or Enter/Escape/Backspace/Delete/Tab/arrows/Home/End/Page keys. Its normalized modifier bits are Ctrl=1, Shift=2 and Alt=4. Modifier-only keys do not produce ordinary key events. HID does not identify which key was released when several are held, so any release conservatively stops host repeat.

The official firmware's `FIFO_DEPTH` is 32 (`user_sys.h`). Count is read at `0x02`. A bounded drain handles at most 32 entries and rejects invalid counts. Reading a FIFO entry consumes it. Clearing status (`0x01 = 0`) releases the interrupt line. A periodic count read covers arrivals racing with that clear, even if no fresh interrupt edge is seen.

Connection setup only writes volatile configuration:

| Register | Write | Purpose |
| --- | --- | --- |
| `0x10` | `1` | HID mode |
| `0x00` | `2` | HID interrupt enable |
| `0x02` | `0` | Discard stale FIFO input |
| `0x01` | `0` | Clear interrupt status |

Firmware version (`0xFE`) and configured I2C address (`0xFF`) are read for identification. The driver never writes address `0xFF`, firmware-update/reset register `0xFD`, keyboard firmware, flash or LED settings. The product protocol PDF has incorrect version/address labels; the implementation follows the actual firmware and official libraries. The Character-mode length note is also inconsistent: current firmware returns total bytes including modifier at `0x40`; Aurora's HID mode avoids that ambiguity.

## Scheduling and recovery

- Independent priority-2 worker, 3072-byte stack and fixed 64-event FreeRTOS queue; no allocation per key/frame, no SD access and no LVGL/audio lock in the worker.
- IRQ wakes the worker; a 20 ms fallback checks the FIFO and repeat timer. A 1 ms yield bounds IRQ storms.
- I2C transaction timeout is 10 ms. An absent keyboard is optional and is probed once per 2 seconds. It may be attached after boot.
- I2C errors/unplug discard queued input and reset held/repeat state. Reconnection validates and reapplies the mode; no stale commands are replayed. Mode is checked once per second to recover after a module reset.
- Local queue overflow discards backlog and repeat state. Input resumes with subsequent physical presses. Invalid HID usages cancel repeat; `FF FF` denotes no new event.
- Printable and navigation/editing keys repeat after 450 ms, then every 60 ms. Enter, Escape, Tab and Ctrl/Alt shortcuts do not repeat. The UI additionally ignores inappropriate repeated playback shortcuts, including repeated Space outside text input.
- A delayed worker emits at most one repeat rather than catching up in a burst. A held state expires after 10 seconds to bound a lost release; releasing and pressing again resumes it. Ctrl+Up/Down therefore adjusts volume once per physical press.

## Verification

`tests/test_tab5_keyboard_protocol.c` tests letters/digits/all US punctuation, Aa/Sym-equivalent HID combinations, left/right modifiers, navigation, empty/release/unsupported reports, repeat delay/rate, no catch-up bursts, ten-second safeguard and millisecond wraparound.

`tests/test_tab5_keyboard_hal.c` compiles the real HAL with mocked I2C/RTOS: independent bus/pins, bounded timeouts, allowlisted volatile writes, queue consumption/overflow, failed reads, invalid count, worker absent/hotplug/unplug behavior and two-second probe spacing. These host tests passed; they do not substitute for physical keyboard testing.

## Primary sources

Reviewed 2026-10-13:

- [M5Stack A164 product/pin map](https://docs.m5stack.com/en/tab5/Tab5_Keyboard).
- [STM32 register map](https://github.com/m5stack/M5Tab5-Keyboard-Internal-FW/blob/bbfd579182563e63acd867333e6138f3e5e6e047/code/Keyboard_APP/Core/User/i2c/user_i2c_reg.h) and [read/write semantics](https://github.com/m5stack/M5Tab5-Keyboard-Internal-FW/blob/bbfd579182563e63acd867333e6138f3e5e6e047/code/Keyboard_APP/Core/User/i2c/user_i2c_callback.c).
- [STM32 Aa/Sym/HID and release logic](https://github.com/m5stack/M5Tab5-Keyboard-Internal-FW/blob/bbfd579182563e63acd867333e6138f3e5e6e047/code/Keyboard_APP/Core/User/keyboard/user_keyboard_handle.c) and [FIFO depth](https://github.com/m5stack/M5Tab5-Keyboard-Internal-FW/blob/bbfd579182563e63acd867333e6138f3e5e6e047/code/Keyboard_APP/Core/User/sys/user_sys.h).
- [Official ESP-IDF driver](https://github.com/m5stack/M5Tab5-Keyboard-UserDemo/blob/5ed3b7bf6d9eba2a549e23d7cf70dbf71bd83a8f/components/m5_tab5_keyboard_component/src/m5_tab5_keyboard.cpp).
- [Official UnitUnified driver and Character-length clarification](https://github.com/m5stack/M5Unit-KEYBOARD/blob/b58d024f52cc1b1fa4f189a857ef64468369409a/src/unit/unit_Tab5Keyboard.cpp).
