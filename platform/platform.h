/*
 * frank-ecat — eCat3 on RP2350 (from frank-quake2 / frank-castle)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Board bring-up shared by the diag and emulator firmware. */

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* stdio, then voltage + flash timings + sys clock. Returns the clock in Hz. */
uint32_t platform_init_clocks(void);

/* PSRAM controller, .psram_data/.psram_bss, dlmalloc heap. Must follow the
 * final clock change. Returns the detected CS pin. */
unsigned platform_init_psram(void);

/* TinyUSB host + 1 kHz tuh_task() pump. */
void platform_init_usb(void);

/* Mount the SD card. Returns the FatFs result (0 = FR_OK). */
int platform_mount_sd(void);

/* Print and clear crash info saved by the previous boot's fault handler. */
void platform_report_previous_crash(void);
/* the reset reason and a crash before it, again for a console that opened late */
void platform_report_boot(void);

/* DWT cycle counter, for profiling. */
void platform_cyccnt_enable(void);
static inline uint32_t platform_cyccnt(void) {
    return *(volatile uint32_t *)0xE0001004;
}

void __attribute__((noreturn)) platform_reboot(bool bootsel);
/* the same, asked for (keeps the next boot out of safe mode) */
void __attribute__((noreturn)) platform_reboot_clean(bool bootsel);
void platform_uart_cmd(int ch);
int platform_debug_cmd_line(char *out, int n);
extern volatile bool platform_perf_verbose;

/* C heap (ecat_malloc.c): usage report, explicit SRAM / PSRAM blocks */
void ecat_heap_report(void);
void *ecat_sram_malloc(size_t n);
void *ecat_psram_malloc(size_t n);
void platform_usb_poll(void);

/* Hang recovery: a hardware watchdog fed by the main loop; breadcrumbs (a
 * stage and an argument) kept over the reboot; after a watchdog reboot,
 * platform_safe_mode_if_hung() prints them and enters BOOTSEL */
void platform_watchdog_start(uint32_t ms);
void platform_watchdog_feed(void);
void platform_crumb(uint32_t stage, uint32_t arg);
void platform_crumb_tick(void);
void platform_safe_mode_if_hung(void);
/* a 100 ms timer IRQ that records where core 0 stands when the main loop
 * (platform_crumb_tick) has not moved for 2 s; safe mode prints it */
void platform_stall_watch_start(void);
/* sampling profiler on the same timer: 4096 PCs of core 0 every `us`, then
 * platform_prof_dump_if_done() (main loop) prints them ("PROF BEGIN" .. "PROF END") */
void platform_prof_start(uint32_t us);
void platform_prof_pairs(bool on);
void platform_prof_core1_init(void);
unsigned platform_core1_stack_used(void);
void platform_prof_start_core1(uint32_t us);
bool platform_prof_dump_if_done(void);

/* core 1 on a 4 KB stack in main RAM */
void platform_launch_core1(void (*entry)(void));

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_H */
