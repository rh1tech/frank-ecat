/*
 * frank-ecat — eCat3 on RP2350 (FRANK M2)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * From frank-dino src/ui/menu.c: the machine is "paused" by the main loop
 * being in here instead of slicing it, and the waits keep the board
 * serviced through ecat_ui_poll().
 */

/*
 * The way in and out of the on-screen UI, and the keys that feed it.
 *
 * Everything that draws now lives in ui_browser.c and ui_nav.c, ported
 * from pico-speccy. What is left here is the part that is about this
 * board: pausing the machine around a modal that reads the card, and a
 * queue for keys that arrive from a USB callback rather than from stdin.
 */

#include "menu.h"

#include <stdio.h>
#include <string.h>

#include <pico/stdlib.h>

#include "ecat_ui.h"
#include "ui_browser.h"
#include "ui_nav.h"
#include "vga.h"
#include "ui_render.h"

static bool open_;
static bool straight_to_settings;

/*
 * The settings menu asking for the listing to be read again.
 *
 * "Rescan" used to be a key in the browser. It belongs with the other
 * card-level actions, but the settings tree has no idea what a listing
 * is -- so it raises this and the browser acts on it.
 */
bool menu_rescan_wanted;

bool menu_is_open(void) { return open_; }
void menu_open_settings(const bool yes) { straight_to_settings = yes; }

/*
 * A small queue of keys that arrive from somewhere other than the console.
 *
 * While the UI is open it reads stdin itself, because it is modal: the
 * main loop is blocked inside menu_run() and cannot deliver anything.
 * The first version relied on it to, which deadlocked on the first
 * keypress -- the browser drew once and then waited for ever for a key
 * that only the blocked loop could have handed it.
 *
 * The queue is for the USB keyboard, which arrives through a TinyUSB
 * callback rather than through stdin. A queue and not a single slot,
 * because a cursor key is three bytes -- ESC, '[', then a letter -- and
 * a mailbox that holds one of them delivers the last and loses the shape.
 */
#define KEYQ_SIZE 16

static volatile uint8_t keyq[KEYQ_SIZE];
static volatile uint8_t keyq_head, keyq_tail;

void menu_key(const char c) {
    const uint8_t h = keyq_head;
    if ((uint8_t)(h - keyq_tail) >= KEYQ_SIZE) return;   // full; drop it
    keyq[h % KEYQ_SIZE] = (uint8_t)c;
    keyq_head = (uint8_t)(h + 1);
}

/*
 * The same wait, but it gives up.
 *
 * An escape sequence is a byte that means one thing on its own and
 * another when something follows it, and the only way to tell is to wait
 * a moment and see. Blocking for ever on the second byte is what turns a
 * bare Esc into a hang -- both menus press Esc a lot, so they read the
 * continuation through this and take the timeout as "there wasn't one".
 *
 * Returns -1 if `ms` passed with nothing to show for it.
 */
static absolute_time_t autoclose_at;
static bool autoclose_armed;
static int autoclose_shown = -1;

void menu_autoclose(const unsigned ms) {
    autoclose_armed = ms != 0;
    autoclose_shown = -1;
    autoclose_at = make_timeout_time_ms(ms);

}

static int silent_keys;             /* menu_preload()'s, which are nobody's */

void menu_preload(const char *keys) {
    for (; *keys; keys++) { menu_key(*keys); silent_keys++; }
}

/* The countdown, in the settings menu's footer, once a second. */

static void autoclose_tick(void) {
    const int64_t left = absolute_time_diff_us(get_absolute_time(), autoclose_at);
    const int s = (int)((left + 999999) / 1000000);
    if (s == autoclose_shown) return;
    autoclose_shown = s;
    char buf[64];
    snprintf(buf, sizeof buf, "Starting in %d s. Any key to stay.", s);
    ui_status(buf);
    ui_flush_dirty();
}

static int got_key(const int k) {
    if (silent_keys) { silent_keys--; return k; }
    if (autoclose_armed) {          /* somebody is there: stay */
        autoclose_armed = false;
        ui_status("");
    }
    return k;
}

int menu_wait_key_timeout(const unsigned ms) {
    const absolute_time_t deadline = make_timeout_time_ms(ms);
    for (;;) {
        if (autoclose_armed && absolute_time_diff_us(get_absolute_time(), autoclose_at) <= 0) {
            autoclose_armed = false;
            return '\t';           /* the menu's own close key */
        }
        if (autoclose_armed && !silent_keys) autoclose_tick();
        ecat_ui_poll();
        if (keyq_tail != keyq_head) {
            const uint8_t k = keyq[keyq_tail % KEYQ_SIZE];
            keyq_tail = (uint8_t)(keyq_tail + 1);
            return got_key(k);
        }
        const int c = ecat_ui_console_getchar();
        if (c >= 0) return c;       /* the debug console: not somebody at the board */
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) return -1;
        sleep_ms(1);
    }
}

int menu_wait_key(void) {
    /*
     * Keep the DMA fed while waiting. The machine is paused so nothing
     * is producing samples, but the blocks still drain and still have to
     * be written, or the carrier is left playing whatever was in them.
     *
     * And keep the host stack running: this is modal, the main loop is
     * inside it, so nothing else is calling tuh_task(). Without that the
     * keyboard's reports stop being collected the moment the UI opens,
     * and it waits for ever for a key it has made itself unable to
     * receive. Both live in menu_wait_key_timeout().
     */
    for (;;) {
        const int c = menu_wait_key_timeout(1000);
        if (c >= 0) return c;       /* the debug console: not somebody at the board */
    }
}

bool menu_run(void) {
    open_ = true;
    vga_set_mode(VGA_MODE_UI);      /* before anything is drawn over the picture */

    bool loaded = false;
    if (straight_to_settings) {
        straight_to_settings = false;
        ui_menu_run();
        if (menu_rescan_wanted) { menu_rescan_wanted = false; ui_browser_rescan(); }
    } else {
        loaded = ui_browser_run();
    }

    vga_set_mode(VGA_MODE_MACHINE);
    open_ = false;
    return loaded;
}
