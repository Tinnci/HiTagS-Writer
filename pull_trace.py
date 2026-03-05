#!/usr/bin/env python3
"""Pull a file from Flipper Zero SD card via CLI serial."""
import serial
import time
import sys

PORT = '/dev/ttyACM1'
REMOTE = '/ext/lfrfid/Trace_00000000.htsd'
LOCAL = 'trace_captured.htsd'

s = serial.Serial(PORT, timeout=8)
time.sleep(0.3)
s.read(s.in_waiting)

s.write(f'storage read {REMOTE}\r\n'.encode())
time.sleep(6)

resp = b''
while True:
    chunk = s.read(4096)
    if not chunk:
        break
    resp += chunk

s.close()

text = resp.decode('utf-8', errors='replace')
lines = text.split('\n')

start = 0
end = len(lines)
for i, l in enumerate(lines):
    if 'Size:' in l:
        start = i + 1
    if l.strip().startswith('>:'):
        end = i
        break

content = '\n'.join(lines[start:end])
with open(LOCAL, 'w') as f:
    f.write(content)

print(f'Saved {len(content)} bytes to {LOCAL}')
