#!/bin/bash
#
# frank-ecat — eCat3 (retro computers emulator) on RP2350 (FRANK M2)
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Flash a firmware with picotool, with nobody at the board:
#   - a frank-ecat firmware with the USB console up is sent "~BOOT~" and
#     reboots into BOOTSEL;
#   - one that hung reaches BOOTSEL by itself (watchdog -> safe mode ->
#     BOOTSEL, platform.c), this waits for it;
#   - then the image is loaded (a 3 MB image takes over 30 s) and started.
#
# Usage: ./flash.sh [target]     (default frank-ecat-rk86: build/<target>.uf2)
# Environment: CONSOLE=/dev/cu.usbmodemXXXX (default: the first that answers)

set -u
cd "$(dirname "$0")"
TARGET=${1:-frank-ecat-rk86}
UF2="build/${TARGET}.uf2"
[ -f "$UF2" ] || { echo "no $UF2: run ./build.sh $TARGET" >&2; exit 1; }

# "info" fails on a half-written image too, so the device answering is what counts
in_bootsel() { ! timeout 5 picotool info 2>&1 | grep -q "No accessible RP"; }

if ! in_bootsel; then
    for port in ${CONSOLE:-/dev/cu.usbmodem*}; do
        [ -e "$port" ] || continue
        timeout 3 python3 -c "import serial,time,sys; s=serial.Serial(sys.argv[1],115200,timeout=0.2); s.write(b'~BOOT~'); time.sleep(0.3)" "$port" 2>/dev/null
    done
    echo "waiting for BOOTSEL..."
    for _ in $(seq 1 30); do in_bootsel && break; sleep 2; done
fi
in_bootsel || { echo "the board is not in BOOTSEL" >&2; exit 1; }
timeout 180 picotool load -x "$UF2" 2>&1 | tr '\r' '\n' | grep -v "^Loading\|^Verifying\|^$" | tail -2
