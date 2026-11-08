/*
 * frank-ecat — eCat3 on RP2350 (FRANK M2)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Hardware bring-up firmware: clocks, PSRAM, SD card, the PIO HDMI output
 * in each video mode the emulator may use, HDMI and I2S sound.
 *
 * Console commands (one key each):
 *   0..9  video mode from diag_modes[] (stored, then reboot: a mode change
 *         reprograms the PIO/DMA, as in pico-speccy)
 *   h/i/o sound to HDMI / I2S / off      t  tone on/off
 *   a     HDMI audio (data islands) on/off, reboot
 *   x     the machine's other resolution (live layout change)
 *   p     PSRAM test                     s  SD card root listing
 *   ~BOOT~ reboot to BOOTSEL             ~RST~ reboot
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/structs/io_bank0.h"
#include "hardware/structs/pads_bank0.h"

#include "platform.h"
#include "frank_audio.h"
#include "graphics.h"
#include "hdmi.h"
#include "font8x8.h"
#include "frank_fb.h"
#include "ff.h"

/* Each case: an output mode (graphics/video_mode_table.h, HSTX at 504 MHz), the
 * machine's refresh (Hz x 100, 0 = the table's), and its framebuffer as the eCat
 * display produces it, laid out by hdmi_set_layout() (×hs across, ×vs down,
 * centred). alt_*: the machine's other resolution, switched to live with 'x'. */
typedef struct { int w, h, hs, vs; } diag_layout_t;
static const struct {
    int mode, hz;
    diag_layout_t lay, alt;
    const char *name;
} diag_modes[] = {
    { 0,  5992, { 560, 192, 1, 2 }, { 0 },              "Pravetz-8C 560x192 640x480@60" },
    { 2,  5008, { 560, 192, 1, 2 }, { 240, 224, 2, 2 }, "Pravetz 560x192 / Oric 240x224 640x480@50" },
    { 2,  5000, { 320, 240, 2, 2 }, { 640, 240, 1, 2 }, "Irisha 320/640x240 640x480@50" },
    { 39, 5000, { 384, 256, 2, 2 }, { 0 },              "Orion 384x256 800x600@50" },
    { 28, 5001, { 468, 300, 2, 2 }, { 0 },              "RK86 468x300 1024x768@50" },
    { 28, 5000, { 512, 256, 2, 3 }, { 280, 256, 3, 3 }, "Agat 512x256 / 280x256 1024x768@50" },
    { 28, 5060, { 480, 250, 2, 3 }, { 480, 256, 2, 3 }, "Unior 480x250 / 480x256 1024x768@50" },
    { 28, 5297, { 480, 250, 2, 3 }, { 320, 256, 3, 3 }, "Argo 480x250 / ZX 320x256 1024x768@53" },
    { 0,  0,    { 320, 240, 0, 0 }, { 0 },              "640x480@60 Spectrum scanout 320x240" },
};
static bool alt_on;
#define N_MODES ((int)(sizeof(diag_modes) / sizeof(diag_modes[0])))

/* survives the watchdog reboot of a mode change */
static uint32_t __attribute__((section(".uninitialized_data"))) saved_mode[3];
#define SAVED_MAGIC 0xEC47D1A6u

static int cur;
#define FB_W 640
#define FB_H 300
static uint8_t fb[FB_W * FB_H];
static int fb_w, fb_h;
static volatile uint32_t vsyncs;

/* ---- the HDMI driver's hooks ---- */

uint8_t *__not_in_flash_func(getLineBuffer)(int line) {
    return fb + line * fb_w;
}

int __not_in_flash_func(get_video_mode)(void) {
    return diag_modes[cur].mode;
}

void __not_in_flash_func(ESPectrum_vsync)(void) {
    vsyncs++;
}

/* ---- test picture ---- */

/* every write goes through here: a layout's rows are in plain order, the
 * driver's own (Spectrum-style) scanout wants x^2 order */
static bool fb_plain;
static inline void pset(int x, int y, uint8_t c) {
    fb[y * fb_w + (fb_plain ? x : FRANK_FB_X(x))] = c;
}

