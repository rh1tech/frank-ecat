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
| `src/emulator/devices/common/fdd.cpp` (`FDD::load_config`) | A disk image named by the configuration but not found leaves the drive empty with `ECAT_EMBEDDED`, as in the WASM build (the library is on a card that may not carry it). |

## Displays (written in `ecat_pixel_t`, neutral upstream)

| File | Machines |
|---|---|
| `devices/common/i8275display.cpp` | Радио-86РК, Микроша, Апогей, Спектр-001, Юниор, Арго |
| `devices/specific/o128display.cpp` | Орион-128 |
| `devices/specific/irisha_display.cpp` | Ириша, Диалог, Каспий (the 20 blank lines: `20 * line_bytes / sizeof(ecat_pixel_t)` dots, was `5 * line_bytes` of `uint32_t`) |
| `devices/specific/agat_7_display.cpp`, `agat_9_display.cpp` | Агат-7, Агат-9 (`PXB()` takes the RGBA byte offsets to the pixel size) |
| `devices/specific/apple2_video.cpp` | Правец-82/8М/8А/8С |
| `devices/specific/oric_ula.cpp` | Правец-8Д |

Not converted yet (out of the first release): the БК, УК-НЦ, ДВК (КСМ, ИЕ15, КГД, КЦГД),
Э-85 and Поиск-1 displays. With `ECAT_EMBEDDED` their tests fail.

## Console build

| File | Change |
|---|---|
| `src/CMakeLists.txt` | Option `ECAT_EMBEDDED_CHECK` (default OFF): `eCat3-headless` with `ECAT_EMBEDDED`. |
| `src/headless/renderer_null.h`, `.cpp` | With `ECAT_EMBEDDED`: an 8-bit surface, `MapRGB()` through `IndexedPalette`, screenshots expanded back to RGBA so the upstream references apply. |
