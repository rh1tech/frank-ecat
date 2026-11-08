/*
 * frank-ecat — eCat3 on RP2350 (FRANK M2)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The HDMI driver's framebuffer byte order in its own (Spectrum-style) modes
 * (pico-speccy, from ESPectrum's ESP32 VGA driver): inside each group of 4
 * pixels, pixel x is stored at byte x ^ 2, i.e. a 32-bit word holds pixels
 * 2,3,0,1. The line IRQ undoes it with a 16-bit rotate per word. A layout
 * (hdmi_set_layout(), the emulator's surface) is in plain order instead.
 */

#ifndef FRANK_FB_H
#define FRANK_FB_H

#include <stdint.h>

/* byte offset of pixel x in a row */
#define FRANK_FB_X(x) ((x) ^ 2)

/* n pixels (n a multiple of 4) in plain order -> a row in the driver's order */
static inline void frank_fb_store_row(uint8_t *dst, const uint8_t *src, int n) {
    const uint32_t *s = (const uint32_t *)src;
    uint32_t *d = (uint32_t *)dst;
    for (int i = 0; i < (n >> 2); i++) {
        uint32_t v = s[i];
        d[i] = (v >> 16) | (v << 16);
    }
}

#endif /* FRANK_FB_H */
