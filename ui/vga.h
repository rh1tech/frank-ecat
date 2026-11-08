/*
 * frank-ecat — eCat3 on RP2350 (FRANK M2)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The menus' surface, under the name frank-dino's UI knows it by.
 *
 * There is no second framebuffer: the menu draws into the machine's own
 * surface, at 320x240, after its picture has been put aside in the PSRAM,
 * and the picture comes back when the menu closes. The HDMI driver keeps
 * scanning the same buffer and only its layout changes (ecat_main.cpp).
 *
 * The UI's colours are palette indices UI_PAL_BASE and up, which the
 * machine's palette never reaches (IndexedPalette::SIZE).
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VGA_UI_W 320
#define VGA_UI_H 240
#define UI_PAL_BASE 224

typedef enum { VGA_MODE_MACHINE = 0, VGA_MODE_UI } vga_mode_t;

/* the back buffer the UI draws in, and the rows it has drawn (ui_gfx.c) */
extern uint8_t *vga_ui_fb;
extern int ui_dirty_lo, ui_dirty_hi;

/* The drawn rows to the screen, after the next vertical sync. Called while
 * a menu waits for a key, so a change appears in one piece. */
void vga_ui_present(void);

vga_mode_t vga_get_mode(void);
void       vga_set_mode(vga_mode_t m);
void       vga_build_ui_palette(void);

#ifdef __cplusplus
}
#endif
