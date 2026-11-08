/*
 * frank-ecat — eCat3 on RP2350 (FRANK M2)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The C library's files (newlib syscalls, the pico-sdk ones are weak):
 *   0..2   the console (pico stdio);
 *   a path in the flash VFS, opened for reading: served from flash;
 *   anything else: FatFs on the SD card (paths as they are, "/ecat/...").
 * Paths are normalised first ("a//b", "a/./b", "a/x/../b"), since the emulator
 * joins its directories and file names as strings.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ff.h"
#include "frank_vfs.h"
#include "pico/stdio.h"
#include "pico/time.h"

#define MAX_FILES 12
#define FD_BASE   3

typedef struct {
    bool used;
    const ecat_vfs_entry_t *vfs;   /* NULL: a FatFs file */
    uint32_t pos;
    FIL *fil;
} file_t;

static file_t files[MAX_FILES];
static bool sd_mounted;

void frank_fs_set_sd(bool mounted) {
    sd_mounted = mounted;
}

const ecat_vfs_entry_t *ecat_vfs_find(const char *path) {
    for (unsigned i = 0; i < ecat_vfs_count; i++)
        if (strcmp(ecat_vfs[i].path, path) == 0) return &ecat_vfs[i];
    return NULL;
}

/* "a//b" -> "a/b", "a/./b" -> "a/b", "a/x/../b" -> "a/b"; backslashes too */
static void normalise(const char *in, char *out, size_t n) {
    size_t o = 0;
    const char *p = in;
    while (*p && o + 1 < n) {
        char c = (*p == '\\') ? '/' : *p;
        if (c == '/') {
            while (p[1] == '/' || p[1] == '\\') p++;
            if (p[1] == '.' && (p[2] == '/' || p[2] == '\\' || p[2] == 0)) {
                p += 2;
                continue;
            }
            if (p[1] == '.' && p[2] == '.' && (p[3] == '/' || p[3] == '\\' || p[3] == 0)) {
                while (o > 0 && out[o - 1] != '/') o--;   /* drop the last component */
                if (o > 0) o--;
                p += 3;
                continue;
            }
        }
        out[o++] = c;
        p++;
    }
    out[o] = 0;
    if (o == 0 && n > 1) { out[0] = '/'; out[1] = 0; }
}

static int alloc_fd(void) {
    for (int i = 0; i < MAX_FILES; i++)
        if (!files[i].used) {
            memset(&files[i], 0, sizeof(files[i]));
            files[i].used = true;
            return i;
        }
    return -1;
}

static file_t *get(int fd) {
    fd -= FD_BASE;
    if (fd < 0 || fd >= MAX_FILES || !files[fd].used) return NULL;
    return &files[fd];
}

static int fr_errno(FRESULT fr) {
    switch (fr) {
    case FR_NO_FILE: case FR_NO_PATH: case FR_INVALID_NAME: return ENOENT;
    case FR_DENIED: case FR_WRITE_PROTECTED: return EACCES;
    case FR_EXIST: return EEXIST;
    case FR_TOO_MANY_OPEN_FILES: return EMFILE;
    case FR_NOT_ENOUGH_CORE: return ENOMEM;
    default: return EIO;
    }
}

int _open(const char *name, int flags, ...) {
    char path[256];
    normalise(name, path, sizeof(path));
    const int acc = flags & O_ACCMODE;
    if (acc == O_RDONLY) {
        const ecat_vfs_entry_t *v = ecat_vfs_find(path);
        if (v) {
            int i = alloc_fd();
            if (i < 0) { errno = EMFILE; return -1; }
            files[i].vfs = v;
            return i + FD_BASE;
        }
    }
    if (!sd_mounted) { errno = ENOENT; return -1; }
    BYTE mode = 0;
    if (acc == O_RDONLY) mode = FA_READ;
    else if (acc == O_WRONLY) mode = FA_WRITE;
    else mode = FA_READ | FA_WRITE;
    if (flags & O_CREAT) mode |= (flags & O_TRUNC) ? FA_CREATE_ALWAYS : FA_OPEN_ALWAYS;
    else if (flags & O_TRUNC) mode |= FA_CREATE_ALWAYS;
    if (flags & O_APPEND) mode |= FA_OPEN_APPEND;
    int i = alloc_fd();
    if (i < 0) { errno = EMFILE; return -1; }
    FIL *f = (FIL *)malloc(sizeof(FIL));
    if (!f) { files[i].used = false; errno = ENOMEM; return -1; }
    FRESULT fr = f_open(f, path, mode);
    if (fr != FR_OK) {
        free(f);
        files[i].used = false;
        errno = fr_errno(fr);
        return -1;
    }
    files[i].fil = f;
    return i + FD_BASE;
}

int _close(int fd) {
    file_t *f = get(fd);
    if (!f) { errno = EBADF; return -1; }
    if (f->fil) {
        f_close(f->fil);
        free(f->fil);
    }
    f->used = false;
    return 0;
}

