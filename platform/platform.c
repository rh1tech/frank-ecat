/*
 * frank-ecat — eCat3 on RP2350 (board bring-up from frank-quake2 / frank-castle)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Board bring-up, following frank-fodder's main.c order:
 * voltage -> flash timings -> sys clock -> PSRAM (once, at the final clock).
 */

#include "platform.h"
#include "board_config.h"
#include "psram_init.h"

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/structs/psm.h"
#include "hardware/structs/qmi.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "hardware/structs/timer.h"
#include "hardware/sync.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"
#include "ff.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Reboot                                                              */
/* ------------------------------------------------------------------ */

/* Where the firmware was, kept over a watchdog reboot (and its fault
 * handlers' reboots): a stage number, an argument, and how many times the
 * main loop went round. A deliberate reboot clears it (platform_reboot_clean) */
static volatile uint32_t __attribute__((section(".uninitialized_data"))) crumbs[4];
#define CRUMB_MAGIC 0xC4B5A000u
/* the program counter and link register of core 0 when the stall watcher saw
 * the main loop standing still (platform_stall_watch_start) */
static volatile uint32_t __attribute__((section(".uninitialized_data"))) stall_pc[3];

/* A reboot that was asked for (a command, a machine switch): not a hang */
void __attribute__((noreturn)) platform_reboot_clean(bool bootsel) {
    crumbs[0] = 0;
    platform_reboot(bootsel);
}

/* Every software reboot goes through here: the PSRAM must leave QPI mode
 * first, or the boot ROM cannot read the flash and the chip hangs until a
 * power cycle (see psram_reset_to_spi). */
void __attribute__((noreturn)) platform_reboot(bool bootsel) {
#if !ECAT_CONSOLE_USB
    extern void uart_buffered_flush(void);
    uart_buffered_flush(); /* crash reports sit in the ring until drained */
#endif
    if (get_core_num() == 0) {
        /* stop core 1 (it may be reading PSRAM). Not multicore_reset_core1():
         * that waits for core 1's boot ROM to answer on the FIFO, and hung
         * here once; we are rebooting anyway, so just force it off. */
        hw_set_bits(&psm_hw->frce_off, PSM_FRCE_OFF_PROC1_BITS);
        while (!(psm_hw->frce_off & PSM_FRCE_OFF_PROC1_BITS)) tight_loop_contents();
        busy_wait_us(10);
        /* release it again: it restarts into the boot ROM, which waits for a
         * launch and does not touch PSRAM. Left forced off, the bit survives
         * the watchdog reboot and the chip does not come back. */
        hw_clear_bits(&psm_hw->frce_off, PSM_FRCE_OFF_PROC1_BITS);
        busy_wait_us(10);
    }
    save_and_disable_interrupts();
    psram_reset_to_spi();
    if (bootsel) reset_usb_boot(0, 0);
    watchdog_reboot(0, 0, 10);
    while (1) tight_loop_contents();
}

volatile bool platform_perf_verbose; /* "~VERB~" toggles the detailed perf log */
static char dbg_cmd[96];
volatile bool platform_uart_scripted; /* a "~C" command came: letters stop being keys */
static volatile bool dbg_cmd_ready;

/* the main loop takes the pending "~C ...~" command, if any */
int platform_debug_cmd_line(char *out, int n) {
    if (!dbg_cmd_ready) return 0;
    strncpy(out, dbg_cmd, (size_t)n - 1);
    out[n - 1] = 0;
    dbg_cmd_ready = false;
    return 1;
}

/* "~BOOT~" on the UART reboots into BOOTSEL, so the board can be reflashed
 * over USB (picotool) without touching it. Feed every received byte here. */
