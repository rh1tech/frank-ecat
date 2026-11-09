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
| `src/emulator/emulator.cpp` (`timer_proc`), `core.h`, `core.cpp` (`DeviceManager`, `MemoryMapper`), `script/script_engine.*` (`next_wake`) | With `ECAT_EMBEDDED`, one processor: the devices are clocked once a batch of instructions (up to `ECAT_BATCH_MAX` = 32 cycles, ending at the next `clocks_to_event()` and at the script's next action), and an access to any device but memory and the address decoders (`timeless_access()`) clocks them first. Only when every clocked device of the machine declares `batch_safe()` (checked so far: the Агат displays, `Speaker`, ЯЗС, both Агат FDCs, `TapeRecorder`, ВИ53, ВГ75 - which ends a batch at its next line or row through `clocks_to_batch_event()`, converted by the device's divider, its DMA cycles holding the processor within the batch; known difference: `work-rk86-vg75` counts 42111 turns of a loop in a second, not 42110); otherwise every instruction, as upstream. |
| `src/emulator/emulator.*` (`set_parallel()`, `run_partition()`, `parallel_core1_step()`), `core.*` (`Interface::change`, `ecat_cross_*`, `ComputerDevice::m_core`, `DeviceManager` per-domain batches) | With `ECAT_EMBEDDED`: the processors named (a terminal's) run on the second core. Each slice is run as quanta of 100 us both cores finish before either goes on; a line changed on one core for a device of the other is queued and handed over at the next quantum; each core clocks its own domains' devices. Multi-processor machines batch per domain (`batch_safe()` checked for every ДВК device; the ПРТ line clock and the MX disk report their events). |
| `src/emulator/devices/cpu/ie15.*` | With `ECAT_EMBEDDED`: the program ROM read in place, the ports resolved once, and `execute_batched()` running a quantum's port-separated batches without returning. The ИЕ15's logic answers port reads as of the last batch (`timeless_access()`). |
| `src/emulator/core.h` (`MemoryMapper::read_fast()` and the word ones) | With `ECAT_EMBEDDED`: the processors' memory accesses inline where the page is plain RAM or ROM. |
| `src/emulator/devices/common/dl11.*` | With `ECAT_EMBEDDED`: a status poll does not bring the devices up to date (the DL11 ends batches at its own events, and its RBUF read and writes do bring them up to date). Known difference: `work-dvk3-panel-halt` logs the PC of an `INC R0 / BR .-2` loop in the other of its two instructions (a character typed at the console reaches the program a few cycles later). |
| `src/emulator/devices/cpu/k1801vm1.*`, `pdp11core.cpp` | With `ECAT_EMBEDDED`: the word accesses and the bus cycle inline; no interrupt poll when no request line is up. Built at -O3 (cmake/ecat_core.cmake). |
| `src/emulator/core.cpp` (`MemoryMapper::map_read`) | With `ECAT_EMBEDDED`: the answer for an address of an uncached page (the PDP-11 I/O page) kept per address, write memo included; routed ranges are not kept. |
| `src/emulator/devices/specific/dvk_kmd.*` | With `ECAT_EMBEDDED`: a lock across its entry points (the host's processor and the board's run on different cores). |
| `src/emulator/core.h`, `core.cpp` (`MemoryMapper`, `Memory`) | With `ECAT_EMBEDDED`: a page on a plain `RAM` or `ROM` (the classes themselves, no access hook, a ROM not streaming) is read and written in its buffer; the 8-bit port space is resolved once per configuration, as the pages are. |
| `src/emulator/core.h`, `core.cpp` (`MapperPage`, `fill_page`) | With `ECAT_EMBEDDED`: a page that reads one range and writes another (the Орион's ROM over RAM, its RAM pages) keeps both directions in place, each with its own offset; a direction that is not plain goes to the mapper by itself (`uncached_r` / `uncached_w`). |
| `src/emulator/devices/common/wd1793.*`, `generator.*`, `tape.h`, `i8255.h` | With `ECAT_EMBEDDED`: `batch_safe()`, the WD1793 ending batches at its index pulse, register and command delays and head unload, the generator at its pulse edges, the tape recorder every cycle only while it moves, records or has the motor on; the ВВ55 answers as of the last batch (`timeless_access()`). |
| `src/emulator/core.cpp` (`ECAT_SYNC_FOR_W`), `emulator.cpp` (`timer_proc`) | With `ECAT_EMBEDDED`: a write to a clocked device ends the batch after the instruction (`DeviceManager::m_batch_cut`): what it starts - a DMA transfer, a timer reloaded - is seen at once. A batch of one processor is up to 16 us (`batch_max_cycles()`), as with several. |
| `src/emulator/devices/specific/unior_memory.h`, `argo_memory.h`, `argo_keyboard.h`, `zx_keyboard.h` | With `ECAT_EMBEDDED`: `batch_safe()` (the block transfer starts at the clock() after the ВТ57 write that asked for it); the Арго's memory and keyboard port answer without bringing the devices up to date (`timeless_access()`). |
| `src/emulator/devices/common/i8275display.*` | With `ECAT_EMBEDDED`: the unchanged-row cache also with a font in RAM (the Арго's), compared with a copy once a frame. |
| `src/emulator/core.h`, `core.cpp` (`AddressableDevice::page_table()`, `MapperPage::table_r/_w`) | With `ECAT_EMBEDDED`: a device that keeps its memory in 256-byte pages (the Apple II MMU) hands the mapper its page table; `read_fast()`/`write_fast()` read and write those pages in place, a null entry (a switch, a card) goes to the device. |
| `src/emulator/devices/specific/apple2_*.h`, `pravetz_keyboard.h`, `apple2_cards.h`, `common/ins8250.h`, `via6522.h`, `ay8910.h`, `scankeyboard.h`, `specific/oric_ula.h` | With `ECAT_EMBEDDED`: `batch_safe()` (the UARTs and the U-LAN end batches at their next byte); the MMU, the IOU and the Disk II answer without bringing the devices up to date (`timeless_access()`: the Disk II runs its sequencer up to the bus time itself, the IOU's RDVBLBAR and a card's page of the MMU do bring them up to date). Known difference: `disk-pravetz-8a-init` counts 15 bits more written by INIT (a typed key reaches the program a batch later), the disk the same. |
| `src/emulator/devices/specific/apple2_video.*` | With `ECAT_EMBEDDED`: a line whose bytes, modes, monitor and flash phase are those it was drawn with is not drawn again. |
| `src/emulator/core.h`, `core.cpp` (`MemoryMapper::pages_alt`) | With `ECAT_EMBEDDED`: each page of a 16-bit map keeps the entry it had under the configuration before, swapped back when that configuration returns (the БК0011М flips its BASIC ROM banks thousands of times a second). |
| `src/emulator/emulator.cpp` (`Emulator::load_config`, `precise_*`) | With `ECAT_EMBEDDED`: the high precision raster mode of the БК is off unless the ini asks for it (it clocks the machine every instruction). |
| `src/emulator/devices/specific/bk_display.*` | The surface written in `ecat_pixel_t` (it was 32-bit only). With `ECAT_EMBEDDED`: `batch_safe()`, one instruction a batch in the high precision mode. |
| `src/emulator/devices/common/mouse.h`, `specific/uknc_hdd.h`, `smk512.h`, `a16m.h` | With `ECAT_EMBEDDED`: the mouse and the hard disk `batch_safe()` (the disk ends a batch at the end of its command); the СМК-512 and А16М answer without bringing the devices up to date (`timeless_access()`). |
| `src/emulator/devices/specific/uknc_display.*`, `e85_video.*`, `poisk.cpp` | The surface written in `ecat_pixel_t` (they were 32-bit only). With `ECAT_EMBEDDED`: the Э-85's 4096 colours mapped to the palette as a line first shows each; the УК-НЦ display skips a line whose inputs (the planes' bytes, address, scale, colours, cursor) hash as when it was drawn. |
| `src/emulator/core.h`, `core.cpp` (`ComputerDevice::m_domain`, `DeviceManager::pending_of()`), `devices/specific/uknc_*.h`, `uknc_timer.cpp`, `uknc_ram_timing.cpp` | With `ECAT_EMBEDDED`: the УК-НЦ memory timing adds the clocks its domain still owes the devices to its count (the waits are the same as clocked every instruction); it, the keyboard, the sound, the display and the timer (which ends a batch where its counter reaches zero) `batch_safe()`. |
| `src/emulator/devices/common/scn2661.h`, `mc146818.h`, `specific/e85_*.h` | With `ECAT_EMBEDDED`: `batch_safe()`; the SCN2661, the Э-85 communication channel, video, display and КНГМД end batches at their next event (byte, vector ready, line end, the controller's look at its pins). |
| `src/emulator/core.h`, `core.cpp` (`timeless_write()`, `write_ends_batch()`) | With `ECAT_EMBEDDED`: hooks for a device whose writes, unlike its reads, bring the devices up to date, and for one whose write need not end the batch (none of the shipped devices uses them yet). |
| `src/emulator/devices/common/i8253.*` | With `ECAT_EMBEDDED`: a batch ends where a channel with a period of 256 counts or more counts out (its output moves: an interrupt, the Поиск's sound gate); a faster one (a tone, a baud rate) is counted over the batch as before. |
| `src/emulator/devices/cpu/i8086.cpp` | With `ECAT_EMBEDDED`: memory through the mapper's inline `read_fast()`/`write_fast()` and the word ones. |
| `src/emulator/devices/specific/poisk.*` | With `ECAT_EMBEDDED`: the display ends batches where its sync lines change (polled through a ВВ55) and adds the clocks still owed to its memory waits; the floppy control ends one where its motor times out. |
| `src/emulator/devices/specific/dvk_kcgd.*` | With `ECAT_EMBEDDED`: the КЦГД keeps, per 64-word chunk of its video memory, the count of writes at the last one; the display skips a line drawn after the last write to every chunk it covers, from the same address and palette. |
| `src/emulator/core.cpp` (`MemoryMapper::fill_page`) | With `ECAT_EMBEDDED`: a read-only routed range that answers a whole page (the КЦГД's strict ROM) has the page's reads cached; its writes take the full scan. |
| `src/emulator/devices/specific/irisha_display.*` | With `ECAT_EMBEDDED`: only the bytes of the video memory written since the last picture are drawn again (a range kept by the write callback); a mode, colour or page change draws the whole. |
| `src/emulator/core.h` (`PortAddress`) | With `ECAT_EMBEDDED`: a port-address read (a keyboard's code, polled in a loop) does not bring the devices up to date unless it stores the address read; a write does, without ending the batch. Known difference: `work-agat-9-yazs-regs` logs the ЯЗС's envelope and drum counters a step apart. |
| `src/emulator/devices/common/keyboard.*`, `src/libs/dsk_tools/src/charmaps.*` | With `ECAT_EMBEDDED`: the key names and the character maps as C strings in flash (as `std::string` they were 120 KB of SRAM, `KEYS[]` copied into every file including the header). |
| `src/emulator/devices/specific/agat_9_mapper.*` | With `ECAT_EMBEDDED`: the PROM decoded per 4 KB page and kept until the switches, PM or the page map change; plain RAM read and written in its buffer. |
| `src/emulator/devices/specific/agat_7_display.*`, `agat_9_display.*` (render) | A line's 16 colours from a table built once a line. With `ECAT_EMBEDDED`: a line drawn from the same bytes, mode, bank, page, palette and blink phase is not drawn again (every line once a second regardless). |
| `src/emulator/devices/common/fdd.cpp` (`FDD::load_config`) | A disk image named by the configuration but not found leaves the drive empty with `ECAT_EMBEDDED`, as in the WASM build (the library is on a card that may not carry it). |
| `src/emulator/devices/common/tape.cpp` (`TapeRecorder::load_config`) | The same for a tape image named by the configuration: with `ECAT_EMBEDDED` the recorder starts empty. |

## Displays (written in `ecat_pixel_t`, neutral upstream)

| File | Machines |
|---|---|
| `devices/common/i8275display.cpp` | Радио-86РК, Микроша, Апогей, Спектр-001, Юниор, Арго |
| `devices/specific/o128display.cpp` | Орион-128 |
| `devices/specific/irisha_display.cpp` | Ириша, Диалог, Каспий (the 20 blank lines: `20 * line_bytes / sizeof(ecat_pixel_t)` dots, was `5 * line_bytes` of `uint32_t`) |
| `devices/specific/agat_7_display.cpp`, `agat_9_display.cpp` | Агат-7, Агат-9 (`PXB()` takes the RGBA byte offsets to the pixel size) |
| `devices/specific/apple2_video.cpp` | Правец-82/8М/8А/8С |
| `devices/specific/dvk_ie15.cpp`, `dvk_ksm_display.cpp`, `dvk_kcgd.*` | ДВК (ИЕ15, КСМ with КГД, КЦГД); with `ECAT_EMBEDDED` the ИЕ15 skips a line and the КСМ a row of characters drawn from the same memory |
| `devices/specific/oric_ula.cpp` | Правец-8Д |

Not converted yet: the БК, УК-НЦ, Э-85 and Поиск-1 displays. With `ECAT_EMBEDDED` their tests fail.

## Console build

| File | Change |
|---|---|
| `src/CMakeLists.txt` | Option `ECAT_EMBEDDED_CHECK` (default OFF): `eCat3-headless` with `ECAT_EMBEDDED`. |
| `src/headless/renderer_null.h`, `.cpp` | With `ECAT_EMBEDDED`: an 8-bit surface, `MapRGB()` through `IndexedPalette`, screenshots expanded back to RGBA so the upstream references apply. |