int _read(int fd, char *buf, int len) {
    if (fd == 0) return stdio_get_until(buf, len, at_the_end_of_time);
    file_t *f = get(fd);
    if (!f) { errno = EBADF; return -1; }
    if (f->vfs) {
        uint32_t left = f->vfs->size > f->pos ? f->vfs->size - f->pos : 0;
        uint32_t n = (uint32_t)len < left ? (uint32_t)len : left;
        memcpy(buf, f->vfs->data + f->pos, n);
        f->pos += n;
        return (int)n;
    }
    UINT got = 0;
    FRESULT fr = f_read(f->fil, buf, (UINT)len, &got);
    if (fr != FR_OK) { errno = fr_errno(fr); return -1; }
    return (int)got;
}

int _write(int fd, const char *buf, int len) {
    if (fd == 1 || fd == 2) {
        stdio_put_string(buf, len, false, true);
        return len;
    }
    file_t *f = get(fd);
    if (!f) { errno = EBADF; return -1; }
    if (f->vfs) { errno = EBADF; return -1; }
    UINT put = 0;
    FRESULT fr = f_write(f->fil, buf, (UINT)len, &put);
    if (fr != FR_OK) { errno = fr_errno(fr); return -1; }
    return (int)put;
}

off_t _lseek(int fd, off_t pos, int whence) {
    file_t *f = get(fd);
    if (!f) { errno = EBADF; return -1; }
    int64_t base;
    int64_t size = f->vfs ? (int64_t)f->vfs->size : (int64_t)f_size(f->fil);
    int64_t cur = f->vfs ? (int64_t)f->pos : (int64_t)f_tell(f->fil);
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = cur; break;
    case SEEK_END: base = size; break;
    default: errno = EINVAL; return -1;
    }
    int64_t to = base + pos;
    if (to < 0) { errno = EINVAL; return -1; }
    if (f->vfs) {
        f->pos = (uint32_t)to;
        return (off_t)to;
    }
    FRESULT fr = f_lseek(f->fil, (FSIZE_t)to);
    if (fr != FR_OK) { errno = fr_errno(fr); return -1; }
    return (off_t)to;
}

static void fill_stat(struct stat *st, uint32_t size, bool dir) {
    memset(st, 0, sizeof(*st));
    st->st_size = size;
    st->st_mode = dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    st->st_blksize = 512;
}

int _fstat(int fd, struct stat *st) {
    if (fd >= 0 && fd < FD_BASE) {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFCHR;
        return 0;
    }
    file_t *f = get(fd);
    if (!f) { errno = EBADF; return -1; }
    fill_stat(st, f->vfs ? f->vfs->size : (uint32_t)f_size(f->fil), false);
    return 0;
}

int _stat(const char *name, struct stat *st) {
    char path[256];
    normalise(name, path, sizeof(path));
    const ecat_vfs_entry_t *v = ecat_vfs_find(path);
    if (v) {
        fill_stat(st, v->size, false);
        return 0;
    }
    /* a directory of the VFS: some entry starts with "path/" */
    size_t n = strlen(path);
    for (unsigned i = 0; i < ecat_vfs_count; i++)
        if (strncmp(ecat_vfs[i].path, path, n) == 0 && ecat_vfs[i].path[n] == '/') {
            fill_stat(st, 0, true);
            return 0;
        }
    if (!sd_mounted) { errno = ENOENT; return -1; }
    FILINFO fi;
    FRESULT fr = f_stat(path, &fi);
    if (fr != FR_OK) { errno = fr_errno(fr); return -1; }
    fill_stat(st, (uint32_t)fi.fsize, (fi.fattrib & AM_DIR) != 0);
    return 0;
}

int _isatty(int fd) {
    return fd >= 0 && fd < FD_BASE;
}

int _unlink(const char *name) {
    char path[256];
    normalise(name, path, sizeof(path));
    if (!sd_mounted || ecat_vfs_find(path)) { errno = EACCES; return -1; }
    FRESULT fr = f_unlink(path);
    if (fr != FR_OK) { errno = fr_errno(fr); return -1; }
    return 0;
}

int mkdir(const char *name, mode_t mode) {
    (void)mode;
    char path[256];
    normalise(name, path, sizeof(path));
    if (!sd_mounted) { errno = ENOENT; return -1; }
    FRESULT fr = f_mkdir(path);
    if (fr != FR_OK && fr != FR_EXIST) { errno = fr_errno(fr); return -1; }
    return fr == FR_EXIST ? (errno = EEXIST, -1) : 0;
}

int _rename(const char *from, const char *to) {
    char a[256], b[256];
    normalise(from, a, sizeof(a));
    normalise(to, b, sizeof(b));
    if (!sd_mounted) { errno = ENOENT; return -1; }
    f_unlink(b);
    FRESULT fr = f_rename(a, b);
    if (fr != FR_OK) { errno = fr_errno(fr); return -1; }
    return 0;
}

/* the emulator's clock (thread_compat.h, ECAT_SINGLE_THREAD) */
uint64_t ecat_platform_now_ms(void) {
    return time_us_64() / 1000;
}

void ecat_platform_sleep_ms(unsigned int ms) {
    sleep_ms(ms);
}

/* libstdc++'s this_thread::sleep_for */
int usleep(useconds_t us) {
    sleep_us(us);
    return 0;
}

unsigned int sleep(unsigned int seconds) {
    sleep_ms(seconds * 1000u);
    return 0;
}