static void put_char(int x, int y, char c, uint8_t fg, uint8_t bg) {
    const uint8_t *g = &font_8x8[(uint8_t)c * 8];
    for (int r = 0; r < 8; r++)
        for (int b = 0; b < 8; b++)
            if (x + b < fb_w && y + r < fb_h) pset(x + b, y + r, ((g[r] >> b) & 1) ? fg : bg);
}

static void put_str(int x, int y, const char *s, uint8_t fg, uint8_t bg) {
    for (; *s; s++, x += 8) put_char(x, y, *s, fg, bg);
}

/* palette: 0..15 the CGA colours, 16..79 a grey ramp, 80..143 R/G/B ramps
 * (indices from 184 up are the driver's while HDMI audio is on) */
static void make_palette(void) {
    for (int i = 0; i < 16; i++) {
        int lo = (i & 8) ? 0x55 : 0;
        int r = ((i & 4) ? 0xAA : 0) + lo, g = ((i & 2) ? 0xAA : 0) + lo, b = ((i & 1) ? 0xAA : 0) + lo;
        if (i == 6) g = 0x55; /* brown */
        graphics_set_palette(i, (uint32_t)r << 16 | g << 8 | b);
    }
    for (int i = 0; i < 64; i++) {
        int v = i * 255 / 63;
        graphics_set_palette(16 + i, (uint32_t)v << 16 | v << 8 | v);
    }
    for (int i = 0; i < 21; i++) {
        int v = i * 255 / 20;
        graphics_set_palette(80 + i, (uint32_t)v << 16);
        graphics_set_palette(101 + i, (uint32_t)v << 8);
        graphics_set_palette(122 + i, (uint32_t)v);
    }
}

static int status_y; /* first row of the status lines */

static void fill_rect(int x0, int y0, int w, int h, uint8_t c) {
    for (int y = y0; y < y0 + h && y < fb_h; y++)
        for (int x = x0; x < x0 + w && x < fb_w; x++) pset(x, y, c);
}

/* Laid out in proportion to the source size (192..300 rows, 240..640 wide):
 * title, 16 colour bars, grey and R/G/B ramps, a 1-pixel checkerboard and
 * 1-pixel stripes (lost or doubled pixels show at once), a white frame on the
 * outermost pixels, status lines. */
static void draw_picture(void) {
    memset(fb, 0, sizeof(fb));
    for (int x = 0; x < fb_w; x++) {
        pset(x, 0, 15);
        pset(x, fb_h - 1, 15);
    }
    for (int y = 0; y < fb_h; y++) {
        pset(0, y, 15);
        pset(fb_w - 1, y, 15);
    }
    const int m = 4;                    /* margin inside the frame */
    const int iw = fb_w - 2 * m;
    int y = 3;
    char line[64];
    put_str(m, y, "frank-ecat diag " ECAT_VERSION, 14, 0);
    y += 10;
    snprintf(line, sizeof(line), "%d: %s", cur, diag_modes[cur].name);
    put_str(m, y, line, 15, 0);
    y += 12;
    const int bars_h = fb_h / 5;
    for (int i = 0; i < 16; i++) {
        int x0 = m + i * iw / 16, x1 = m + (i + 1) * iw / 16;
        fill_rect(x0, y, x1 - x0, bars_h, (uint8_t)i);
    }
    y += bars_h + 4;
    const int ramp_h = fb_h / 24 + 2;
    for (int x = 0; x < iw; x++) {
        fill_rect(m + x, y, 1, ramp_h, (uint8_t)(16 + x * 64 / iw));
        fill_rect(m + x, y + ramp_h, 1, ramp_h / 2 + 1, (uint8_t)(80 + x * 21 / iw));
        fill_rect(m + x, y + ramp_h + ramp_h / 2 + 1, 1, ramp_h / 2 + 1, (uint8_t)(101 + x * 21 / iw));
        fill_rect(m + x, y + 2 * (ramp_h / 2 + 1) + ramp_h, 1, ramp_h / 2 + 1, (uint8_t)(122 + x * 21 / iw));
    }
    y += ramp_h + 3 * (ramp_h / 2 + 1) + 4;
    const int pat_h = fb_h / 10;
    for (int yy = y; yy < y + pat_h; yy++) {
        for (int x = m; x < fb_w / 3; x++) pset(x, yy, ((x ^ yy) & 1) ? 15 : 0);
        for (int x = fb_w / 3; x < 2 * fb_w / 3; x++) pset(x, yy, (x & 1) ? 15 : 1);
        for (int x = 2 * fb_w / 3; x < fb_w - m; x++) pset(x, yy, (yy & 1) ? 14 : 4);
    }
    y += pat_h + 3;
    snprintf(line, sizeof(line), "fb %dx%d", fb_w, fb_h);
    put_str(m, y, line, 7, 0);
    status_y = y + 10;
}

