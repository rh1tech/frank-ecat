/*
 * frank-ecat — eCat3 on RP2350 (FRANK M2)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * HID usage (USB HID Usage Tables, keyboard page 0x07) -> EmuKey. The same
 * keys as eCat3's browser build (src/wasm/ecat_wasm.js, CODE_TO_EMUKEY), the
 * keypad with the keypad bit.
 */

#include "frank_keys.h"

#define KEYPAD 0x20000000

int frank_hid_to_emukey(uint8_t u)
{
    if (u >= 0x04 && u <= 0x1D) return 'A' + (u - 0x04);           /* A..Z */
    if (u >= 0x1E && u <= 0x26) return '1' + (u - 0x1E);           /* 1..9 */
    if (u >= 0x3A && u <= 0x45) return 0x01000030 + (u - 0x3A);    /* F1..F12 */
    if (u >= 0x59 && u <= 0x61) return ('1' + (u - 0x59)) | KEYPAD; /* keypad 1..9 */
    switch (u) {
    case 0x27: return '0';
    case 0x28: return EMUKEY_RETURN;
    case 0x29: return EMUKEY_ESCAPE;
    case 0x2A: return 0x01000003;            /* Backspace */
    case 0x2B: return 0x01000001;            /* Tab */
    case 0x2C: return ' ';
    case 0x2D: return '-';
    case 0x2E: return '=';
    case 0x2F: return '[';
    case 0x30: return ']';
    case 0x31: return '\\';
    case 0x33: return ';';
    case 0x34: return '\'';
    case 0x35: return '`';
    case 0x36: return ',';
    case 0x37: return '.';
    case 0x38: return '/';
    case 0x39: return 0x01000024;            /* CapsLock */
    case 0x47: return 0x01000026;            /* ScrollLock */
    case 0x48: return 0x01000008;            /* Pause */
    case 0x49: return 0x01000006;            /* Insert */
    case 0x4A: return 0x01000010;            /* Home */
    case 0x4B: return 0x01000016;            /* PageUp */
    case 0x4C: return 0x01000007;            /* Delete */
    case 0x4D: return 0x01000011;            /* End */
    case 0x4E: return 0x01000017;            /* PageDown */
    case 0x4F: return 0x01000014;            /* Right */
    case 0x50: return 0x01000012;            /* Left */
    case 0x51: return EMUKEY_DOWN;
    case 0x52: return EMUKEY_UP;
    case 0x53: return 0x01000025;            /* NumLock */
    case 0x54: return '/' | KEYPAD;
    case 0x55: return '*' | KEYPAD;
    case 0x56: return '-' | KEYPAD;
    case 0x57: return '+' | KEYPAD;
    case 0x58: return 0x01000005;            /* keypad Enter */
    case 0x62: return '0' | KEYPAD;
    case 0x63: return '.' | KEYPAD;
    case 0xE0: return EMUKEY_CONTROL;        /* the driver's pseudo usages */
    case 0xE1: return EMUKEY_SHIFT;
    case 0xE2: return EMUKEY_ALT;
    default: return 0;
    }
}
