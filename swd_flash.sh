#!/bin/bash
#
# frank-ecat — flash over SWD with a Debugprobe (CMSIS-DAP).
#
# Usage: ./swd_flash.sh [target] [--reset-only]   (default target: frank-ecat-diag)
#
# 1 MHz: with the diag/game driving HSTX on GPIO12..19, flash erase at 4 MHz
# timed out (ROM call never halted); 1 MHz verifies. Same as frank-linux probe.sh.
set -euo pipefail
cd "$(dirname "$0")"

TARGET=frank-ecat-diag
RESET_ONLY=0
for a in "$@"; do
    case "$a" in
        --reset-only) RESET_ONLY=1 ;;
        *) TARGET="$a" ;;
    esac
done

ARGS=(-f interface/cmsis-dap.cfg -c "adapter speed ${SWD_KHZ:-1000}" -f target/rp2350.cfg)

# A whole-chip reset through the watchdog: openocd's "reset" restarts core 0
# only, so core 1 and the PIO/DMA of the previous firmware (its HDMI output)
# kept running. WDSEL = everything but the oscillators, then TRIGGER.
WDRESET=(-c "mww 0x40018008 0x01fffff3" -c "mww 0x400d8000 0x80000000")

if [ "$RESET_ONLY" = 1 ]; then
    exec openocd "${ARGS[@]}" -c init "${WDRESET[@]}" -c exit
fi

ELF="build/${TARGET}.elf"
[ -f "$ELF" ] || { echo "ERROR: $ELF not found. Run ./build.sh first." >&2; exit 1; }
openocd "${ARGS[@]}" -c "program $ELF verify" "${WDRESET[@]}" -c exit