void platform_uart_cmd(int ch) {
    static const char magic[] = "~BOOT~";
    static unsigned pos;
    pos = (ch == magic[pos]) ? pos + 1 : (ch == magic[0]);
    if (pos == sizeof(magic) - 1) {
        printf("\nrebooting to BOOTSEL\n");
        sleep_ms(20);
        platform_reboot_clean(true);
    }
    /* "~RST~": plain reboot (test scripts restart the firmware this way) */
    static const char rst[] = "~RST~";
    static unsigned rpos;
    rpos = (ch == rst[rpos]) ? rpos + 1 : (ch == rst[0]);
    if (rpos == sizeof(rst) - 1) {
        printf("\nrebooting\n");
        sleep_ms(20);
        platform_reboot_clean(false);
    }
    /* "~VERB~": toggle the detailed perf log */
    static const char verb[] = "~VERB~";
    static unsigned vpos;
    vpos = (ch == verb[vpos]) ? vpos + 1 : (ch == verb[0]);
    if (vpos == sizeof(verb) - 1) {
        extern volatile bool platform_perf_verbose;
        vpos = 0;
        platform_perf_verbose = !platform_perf_verbose;
    }
    /* "~C LINE~": a debug command for the main loop,
     * except "pad BITS FRAMES", handled by the pad code */
    {
        static char line[96];
        static int cst, clen;
        if (cst == 3) {
            if (ch == '~') {
                line[clen] = 0;
                cst = 0;
                unsigned bits, frames;
                if (sscanf(line, "pad %x %u", &bits, &frames) == 2) {
                    extern void platform_pad_hold(unsigned bits, unsigned frames) __attribute__((weak));
                    if (platform_pad_hold) platform_pad_hold(bits, frames);
                } else if (!dbg_cmd_ready) {
                    memcpy(dbg_cmd, line, (size_t)clen + 1);
                    dbg_cmd_ready = true;
                }
            } else if (clen < (int)sizeof(line) - 1) {
                line[clen++] = (char)ch;
            } else {
                cst = 0;
            }
        } else if (ch == '~') {
            cst = 1;
        } else if (cst == 1 && ch == 'C') {
            cst = 2;
        } else if (cst == 2 && ch == ' ') {
            cst = 3;
            platform_uart_scripted = true;
            clen = 0;
        } else {
            cst = 0;
        }
    }
    /* "~PROF~": profile from now on (profiler builds link profiler_rearm) */
    extern void profiler_rearm(void) __attribute__((weak));
    static const char prof[] = "~PROF~";
    static unsigned ppos;
    ppos = (ch == prof[ppos]) ? ppos + 1 : (ch == prof[0]);
    if (ppos == sizeof(prof) - 1) {
        ppos = 0;
        if (profiler_rearm) {
            printf("\nprofiling\n");
            profiler_rearm();
        }
    }
}

/* ------------------------------------------------------------------ */
/* Crash capture (survives the watchdog reboot)                        */
/* ------------------------------------------------------------------ */

static volatile uint32_t __attribute__((section(".uninitialized_data"))) crash_info[12];

#define CRASH_HARDFAULT 0xDEADBEEFu
#define CRASH_PANIC     0xDEADCAFEu

void __real_panic(const char *fmt, ...);

void __attribute__((noreturn)) __wrap_panic(const char *fmt, ...) {
    /* TinyUSB host asserts during flaky enumeration are not worth a reboot:
     * park this core and let the other one keep running. */
    if (fmt && (strstr(fmt, "Host") || strstr(fmt, "HCD") || strstr(fmt, "tuh") ||
                strstr(fmt, "EP"))) {
        while (1) tight_loop_contents();
    }
    void *lr;
    __asm volatile("mov %0, lr" : "=r"(lr));
    crash_info[0] = CRASH_PANIC;
    crash_info[1] = (uint32_t)(uintptr_t)lr;
    crash_info[2] = (uint32_t)(uintptr_t)fmt;
    crash_info[3] = get_core_num();
    printf("\n*** PANIC on core %u ***\n", (unsigned)crash_info[3]);
    if (fmt) {
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
        printf("\n");
    }
    sleep_ms(100);
    platform_reboot(false);
}

void __attribute__((used)) HardFault_Handler_C(uint32_t *frame) {
    crash_info[0] = CRASH_HARDFAULT;
    crash_info[1] = frame[6]; /* PC */
    crash_info[2] = frame[5]; /* LR */
    crash_info[3] = frame[0];
    crash_info[4] = frame[1];
    crash_info[5] = *(volatile uint32_t *)0xE000ED28; /* CFSR */
    crash_info[6] = *(volatile uint32_t *)0xE000ED2C; /* HFSR */
    crash_info[7] = *(volatile uint32_t *)0xE000ED38; /* BFAR */
    crash_info[8] = get_core_num();
    printf("\n*** HARDFAULT core %u PC=0x%08x LR=0x%08x CFSR=0x%08x BFAR=0x%08x ***\n",
           (unsigned)crash_info[8], (unsigned)crash_info[1], (unsigned)crash_info[2],
           (unsigned)crash_info[5], (unsigned)crash_info[7]);
    busy_wait_ms(500);
    platform_reboot(false);
}

void __attribute__((naked, used)) isr_hardfault(void) {
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "b HardFault_Handler_C\n");
}

