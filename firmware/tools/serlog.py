#!/usr/bin/env python3
"""Reset the board and print its serial log for N seconds: serlog.py [seconds] [port]"""
import sys, time, serial
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 8
s = serial.Serial(sys.argv[2] if len(sys.argv) > 2 else "/dev/ttyACM0", 115200, timeout=0.2)
if "--no-reset" not in sys.argv:
    s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False
end = time.time() + secs
while time.time() < end:
    d = s.read(4096)
    if d: sys.stdout.write(d.decode(errors="replace")); sys.stdout.flush()
