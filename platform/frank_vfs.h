/*
 * frank-ecat — eCat3 on RP2350 (FRANK M2)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The read-only file system in flash (tools/embed_vfs.py) and the files of the
 * C library on top of it and the SD card (frank_fs.c): open() of a path that is
 * in the VFS reads it from flash, any other path (and any write) goes to the
 * card through FatFs. So std::ifstream, fopen() & co in the eCat3 core work
 * unchanged.
 */

#ifndef FRANK_VFS_H
#define FRANK_VFS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *path;       /* "/ecat/computers/radio-86rk/Radio-86rk.cfg" */
    const uint8_t *data;
    uint32_t size;
} ecat_vfs_entry_t;

extern const ecat_vfs_entry_t ecat_vfs[];
extern const unsigned ecat_vfs_count;
/* the machine configurations of this firmware, in menu order */
extern const char *const ecat_machines[];
extern const unsigned ecat_machine_count;

const ecat_vfs_entry_t *ecat_vfs_find(const char *path);

/* the card is mounted (platform_mount_sd() == FR_OK) */
void frank_fs_set_sd(bool mounted);

#ifdef __cplusplus
}
#endif

#endif /* FRANK_VFS_H */
