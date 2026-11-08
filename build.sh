#!/bin/bash
#
# frank-ecat — eCat3 on RP2350 (FRANK M2)
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Usage: ./build.sh [target ...]       (default: all)
# Environment: CONSOLE=usb|uart (default usb: USB CDC console, no keyboard;
#              uart: USB keyboard host, console on UART0 GPIO 0/1)
#              CPU=504 PSRAM=168 FLASH=126 (frank-quake2's M2 timings), BUILD_DIR=build

set -e
cd "$(dirname "$0")"

if [ -z "$PICO_SDK_PATH" ] || [ ! -f "$PICO_SDK_PATH/pico_sdk_init.cmake" ]; then
    export PICO_SDK_PATH="$HOME/pico/pico-sdk"
fi

CONSOLE=${CONSOLE:-usb}
CPU=${CPU:-504}
PSRAM=${PSRAM:-168}
FLASH=${FLASH:-126}
FLASH_RXDELAY=${FLASH_RXDELAY:-5}
PSRAM_RXDELAY=${PSRAM_RXDELAY:-6}
BUILD_DIR=${BUILD_DIR:-build}

echo "Building frank-ecat: console=$CONSOLE CPU=${CPU}MHz PSRAM=${PSRAM}MHz flash=${FLASH}MHz"
mkdir -p "$BUILD_DIR"
cmake -S . -B "$BUILD_DIR" -DPICO_PLATFORM=rp2350 \
      -DDEBUG_CONSOLE=$CONSOLE -DCPU_SPEED=$CPU -DPSRAM_SPEED=$PSRAM -DFLASH_SPEED=$FLASH \
      -DPSRAM_RXDELAY=$PSRAM_RXDELAY -DFLASH_RXDELAY=$FLASH_RXDELAY >/dev/null
if [ $# -gt 0 ]; then
    cmake --build "$BUILD_DIR" -j8 --target "$@"
else
    cmake --build "$BUILD_DIR" -j8
fi