static void draw_status(void) {
    char line[48];
    snprintf(line, sizeof(line), "vsync %-8lu audio %-4s ring %4d", (unsigned long)vsyncs,
             frank_audio_output_name(frank_audio_output()), frank_audio_level());
    put_str(4, status_y, line, 10, 0);
    snprintf(line, sizeof(line), "underruns %-8lu", (unsigned long)frank_audio_underruns);
    put_str(4, status_y + 10, line, 10, 0);
}

/* ---- core 1: the HDMI line IRQ ---- */

static void core1_main(void) {
    graphics_init();
    graphics_set_buffer(NULL, fb_w, fb_h);
    graphics_set_bgcolor(0x000000);
    multicore_fifo_push_blocking(1);
    while (1) tight_loop_contents();
}

/* ---- sound: a two-tone test signal ---- */

static bool tone_on = true;

static void tone_fill(void) {
    static int16_t buf[2 * 256];
    static float ph_l, ph_r;
    while (frank_audio_free() >= 256) {
        for (int i = 0; i < 256; i++) {
            int16_t l = tone_on ? (int16_t)(sinf(ph_l) * 6000) : 0;
            int16_t r = tone_on ? (int16_t)(sinf(ph_r) * 6000) : 0;
            ph_l += 2 * (float)M_PI * 1000.0f / FRANK_AUDIO_RATE;
            ph_r += 2 * (float)M_PI * 1000.0f / FRANK_AUDIO_RATE;
            if (ph_l > 2 * (float)M_PI) ph_l -= 2 * (float)M_PI;
            if (ph_r > 2 * (float)M_PI) ph_r -= 2 * (float)M_PI;
            buf[2 * i] = l;
            buf[2 * i + 1] = r;
        }
        frank_audio_push(buf, 256);
    }
}

/* ---- video hardware state ---- */

static void video_dump(void) {
    printf("clk_sys %lu Hz\n", (unsigned long)clock_get_hz(clk_sys));
    for (int pin = 12; pin <= 19; pin++)
        printf("gpio%d funcsel %lu pad 0x%02lx out %d\n", pin,
               (unsigned long)(io_bank0_hw->io[pin].ctrl & 0x1f), (unsigned long)pads_bank0_hw->io[pin],
               (int)gpio_get_out_level(pin));
    PIO pios[3] = { pio0, pio1, pio2 };
    for (int p = 0; p < 3; p++) {
        printf("pio%d ctrl 0x%08lx fstat 0x%08lx fdebug 0x%08lx gpiobase %lu\n", p,
               (unsigned long)pios[p]->ctrl, (unsigned long)pios[p]->fstat, (unsigned long)pios[p]->fdebug,
               (unsigned long)pios[p]->gpiobase);
        for (int sm = 0; sm < 4; sm++)
            if (pios[p]->ctrl & (1u << sm))
                printf("  sm%d clkdiv 0x%08lx pc %lu pinctrl 0x%08lx execctrl 0x%08lx\n", sm,
                       (unsigned long)pios[p]->sm[sm].clkdiv, (unsigned long)pios[p]->sm[sm].addr,
                       (unsigned long)pios[p]->sm[sm].pinctrl, (unsigned long)pios[p]->sm[sm].execctrl);
        pios[p]->fdebug = 0xffffffff;
    }
    for (int ch = 0; ch < 8; ch++)
        printf("dma%d ctrl 0x%08lx count %lu read 0x%08lx write 0x%08lx\n", ch,
               (unsigned long)dma_hw->ch[ch].ctrl_trig, (unsigned long)dma_hw->ch[ch].transfer_count,
               (unsigned long)dma_hw->ch[ch].read_addr, (unsigned long)dma_hw->ch[ch].write_addr);
}

