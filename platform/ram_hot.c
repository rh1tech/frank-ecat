/*
 * frank-ecat — eCat3 on RP2350 (FRANK M2)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copies the hot functions (.ram_hot, memmap_ecat.ld) from flash to SRAM
 * before anything can call them: from .preinit_array, which runs before the
 * C++ constructors.
 */

#include <stdint.h>
#include <string.h>

extern uint8_t __ram_hot_start__[], __ram_hot_end__[], __ram_hot_source__[];

static void ram_hot_copy(void) {
    memcpy(__ram_hot_start__, __ram_hot_source__, (size_t)(__ram_hot_end__ - __ram_hot_start__));
}

__attribute__((used, section(".preinit_array"))) static void (*const ram_hot_init)(void) = ram_hot_copy;