void platform_report_previous_crash(void) {
    if (crash_info[0] == CRASH_HARDFAULT) {
        printf("*** PREVIOUS CRASH (HardFault) core %u ***\n", (unsigned)crash_info[8]);
        printf("PC=0x%08x LR=0x%08x R0=0x%08x R1=0x%08x\n", (unsigned)crash_info[1],
               (unsigned)crash_info[2], (unsigned)crash_info[3], (unsigned)crash_info[4]);
        printf("CFSR=0x%08x HFSR=0x%08x BFAR=0x%08x\n", (unsigned)crash_info[5],
               (unsigned)crash_info[6], (unsigned)crash_info[7]);
    } else if (crash_info[0] == CRASH_PANIC) {
        printf("*** PREVIOUS CRASH (panic) core %u LR=0x%08x ***\n", (unsigned)crash_info[3],
               (unsigned)crash_info[1]);
        const char *msg = (const char *)(uintptr_t)crash_info[2];
        if (msg) printf("Message: %s\n", msg);
    }
    crash_info[0] = 0;
}

/* ------------------------------------------------------------------ */
/* Clocks                                                              */
/* ------------------------------------------------------------------ */

/* rxdelay counts half sys clock cycles, so a delay tuned for the final
 * clock is far too long at the boot clock: the safe timing goes in before
 * the clock is raised, the fast one after (tools: diag 'f' sweeps it). */
/* COOLDOWN 1; 3 (0xE0007000) measured no faster for the flash */
#ifndef FLASH_TIMING_BASE
#define FLASH_TIMING_BASE 0x60007000u
#endif

static void __no_inline_not_in_flash_func(set_flash_timings)(int cpu_mhz, int max_mhz, int rx_override) {
    const int clock_hz = cpu_mhz * 1000000;
    const int max_flash_freq = max_mhz * 1000000;

    int divisor = (clock_hz + max_flash_freq - (max_flash_freq >> 4) - 1) / max_flash_freq;
    if (divisor == 1 && clock_hz >= 166000000) divisor = 2;

    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000 && clock_hz >= 166000000) rxdelay += 1;
    if (rx_override >= 0) rxdelay = rx_override;

    qmi_hw->m[0].timing = FLASH_TIMING_BASE | rxdelay << QMI_M0_TIMING_RXDELAY_LSB |
                          divisor << QMI_M0_TIMING_CLKDIV_LSB;
}

#ifndef FLASH_RXDELAY
#define FLASH_RXDELAY -1
#endif
#define FLASH_SAFE_MHZ 66

uint32_t platform_init_clocks(void) {
    stdio_init_all();
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("\n==== frank-ecat %s ====\n", ECAT_VERSION);
    if (watchdog_caused_reboot()) printf("(watchdog reboot)\n");

#if CPU_CLOCK_MHZ > 252
    vreg_disable_voltage_limit();
    vreg_set_voltage(CPU_VOLTAGE);
    set_flash_timings(CPU_CLOCK_MHZ, FLASH_SAFE_MHZ, -1);
    sleep_ms(10);
#endif
    if (!set_sys_clock_khz(CPU_CLOCK_MHZ * 1000, false)) {
        printf("%d MHz unreachable, falling back to 252 MHz\n", CPU_CLOCK_MHZ);
        set_sys_clock_khz(252 * 1000, true);
    }
    if (FLASH_MAX_FREQ_MHZ > FLASH_SAFE_MHZ)
        set_flash_timings((int)(clock_get_hz(clk_sys) / 1000000), FLASH_MAX_FREQ_MHZ, FLASH_RXDELAY);
#if PERI_CLOCK_MHZ
    /* clk_peri (UART, SPI) from clk_sys instead of the 48 MHz USB PLL: the
     * SD card's SPI runs at clk_peri / an even divider, 24 MHz at most from
     * 48 MHz. The UART divisor is set again below. */
    {
        uint32_t sys = clock_get_hz(clk_sys);
        uint32_t div = (sys + PERI_CLOCK_MHZ * 1000000u / 2) / (PERI_CLOCK_MHZ * 1000000u);
        if (div < 1) div = 1;
        clock_configure_int_divider(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, sys, div);
    }
#endif
    /* The UART baud divisor was computed at the boot clock. */
    stdio_init_all();
#if !ECAT_CONSOLE_USB
    extern void uart_buffered_init(void);
    uart_buffered_init(); /* printf must never stall the emulation */
#endif
    uint32_t hz = clock_get_hz(clk_sys);
    printf("sys clock %lu Hz, peri %lu Hz\n", (unsigned long)hz, (unsigned long)clock_get_hz(clk_peri));
    return hz;
}

/* ------------------------------------------------------------------ */
/* PSRAM                                                               */
/* ------------------------------------------------------------------ */

