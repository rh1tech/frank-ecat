# frank-ecat

[eCat3](https://github.com/Ptr314/ecat3), the universal emulator of Soviet and
Eastern-bloc computers, on the RP2350 — the FRANK / Murmulator M2 board: HDMI
with sound, I2S as an alternative, 8 MB PSRAM, SD card, USB keyboard.

One firmware (UF2) per family of machines, all from one code base: the upstream
eCat3 core, compiled for the microcontroller, with each family's
configurations and ROMs packed into flash.

Work in progress. Running now with sound over HDMI, at full speed unless noted (the load is a core's busy share at the machine's own clock):

| Firmware | Machines | Load at full speed |
|---|---|---|
| `frank-ecat-rk86` | Радио-86РК, Спектр-001, Микроша, Апогей БК-01Ц | 41-48% of a core |
| `frank-ecat-dvk` | ДВК-1, ДВК-2 (ИЕ15), ДВК-3 (КСМ, КГД), ДВК-4 (КЦГД) | both cores: ДВК-1 and ДВК-2 100% (90% busy), ДВК-3 100% (RT-11 82% busy, the console prompt 99%), ДВК-4 75-80% (4.1-4.4 of 5.5 MHz, the КЦГД's processor on the table timing): not yet |
| `frank-ecat-agat` | Агат-7 (140 KB / 840 KB drives, -pp, -max), Агат-9 (-pp, ЯЗС) | 51-88% |
| `frank-ecat-orion` | Орион-128 (М1, М3 with disks, Z80 Card II) | ~52% (М1, М3), 84% (Z80 Card II at 5 MHz) |
| `frank-ecat-irisha` | Ириша (ГМД-70, КНГМД), Диалог, Каспий | 62-75% |
| `frank-ecat-unior` | Юниор ФВ-6506, Арго ФВ-6511 | 53% (Юниор), 68% (Арго) |
| `frank-ecat-pravetz` | Правец-82, -8М, -8А, -8А (full), -8С (Apple II), Правец-8Д (Oric) | 49-84% |
| `frank-ecat-bk` | БК0010, БК0010-01 and БК0011М, each plain, with the КНГМД, А16М, СМК-512; БК0011М МСТД | 69-91% |
| `frank-ecat-uknc` | УК-НЦ, УК-НЦ (031), УК-НЦ (Бейсик) | both cores, a fast mode (the table timing, no memory waits, the picture at 25 Hz, double batches: `platform/machine_overrides.txt`): 85-100% (6.8-8.0 of 8 MHz); RT-11 boots from the disk |
| `frank-ecat-e85` | Электроника МС 0585, with МС 1702, with the hard disk | both cores (the КНГМД's 8051 on the second): 100% at the ПРОС prompt (73% busy), 65-85% while loading |
| `frank-ecat-poisk` | Поиск-1, 128 КБ, with the cartridge RAM, with the КНГМД 302 | 96-100% (MS-DOS 5 and Volkov Commander from the disk) |

Disk and tape images go on the card under `/ecat/software/<machine type>/`
(`agat`, `radio-86rk`, `orion-128`, `irisha`, `unior`, `pravetz`, `bk`, `uknc`, `e85`, `poisk`, ...), where the file browser (F11) opens.

## Layout

| Path | |
|---|---|
| `ecat3/` | the vendored eCat3 (`src/`, `deploy/`, `tests/`), see `ecat3/UPSTREAM.md` and `ecat3/PATCHES.md` |
| `cmake/ecat_core.cmake` | the eCat3 core as a firmware library (`ECAT_EMBEDDED`, `ECAT_SINGLE_THREAD`) |
| `ui/` | the menus, from frank-dino (`ui/ecat_ui.h` is what they ask of the emulator) |
| `platform/` | board bring-up, heap (SRAM + PSRAM), files (flash VFS + SD), sound (HDMI / I2S), the emulator frontend, the diag firmware |
| `drivers/` | HDMI (pico-speccy, HSTX back-end), I2S, PSRAM, SD / FatFs, USB HID |
| `tools/embed_vfs.py` | packs a family's machines into the flash VFS |
| `tools/hot_sections.py`, `platform/hot_functions.txt` | the emulator functions that run from SRAM, per family (`@ family` lines) |

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
switches to machine N of the family, `~C k down ret~` keys (cursor, `esc`, `tab`, `bs`, `f1`..), `~C s~` status (with the reset reason and a crash before it), `~C d~` display/CPU
state, `~C c DEVICE COMMAND PARAMS~` a device command (as a script's
`COMMAND device.command(params)`), `~C f DEVICE FIELD...~` device fields (as
`LOG device.field`), `~C prof~` a sampling profile (`prof1` of core 1, `profl`/`prof1l` with the callers; tools/prof.py), `~BOOT~`, `~RST~`.

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
