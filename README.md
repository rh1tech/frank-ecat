# frank-ecat

[eCat3](https://github.com/Ptr314/ecat3), the universal emulator of Soviet and
Eastern-bloc computers, on the RP2350 — the FRANK / Murmulator M2 board: HDMI
with sound, I2S as an alternative, 8 MB PSRAM, SD card, USB keyboard.

One firmware (UF2) per family of machines, all from one code base: the upstream
eCat3 core, compiled for the microcontroller, with each family's
configurations and ROMs packed into flash.

Work in progress. Running now: the Радио-86РК family (Радио-86РК, Спектр-001,
Микроша, Апогей) at full speed (1.77 MHz, ~70% of a core), with sound over
HDMI.

## Layout

| Path | |
|---|---|
| `ecat3/` | the vendored eCat3 (`src/`, `deploy/`, `tests/`), see `ecat3/UPSTREAM.md` and `ecat3/PATCHES.md` |
| `cmake/ecat_core.cmake` | the eCat3 core as a firmware library (`ECAT_EMBEDDED`, `ECAT_SINGLE_THREAD`) |
| `ui/` | the menus, from frank-dino (`ui/ecat_ui.h` is what they ask of the emulator) |
| `platform/` | board bring-up, heap (SRAM + PSRAM), files (flash VFS + SD), sound (HDMI / I2S), the emulator frontend, the diag firmware |
| `drivers/` | HDMI (pico-speccy, HSTX back-end), I2S, PSRAM, SD / FatFs, USB HID |
| `tools/embed_vfs.py` | packs a family's machines into the flash VFS |
| `tools/hot_sections.py`, `platform/hot_functions.txt` | the emulator functions that run from SRAM |

## Build

    ./build.sh                       # every firmware
    ./build.sh frank-ecat-rk86       # one
    CONSOLE=uart ./build.sh          # USB keyboard host, console on UART0 (GPIO 0/1)

The `.ext` machines are flattened at build time by the host console build of
eCat3, so build it first:

    cmake -S ecat3/src -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_GUI=OFF -DENABLE_HEADLESS=ON
    cmake --build build-host

## Flash

    ./flash.sh frank-ecat-rk86

Sends `~BOOT~` to a running firmware, or waits for a hung one: every firmware
feeds a watchdog, and after a hang it prints where it stopped and enters
BOOTSEL by itself.

## Menus

frank-dino's menus, in the colours of a Soviet manual: **F11** the file
browser (a program loads into memory, a tape image goes on the machine's
tape recorder, a disk image into its drive), **F12** the settings (model,
reset, tape transport, HDMI or I2S sound, volume, emulator info, "Save
settings to the card" into `/ecat/frank.ini`). At power-on the models of the
firmware are listed for 3 s, the last one used marked; a key stays.

Console (USB CDC): a typed line goes to the machine's keyboard; `~C m~` opens
the settings and `~C files~` the files (driven with ANSI cursor keys, Enter,
Esc, Tab to close), `~C boot~` restarts to the model list, `~C m N~`
switches to machine N of the family, `~C s~` status, `~C d~` display/CPU
state, `~C c DEVICE COMMAND PARAMS~` a device command (as a script's
`COMMAND device.command(params)`), `~C f DEVICE FIELD...~` device fields (as
`LOG device.field`), `~C prof~` a sampling profile, `~BOOT~`, `~RST~`.

## Host check

The core patches are checked against eCat3's own test suite:

    cmake -S ecat3/src -B build-host-emb -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DENABLE_GUI=OFF -DENABLE_HEADLESS=ON -DECAT_EMBEDDED_CHECK=ON
    cmake --build build-host-emb
    tools/host_check.sh              # the machines this port ships
    (cd ecat3 && python3 tests/run_tests.py -j 8 --exe ../build-host/eCat3-headless)   # the stock build, all 328

## Contributing

Install the hooks once per clone:

    tools/install-hooks.sh

They refuse commits whose message or identity credits an AI tool
(`tools/check-attribution.sh`).

## License

GPL-3.0-or-later; eCat3 is GPL-3.0, (c) Mikhail Revzin.
