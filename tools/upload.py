#!/usr/bin/env python3
#
# frank-ecat — copy a file to the board's SD card over the USB console
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Usage: upload.py LOCAL_FILE /ecat/software/<machine>/NAME [PORT]
# The firmware ("~C put PATH SIZE~") makes the directories and writes the file.

import os
import sys
import time

import serial


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__ if __doc__ else 'usage: upload.py LOCAL REMOTE [PORT]')
    local, remote = sys.argv[1], sys.argv[2]
    port = sys.argv[3] if len(sys.argv) > 3 else '/dev/cu.usbmodem213101'
    data = open(local, 'rb').read()
    s = serial.Serial(port, 115200, timeout=0.2)
    s.reset_input_buffer()
    s.write(('~C put %s %d~' % (remote, len(data))).encode())
    buf = b''
    t = time.time() + 5
    while b'PUT READY' not in buf and b'PUT FAIL' not in buf and time.time() < t:
        buf += s.read(256)
    if b'PUT READY' not in buf:
        sys.exit('upload: no PUT READY: %r' % buf[-200:])
    for i in range(0, len(data), 256):
        s.write(data[i:i + 256])
        s.flush()
    buf = b''
    t = time.time() + 10
    while b'PUT OK' not in buf and b'PUT FAIL' not in buf and time.time() < t:
        buf += s.read(256)
    line = [l for l in buf.decode(errors='replace').splitlines() if 'PUT' in l]
    print('upload %s -> %s: %s' % (os.path.basename(local), remote, line[-1] if line else 'no answer'))
    sys.exit(0 if b'PUT OK' in buf else 1)


if __name__ == '__main__':
    main()
