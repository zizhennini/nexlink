# read_bootlog.py - reset the ESP32-S3 over USB-Serial/JTAG (COM11) and
# capture its boot log. Usage: python read_bootlog.py [PORT] [SECONDS]
import sys, time
import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM11"
wait = float(sys.argv[2]) if len(sys.argv) > 2 else 18.0

s = serial.Serial(port, 115200, timeout=0.5)
# hard reset: RTS pulses EN on the USB-Serial/JTAG interface
s.setDTR(False)
s.setRTS(True)
time.sleep(0.1)
s.setRTS(False)

t0 = time.time()
while time.time() - t0 < wait:
    n = s.in_waiting
    if n:
        sys.stdout.write(s.read(n).decode("utf-8", "replace"))
        sys.stdout.flush()
    else:
        time.sleep(0.1)
s.close()
