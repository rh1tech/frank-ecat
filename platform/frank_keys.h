/*
 * frank-ecat — eCat3 on RP2350 (FRANK M2)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The USB keyboard: HID usages to eCat3's EmuKey codes (the Qt::Key values),
 * by the position of the key, as the browser build maps KeyboardEvent.code -
 * so a Russian machine's ЙЦУКЕН layout sits where it is printed on the
 * original, whatever the host layout.
 */

#ifndef FRANK_KEYS_H
#define FRANK_KEYS_H

#include <stdint.h>

/* EmuKey code of a HID usage (and the pseudo usages 0xE0..0xE2 the USB driver
 * gives Ctrl/Shift/Alt), 0 for a key the machines do not have */
int frank_hid_to_emukey(uint8_t usage);

/* EmuKey codes the frontend uses */
#define EMUKEY_RETURN   0x01000004
#define EMUKEY_ESCAPE   0x01000000
#define EMUKEY_UP       0x01000013
#define EMUKEY_DOWN     0x01000015
#define EMUKEY_F11      0x0100003a
#define EMUKEY_F12      0x0100003b
#define EMUKEY_SHIFT    0x01000020
#define EMUKEY_CONTROL  0x01000021
#define EMUKEY_ALT      0x01000023

#define EMUMOD_SHIFT    0x02000000
#define EMUMOD_CONTROL  0x04000000
#define EMUMOD_ALT      0x08000000

#endif /* FRANK_KEYS_H */