/* ---- PSRAM / SD tests ---- */

static void psram_test(void) {
    const uint32_t n = 1024 * 1024; /* words: 4 MB */
    volatile uint32_t *p = (volatile uint32_t *)0x11200000u;
    uint32_t t0 = time_us_32();
    for (uint32_t i = 0; i < n; i++) p[i] = i * 2654435761u;
    uint32_t t1 = time_us_32();
    uint32_t bad = 0;
    for (uint32_t i = 0; i < n; i++)
        if (p[i] != i * 2654435761u) bad++;
    uint32_t t2 = time_us_32();
    printf("PSRAM 4 MB @0x11200000: %lu errors, write %lu KB/s, read %lu KB/s\n", (unsigned long)bad,
           (unsigned long)(4ull * 1024 * 1000000 / (t1 - t0)), (unsigned long)(4ull * 1024 * 1000000 / (t2 - t1)));
}

static void sd_list(void) {
    FFDIR d;
    FILINFO fi;
    if (f_opendir(&d, "/") != FR_OK) {
        printf("SD: cannot open /\n");
        return;
    }
    int n = 0;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0] && n < 40) {
        printf("  %s%s %lu\n", fi.fname, (fi.fattrib & AM_DIR) ? "/" : "", (unsigned long)fi.fsize);
        n++;
    }
    f_closedir(&d);
}

