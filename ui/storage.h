/*
 * frank-ecat — eCat3 on RP2350 (FRANK M2)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The card, as the file browser sees it: a directory read into an array,
 * folders first, then files, each by name.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_MAX_PATH 160
#define STORAGE_MAX_NAME 64

typedef enum {
    FILE_OTHER = 0,
    FILE_DIR,
    FILE_PROGRAM,       /* loaded straight into memory (eCat3's HandleExternalFile) */
    FILE_TAPE,          /* put on the machine's tape recorder */
    FILE_DISK,          /* inserted into the machine's drive */
} file_kind_t;

typedef struct {
    char        name[STORAGE_MAX_NAME];
    uint32_t    size;
    file_kind_t kind;
} storage_entry_t;

bool storage_mounted(void);

/* Up to `max` entries of `path`; the count, or -1 when it cannot be read. */
int storage_list(const char *path, storage_entry_t *out, int max);

#ifdef __cplusplus
}
#endif
