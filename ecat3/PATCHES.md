# frank-ecat patches to the vendored eCat3

Everything frank-ecat changes in this tree, file by file. A change either sits behind
`ECAT_EMBEDDED` (defined by the RP2350 firmware build and by the `ECAT_EMBEDDED_CHECK`
console build), or is neutral without it: with `ECAT_EMBEDDED` undefined the code
compiles to exactly what upstream does (the console build passes the whole suite,
328/328, see the top-level README).

## Core

| File | Change |
|---|---|
| `src/emulator/core.h`, `core.cpp` | `Interface::linked_interfaces` is a `std::vector<LinkData>` that grows link by link instead of `LinkData[MAX_LINKS]` (4 KB per line). `MapperArray` holds `MAPPER_MAX_RANGES` entries (48 instead of 100; the largest shipped map has 36), `load_config` reports a map that overflows it. |
| `src/emulator/renderer.h` | `ecat_pixel_t`: one dot of the surface, `uint32_t` RGBA upstream, `uint8_t` palette index with `ECAT_EMBEDDED`. |
| `src/emulator/indexed_palette.h` | New: `IndexedPalette`, the colour → index table behind `MapRGB()` on an 8-bit surface (index 0 is black; 224 entries, the rest of the 256 are the firmware menu's and the border). |
| `src/emulator/core.cpp` (`GenericDisplay::validate`) | Surface width check by `sizeof(ecat_pixel_t)`. |
| `src/emulator/emulator.cpp` (`timer_proc`), `core.h`, `core.cpp` (`DeviceManager`, `MemoryMapper`), `script/script_engine.*` (`next_wake`) | With `ECAT_EMBEDDED`, one processor: the devices are clocked once a batch of instructions (up to `ECAT_BATCH_MAX` = 32 cycles, ending at the next `clocks_to_event()` and at the script's next action), and an access to any device but memory and the address decoders (`timeless_access()`) clocks them first. Only when every clocked device of the machine declares `batch_safe()` (checked so far: the Агат displays, `Speaker`, ЯЗС, both Агат FDCs, `TapeRecorder`, ВИ53, ВГ75 - which ends a batch at its next DMA cycle, line or row through `clocks_to_batch_event()`, converted by the device's divider); otherwise every instruction, as upstream. |
| `src/emulator/core.h`, `core.cpp` (`MemoryMapper`, `Memory`) | With `ECAT_EMBEDDED`: a page on a plain `RAM` or `ROM` (the classes themselves, no access hook, a ROM not streaming) is read and written in its buffer; the 8-bit port space is resolved once per configuration, as the pages are. |
| `src/emulator/devices/common/keyboard.*`, `src/libs/dsk_tools/src/charmaps.*` | With `ECAT_EMBEDDED`: the key names and the character maps as C strings in flash (as `std::string` they were 120 KB of SRAM, `KEYS[]` copied into every file including the header). |
| `src/emulator/devices/specific/agat_9_mapper.*` | With `ECAT_EMBEDDED`: the PROM decoded per 4 KB page and kept until the switches, PM or the page map change; plain RAM read and written in its buffer. |
| `src/emulator/devices/specific/agat_7_display.*`, `agat_9_display.*` (render) | A line's 16 colours from a table built once a line. With `ECAT_EMBEDDED`: a line drawn from the same bytes, mode, bank, page, palette and blink phase is not drawn again (every line once a second regardless). |
| `src/emulator/devices/common/fdd.cpp` (`FDD::load_config`) | A disk image named by the configuration but not found leaves the drive empty with `ECAT_EMBEDDED`, as in the WASM build (the library is on a card that may not carry it). |

## Displays (written in `ecat_pixel_t`, neutral upstream)

| File | Machines |
|---|---|
| `devices/common/i8275display.cpp` | Радио-86РК, Микроша, Апогей, Спектр-001, Юниор, Арго |
| `devices/specific/o128display.cpp` | Орион-128 |
| `devices/specific/irisha_display.cpp` | Ириша, Диалог, Каспий (the 20 blank lines: `20 * line_bytes / sizeof(ecat_pixel_t)` dots, was `5 * line_bytes` of `uint32_t`) |
| `devices/specific/agat_7_display.cpp`, `agat_9_display.cpp` | Агат-7, Агат-9 (`PXB()` takes the RGBA byte offsets to the pixel size) |
| `devices/specific/apple2_video.cpp` | Правец-82/8М/8А/8С |
| `devices/specific/dvk_ie15.cpp`, `dvk_ksm_display.cpp`, `dvk_kcgd.*` | ДВК (ИЕ15, КСМ with КГД, КЦГД); ИЕ15 also skips a line drawn from the same cells with `ECAT_EMBEDDED` |
| `devices/specific/oric_ula.cpp` | Правец-8Д |

Not converted yet: the БК, УК-НЦ, Э-85 and Поиск-1 displays. With `ECAT_EMBEDDED` their tests fail.

## Console build

| File | Change |
|---|---|
| `src/CMakeLists.txt` | Option `ECAT_EMBEDDED_CHECK` (default OFF): `eCat3-headless` with `ECAT_EMBEDDED`. |
| `src/headless/renderer_null.h`, `.cpp` | With `ECAT_EMBEDDED`: an 8-bit surface, `MapRGB()` through `IndexedPalette`, screenshots expanded back to RGBA so the upstream references apply. |
