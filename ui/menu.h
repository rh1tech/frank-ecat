/*
 * frank-ecat — eCat3 on RP2350 (FRANK M2), the menus from frank-dino
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The way in and out of the on-screen UI, and the keys that feed it.
 *
 * Modal and synchronous: menu_run() pauses the machine, hands the screen
 * to the ported browser or settings menu, and resumes when they are
 * done: the main loop is in here, so the machine is not sliced, and a
 * file loaded rewrites memory the CPU is not running from.
 *
 * Runs on core 0, which owns the screen, the card and the console.
 */
#pragma once

#include <stdbool.h>

/*
 * Open the menu and stay in it until the user leaves.
 *
 * Returns true if something was loaded, so the caller knows the machine
 * it resumes is not the machine it paused.
 */
bool menu_run(void);

/*
 * Make the next menu_run() open the settings page at once.
 *
 * F12 means "settings" and F11 means "files", and the settings hang off
 * the browser -- so this is how the one becomes the other without a
 * second modal loop.
 */
void menu_open_settings(bool yes);

/* Feed a console byte to the menu while it is open. */
void menu_key(char c);

/* Whether the menu currently has the screen and the keyboard. */
bool menu_is_open(void);

/*
 * Block until a key arrives, from the console or the mailbox.
 *
 * Shared with the settings page, which is modal in the same way and for
 * the same reason -- and which must keep the audio DMA fed while it
 * waits, or the last block in the ring plays on a loop. See the
 * definition for why this reads stdin rather than waiting on the
 * mailbox alone.
 */
int menu_wait_key(void);

/* Close the menu by itself after `ms` unless a key comes first (0: never).
 * The power-on model list, so a board nobody is at still starts. */
void menu_autoclose(unsigned ms);

/* Keys to act on first, as if typed, but not counting as somebody there. */
void menu_preload(const char *keys);

/* The same, giving up after `ms` and returning -1. */
int menu_wait_key_timeout(unsigned ms);

/*
 * Set by the settings page's "Re-read the card" action.
 *
 * The page knows nothing about directory listings; the browser does, and
 * picks this up when the page closes.
 */
extern bool menu_rescan_wanted;