extern uint8_t __psram_data_start__, __psram_data_end__, __psram_data_load__;
extern uint8_t __psram_bss_start__, __psram_bss_end__;
extern void ecat_heap_init(void);

unsigned platform_init_psram(void) {
    uint pin = get_psram_pin();
    psram_init(pin);

    size_t n = (size_t)(&__psram_data_end__ - &__psram_data_start__);
    if (n) memcpy(&__psram_data_start__, &__psram_data_load__, n);
    n = (size_t)(&__psram_bss_end__ - &__psram_bss_start__);
    if (n) memset(&__psram_bss_start__, 0, n);

    ecat_heap_init();
    printf("PSRAM on CS%u, %u KB static\n", pin,
           (unsigned)((&__psram_bss_end__ - &__psram_data_start__) / 1024));
    return pin;
}

/* ------------------------------------------------------------------ */
/* USB host                                                            */
/* ------------------------------------------------------------------ */

#if USB_HID_ENABLED
extern void usbhid_init(void);
extern void usbhid_task(void);
#else
static inline void usbhid_task(void) {}
#endif

/* The host task runs on the main loop, once per frame (platform_usb_poll),
 * not in a timer interrupt: TinyUSB enumerates with blocking
 * delays, and something on the port that never enumerates (another host,
 * e.g. a PC on the USB-C port) kept it looping inside the interrupt, which
 * froze core 0 for good. A one-shot alarm bounds each call: if one blocks
 * for over a second the USB interrupt is switched off, the call drains and
 * returns, and the host stays off until the next boot. */
static volatile bool usb_dead;
static alarm_id_t usb_guard;

static int64_t usb_guard_cb(alarm_id_t id, void *user) {
    (void)id;
    (void)user;
    usb_dead = true;
    irq_set_enabled(USBCTRL_IRQ, false);
    return 0;
}

void platform_init_usb(void) {
#if USB_HID_ENABLED
    usbhid_init();
#endif
}

void platform_usb_poll(void) {
#if !USB_HID_ENABLED
    return;
#endif
    if (usb_dead) return;
    usb_guard = add_alarm_in_ms(1000, usb_guard_cb, NULL, true);
    usbhid_task();
    if (usb_guard > 0) cancel_alarm(usb_guard);
    if (usb_dead) printf("USB host: device on the port does not enumerate, host disabled\n");
}

/* ------------------------------------------------------------------ */
/* SD card                                                             */
/* ------------------------------------------------------------------ */

int platform_mount_sd(void) {
    static FATFS fs;
    FRESULT fr = FR_NOT_READY;
    for (int attempt = 0; attempt < 6 && fr != FR_OK; attempt++) {
        fr = f_mount(&fs, "", 1);
        if (fr != FR_OK) sleep_ms(50 << attempt);
    }
    printf("SD mount: %d\n", fr);
    return fr;
}

void platform_cyccnt_enable(void) {
    *(volatile uint32_t *)0xE000EDFC |= (1u << 24); /* DEMCR.TRCENA */
    *(volatile uint32_t *)0xE0001004 = 0;           /* CYCCNT */
    *(volatile uint32_t *)0xE0001000 |= 1u;         /* DWT_CTRL.CYCCNTENA */
}

/* Core 1's stack lives in main RAM, not SCRATCH_X: the HDMI line IRQ's code
 * takes most of that bank (PICO_CORE1_STACK_SIZE=0 reserves nothing there). */
#define CORE1_STACK_WORDS 1024
static uint32_t core1_stack[CORE1_STACK_WORDS];

void platform_launch_core1(void (*entry)(void)) {
    multicore_launch_core1_with_stack(entry, core1_stack, sizeof(core1_stack));
}

/* ------------------------------------------------------------------ */
/* Hang recovery: watchdog, breadcrumbs, safe mode                     */
/* ------------------------------------------------------------------ */


void platform_crumb(uint32_t stage, uint32_t arg) {
    crumbs[0] = CRUMB_MAGIC;
    crumbs[1] = stage;
    crumbs[2] = arg;
}

void platform_crumb_tick(void) {
    crumbs[3]++;
}

static bool watchdog_on;

void platform_watchdog_start(uint32_t ms) {
    watchdog_enable(ms, true);   /* paused while a debugger halts the core */
    watchdog_on = true;
}

void platform_watchdog_feed(void) {
    if (watchdog_on) watchdog_update();
}

/* Stall watcher: TIMER1's alarm 0, every 100 ms, a raw handler that reads the
 * interrupted PC and LR off the exception frame. When the main loop counter
 * (crumbs[3]) has not moved for 2 s, the sample is kept for safe mode to
 * report (addr2line turns it into a source line). TIMER1 is unused by the SDK,
 * whose alarm pool runs on TIMER0. */
