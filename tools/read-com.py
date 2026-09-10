import serial
import sys

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
s = serial.Serial(port, 115200, timeout=1)
for _ in range(40):
    line = s.readline()
    if line:
        print(line.decode("utf-8", "replace").rstrip())
s.close()