int main(void) {
    bool no_hdmi_audio = false;
    if (saved_mode[0] == SAVED_MAGIC && saved_mode[1] < N_MODES) {
        cur = (int)saved_mode[1];
        no_hdmi_audio = saved_mode[2] != 0;
    }
    saved_mode[0] = 0;

    platform_init_clocks();
#if ECAT_CONSOLE_USB
    /* give the host a moment to open the CDC port */
    for (int i = 0; i < 30 && !stdio_usb_connected(); i++) sleep_ms(100);
#endif
    platform_report_previous_crash();
    unsigned cs = platform_init_psram();
    (void)cs;
    ecat_heap_report();
    int fr = platform_mount_sd();
    if (fr == 0) sd_list();

    fb_w = diag_modes[cur].lay.w;
    fb_h = diag_modes[cur].lay.h;
    fb_plain = diag_modes[cur].lay.hs != 0;
    if (fb_plain) hdmi_set_layout(fb_w, fb_h, diag_modes[cur].lay.hs, diag_modes[cur].lay.vs);
    if (diag_modes[cur].hz) graphics_set_refresh(diag_modes[cur].mode, diag_modes[cur].hz);
    graphics_set_sys_clk_mhz(clock_get_hz(clk_sys) / 1000000);
    hdmi_lut_clear();
    make_palette();
    draw_picture();
    printf("video: mode %d [%d] %s\n", cur, diag_modes[cur].mode, diag_modes[cur].name);

    platform_launch_core1(core1_main);
    multicore_fifo_pop_blocking();
    printf("video up\n");

    if (no_hdmi_audio)
        printf("HDMI audio off (DVI signal)\n");
    else
        frank_audio_init(FRANK_AUDIO_HDMI);
    ecat_heap_report();

    uint32_t last = time_us_32();
    while (1) {
        tone_fill();
        int c = getchar_timeout_us(0);
        if (c >= 0) {
            platform_uart_cmd(c);
            if (c >= '0' && c <= '9' && c - '0' < N_MODES) {
                saved_mode[0] = SAVED_MAGIC;
                saved_mode[1] = (uint32_t)(c - '0');
                saved_mode[2] = no_hdmi_audio;
                printf("mode %c: %s, rebooting\n", c, diag_modes[c - '0'].name);
                sleep_ms(20);
                platform_reboot_clean(false);
            }
            switch (c) {
            case 'a': /* HDMI audio on/off: reboot into the same mode */
                saved_mode[0] = SAVED_MAGIC;
                saved_mode[1] = (uint32_t)cur;
                saved_mode[2] = !no_hdmi_audio;
                printf("HDMI audio %s, rebooting\n", no_hdmi_audio ? "on" : "off");
                sleep_ms(20);
                platform_reboot_clean(false);
                break;
            case 'x': { /* the machine's other resolution, live */
                const diag_layout_t *l = alt_on ? &diag_modes[cur].lay : &diag_modes[cur].alt;
                if (!l->w) break;
                alt_on = !alt_on;
                fb_w = l->w;
                fb_h = l->h;
                draw_picture();
                hdmi_set_layout(fb_w, fb_h, l->hs, l->vs);
                printf("layout %dx%d x%d/x%d\n", fb_w, fb_h, l->hs, l->vs);
                break;
            }
            case 'h': frank_audio_select(FRANK_AUDIO_HDMI); break;
            case 'i': frank_audio_select(FRANK_AUDIO_I2S); break;
            case 'o': frank_audio_select(FRANK_AUDIO_OFF); break;
            case 't': tone_on = !tone_on; printf("tone %s\n", tone_on ? "on" : "off"); break;
            case 'p': psram_test(); break;
            case 's': sd_list(); break;
            case 'm': ecat_heap_report(); break;
            case 'v': video_dump(); break;
            case 'r': { /* where the palette-fetch DMA (ch3) reads: page A, page B, else */
                unsigned na = 0, nb = 0, no = 0;
                uint32_t other = 0;
                for (int i = 0; i < 20000; i++) {
                    uint32_t a = dma_hw->ch[3].read_addr;
                    if ((a & 0xfffff000u) == 0x20000000u) na++;
                    else if ((a & 0xfffff000u) == 0x20081000u) nb++;
                    else { no++; other = a; }
                }
                printf("ch3 reads: page A %u, page B %u, other %u (last 0x%08lx)\n", na, nb, no, (unsigned long)other);
                break;
            }
            case 'l': /* the two DMA line buffers behind the palette (conv_color[1024..]), run-length */
                for (int b = 0; b < 2; b++) {
                    const uint8_t *lb = (const uint8_t *)0x20001000u + b * 400;
                    for (int k = 0; k < 2; k++) {
                        uint8_t snap[400];
                        memcpy(snap, lb, sizeof(snap));
                        printf("buf%d:", b);
                        int i = 0;
                        while (i < 400) {
                            int j = i;
                            while (j < 400 && snap[j] == snap[i]) j++;
                            printf(" %d@%dx%d", snap[i], i, j - i);
                            i = j;
                        }
                        printf("\n");
                        sleep_us(15873); /* half a line pair later */
                    }
                }
                break;
            case 'w': { /* palette slots: page A (conv_color), page B (conv_color_b) */
                static const int slots[] = { 0, 1, 15, 216, 217, 218, 219, 220, 240, 241, 242, 243, 244, 255 };
                for (unsigned k = 0; k < sizeof(slots) / sizeof(slots[0]); k++) {
                    const volatile uint32_t *a = (const volatile uint32_t *)0x20000000u + slots[k] * 4;
                    const volatile uint32_t *b = (const volatile uint32_t *)0x20081000u + slots[k] * 4;
                    printf("slot %3d A %08lx%08lx %08lx%08lx  B %08lx%08lx %08lx%08lx\n", slots[k],
                           (unsigned long)a[1], (unsigned long)a[0], (unsigned long)a[3], (unsigned long)a[2],
                           (unsigned long)b[1], (unsigned long)b[0], (unsigned long)b[3], (unsigned long)b[2]);
                }
                break;
            }
            }
        }
        uint32_t now = time_us_32();
        if (now - last >= 1000000) {
            last = now;
            draw_status();
            static uint32_t last_vs, last_pops;
            uint32_t pops = 0, credit = 0, cap = 0;
            hdmi_audio_meter(&pops, &credit, &cap);
            printf("vsync %lu (%lu Hz) ring %d underruns %lu, audio packets %lu/s\n", (unsigned long)vsyncs,
                   (unsigned long)(vsyncs - last_vs), frank_audio_level(), (unsigned long)frank_audio_underruns,
                   (unsigned long)(pops - last_pops));
            last_vs = vsyncs;
            last_pops = pops;
            hdmi_audio_health_dump();
        }
    }
}
