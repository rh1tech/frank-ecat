/*
 * frank-ecat — eCat3 on RP2350 (FRANK M2)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "storage.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "ecat_ui.h"

bool storage_mounted(void) { return ecat_ui_card_mounted(); }

static int by_kind_then_name(const void *a, const void *b) {
    const storage_entry_t *x = a, *y = b;
    const int dx = x->kind == FILE_DIR, dy = y->kind == FILE_DIR;
    if (dx != dy) return dy - dx;
    return strcasecmp(x->name, y->name);
}

int storage_list(const char *path, storage_entry_t *out, const int max) {
    if (!storage_mounted()) return -1;
    FFDIR d;
    FILINFO fi;
    if (f_opendir(&d, path) != FR_OK) return -1;
    int n = 0;
    while (n < max && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fattrib & (AM_HID | AM_SYS)) continue;
        if (fi.fname[0] == '.') continue;           /* ._ files a Mac leaves */
        storage_entry_t *e = &out[n];
        strncpy(e->name, fi.fname, sizeof e->name - 1);
        e->name[sizeof e->name - 1] = 0;
        e->size = (uint32_t)fi.fsize;
        e->kind = (fi.fattrib & AM_DIR) ? FILE_DIR : (file_kind_t)ecat_ui_file_kind(e->name);
        n++;
    }
    f_closedir(&d);
    qsort(out, (size_t)n, sizeof out[0], by_kind_then_name);
    return n;
}
