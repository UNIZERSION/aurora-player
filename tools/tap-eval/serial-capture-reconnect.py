"""Capture Tab5 serial output, surviving USB unplug/replug.

usage: python serial-capture-reconnect.py PORT SECONDS OUTFILE
Appends to OUTFILE. When the port disappears (cable unplugged for a pocket
trial), it keeps retrying and continues in the same file. Opening never
toggles DTR/RTS, so the chip is not reset.
"""
import sys
import time

import serial


def main():
    port, seconds, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
    end = time.time() + seconds
    with open(out, 'ab') as f:
        while time.time() < end:
            ser = serial.Serial()
            ser.port, ser.baudrate, ser.timeout = port, 115200, 0.1
            ser.dtr = ser.rts = False
            try:
                ser.open()
            except serial.SerialException:
                time.sleep(0.5)
                continue
            try:
                with ser:
                    last = time.time()
                    while time.time() < end:
                        data = ser.read(4096)
                        if data:
                            f.write(data)
                            f.flush()
                            last = time.time()
                        elif time.time() - last > 3:
                            # The calibration build logs continuously; on
                            # Windows a removed device can leave a silent
                            # handle instead of raising, so reopen.
                            break
            except (serial.SerialException, OSError):
                pass
            time.sleep(0.5)


if __name__ == '__main__':
    main()