#define STALL_IRQ   TIMER1_IRQ_0
#define STALL_US    100000u
static uint32_t stall_seen_tick, stall_count;

/* The same tick, faster, is a sampling profiler (platform_prof_start): the
 * interrupted PCs of core 0 into a buffer, dumped over the console */
#define PROF_SAMPLES 4096
static uint32_t *prof_buf;
static volatile uint32_t prof_n;
static volatile uint32_t sample_us = STALL_US;
static uint32_t stall_div, stall_acc;

void __attribute__((used)) stall_isr_c(uint32_t *frame) {
    timer1_hw->intr = 1u;                              /* acknowledge alarm 0 */
    timer1_hw->alarm[0] = timer1_hw->timerawl + sample_us;
    if (prof_buf && prof_n < PROF_SAMPLES) prof_buf[prof_n++] = frame[6];
    if (stall_div > 1 && ++stall_acc < stall_div) return;   /* the stall check stays at 100 ms */
    stall_acc = 0;
    if (crumbs[3] != stall_seen_tick) {
        stall_seen_tick = crumbs[3];
        stall_count = 0;
        return;
    }
    if (++stall_count == 20) {                         /* 2 s without a loop */
        stall_pc[0] = 0x57A11000u;
        stall_pc[1] = frame[6];                        /* PC */
        stall_pc[2] = frame[5];                        /* LR */
    }
}

static void __attribute__((naked)) stall_isr(void) {
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "b stall_isr_c\n");
}

void platform_prof_start(uint32_t us) {
    if (!prof_buf) prof_buf = (uint32_t *)ecat_sram_malloc(PROF_SAMPLES * sizeof(uint32_t));
    if (!prof_buf) return;
    prof_n = 0;
    stall_div = STALL_US / us;
    sample_us = us;
    printf("profiling: %u samples every %lu us\n", PROF_SAMPLES, (unsigned long)us);
}

/* the samples as hex PCs, 8 a line, then back to the plain stall watch */
bool platform_prof_dump_if_done(void) {
    if (!prof_buf || prof_n != PROF_SAMPLES) return false;   /* not full, or dumped */
    sample_us = STALL_US;
    stall_div = 0;
    printf("PROF BEGIN %u\n", PROF_SAMPLES);
    for (uint32_t i = 0; i < PROF_SAMPLES; i += 8) {
        for (uint32_t k = 0; k < 8; k++) printf("%08lx ", (unsigned long)prof_buf[i + k]);
        printf("\n");
        platform_watchdog_feed();
    }
    printf("PROF END\n");
    prof_n = PROF_SAMPLES + 1;   /* done until the next start */
    return true;
}

void platform_stall_watch_start(void) {
    stall_pc[0] = 0;
    stall_seen_tick = crumbs[3];
    irq_set_exclusive_handler(STALL_IRQ, stall_isr);
    irq_set_priority(STALL_IRQ, 0x40);
    timer1_hw->alarm[0] = timer1_hw->timerawl + STALL_US;
    timer1_hw->inte |= 1u;
    irq_set_enabled(STALL_IRQ, true);
}

/* After a watchdog reboot (a hang): say where it hung, over and over for
 * ~8 s so a console that opens late still sees it, then enter BOOTSEL, so the
 * board can be reflashed with nobody at it. Returns at once otherwise. */
void platform_safe_mode_if_hung(void) {
    if (!watchdog_caused_reboot() || crumbs[0] != CRUMB_MAGIC) {
        crumbs[0] = 0;
        crumbs[3] = 0;
        return;
    }
    const uint32_t stage = crumbs[1], arg = crumbs[2], ticks = crumbs[3];
    const bool have_pc = stall_pc[0] == 0x57A11000u;
    const uint32_t pc = stall_pc[1], lr = stall_pc[2];
    crumbs[0] = 0;
    stall_pc[0] = 0;
    for (int i = 0; i < 16; i++) {
        printf("*** SAFE MODE: the last run hung at stage %lu (arg 0x%08lx) after %lu main loops ***\n",
               (unsigned long)stage, (unsigned long)arg, (unsigned long)ticks);
        if (have_pc) printf("*** stalled at PC=0x%08lx LR=0x%08lx ***\n", (unsigned long)pc, (unsigned long)lr);
        printf("*** entering BOOTSEL in %d s ***\n", (16 - i) / 2);
        sleep_ms(500);
    }
    platform_reboot(true);
}
