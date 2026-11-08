/*
 * frank-ecat — eCat3 on RP2350 (FRANK M2)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The C heap (from frank-quake2's q2_malloc.c): newlib's allocator is
 * replaced (its _malloc_r & co) by two dlmalloc mspaces, one on the SRAM
 * left after .bss, one on the PSRAM left after .psram_bss. free/realloc
 * dispatch on the address.
 *
 * Placement: blocks up to ECAT_SRAM_SMALL (14 KB) go to SRAM: the emulator's
 * objects, devices and their tables, interface links, strings, touched on every
 * emulated instruction (the largest device object of the RK86 family is 13 KB);
 * larger ones to PSRAM: RAM banks, ROM disks, disk images, sound buffers. ecat_sram_malloc() / ecat_psram_malloc() force a side.
 * Before PSRAM is up everything comes from SRAM; when one side is full the
 * other is tried.
 *
 * dlmalloc is built without locks: only core 0 allocates (core 1 runs the
 * HDMI line IRQ, which never allocates after hdmi_audio_init()).
 */

#include <errno.h>
#include <reent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/sync.h"
#include "platform.h"
#include "psram_dlmalloc.h"

#ifndef ECAT_SRAM_SMALL
#define ECAT_SRAM_SMALL (14 * 1024)
#endif

typedef void *mspace;
extern mspace create_mspace_with_base(void *base, size_t capacity, int locked);
extern void *mspace_malloc(mspace msp, size_t bytes);
extern void *mspace_memalign(mspace msp, size_t alignment, size_t bytes);
extern void mspace_free(mspace msp, void *ptr);
extern size_t mspace_usable_size(const void *ptr);

extern uint8_t end[];          /* end of .bss: start of the SRAM heap */
extern uint8_t __HeapLimit[];  /* end of the RAM region */
extern uint8_t __psram_heap_start__[];

#define PSRAM_BASE 0x11000000u
#define PSRAM_END  0x11800000u

static mspace sram_msp;
static size_t sram_size, sram_used, sram_peak;
static size_t psram_size, psram_used, psram_peak;
static bool psram_ready;

static inline int is_psram(const void *p) {
    uintptr_t a = (uintptr_t)p;
    return a >= PSRAM_BASE && a < PSRAM_END;
}

static void sram_init(void) {
    uintptr_t b = ((uintptr_t)end + 15) & ~(uintptr_t)15;
    sram_size = (uintptr_t)__HeapLimit > b ? (uintptr_t)__HeapLimit - b : 0;
    if (sram_size >= 1024) sram_msp = create_mspace_with_base((void *)b, sram_size, 0);
}

/* After psram_init(): until then everything comes from SRAM (stdio, the
 * SDK's early allocations). */
void ecat_heap_init(void) {
    if (!sram_msp) sram_init();
    uintptr_t pb = ((uintptr_t)__psram_heap_start__ + 4095) & ~(uintptr_t)4095;
    psram_size = PSRAM_END - pb;
    psram_heap_init((void *)pb, psram_size);
    psram_ready = true;
}

void ecat_heap_report(void) {
    printf("heap: SRAM %u/%u KB (peak %u), PSRAM %u/%u KB (peak %u)\n",
           (unsigned)(sram_used / 1024), (unsigned)(sram_size / 1024), (unsigned)(sram_peak / 1024),
           (unsigned)(psram_used / 1024), (unsigned)(psram_size / 1024), (unsigned)(psram_peak / 1024));
}

static void *sram_malloc_nl(size_t n) {
    if (!sram_msp) sram_init();
    if (!sram_msp) return NULL; /* too little SRAM left for a heap */
    void *p = mspace_malloc(sram_msp, n);
    if (p) {
        sram_used += mspace_usable_size(p);
        if (sram_used > sram_peak) sram_peak = sram_used;
    }
    return p;
}

static void *psram_malloc_nl(size_t n) {
    if (!psram_ready) return NULL;
    void *p = psram_malloc(n);
    if (p) {
        psram_used += psram_usable_size(p);
        if (psram_used > psram_peak) psram_peak = psram_used;
    }
    return p;
}

/* Both cores allocate (the second processor of a machine runs on core 1):
 * every entry point holds a hardware spinlock, the helpers above and below
 * (_nl) run under it */
#define HEAP_LOCK spin_lock_instance(PICO_SPINLOCK_ID_OS1)
/* With the interrupts left on: an allocation in the PSRAM walks dlmalloc's
 * lists there for 40-90 us, and the HDMI line IRQ waiting behind it missed
 * lines until its DMA misloaded a descriptor and the picture stopped for good.
 * No interrupt handler of this firmware allocates, so none can deadlock on it */
static inline uint32_t heap_lock(void) { spin_lock_unsafe_blocking(HEAP_LOCK); return 0; }
static inline void heap_unlock(uint32_t s) { (void)s; spin_unlock_unsafe(HEAP_LOCK); }
#define LOCKED(expr) ({ uint32_t _s = heap_lock(); __typeof__(expr) _r = (expr); heap_unlock(_s); _r; })

void *ecat_sram_malloc(size_t n) { return LOCKED(sram_malloc_nl(n)); }
void *ecat_psram_malloc(size_t n) { return LOCKED(psram_malloc_nl(n)); }

/* Set while the C++ runtime allocates an exception: see below */
static volatile int sram_only;

static void *heap_malloc(size_t n) {
    void *p = NULL;
    if (sram_only) return sram_malloc_nl(n);
    if (!psram_ready || n <= ECAT_SRAM_SMALL) p = sram_malloc_nl(n);
    if (!p) p = psram_malloc_nl(n);
    if (!p) p = sram_malloc_nl(n);
    return p;
}

static void heap_free(void *p) {
    if (!p) return;
    if (is_psram(p)) {
        psram_used -= psram_usable_size(p);
        psram_free(p);
    } else if (sram_msp) {
        sram_used -= mspace_usable_size(p);
        mspace_free(sram_msp, p);
    }
}

static void *heap_realloc(void *p, size_t n) {
    if (!p) return heap_malloc(n);
    if (!n) {
        heap_free(p);
        return NULL;
    }
    size_t old = is_psram(p) ? psram_usable_size(p) : mspace_usable_size(p);
    if (n <= old) return p;
    void *q = heap_malloc(n);
    if (!q) return NULL;
    memcpy(q, p, old);
    heap_free(p);
    return q;
}

/* ---- newlib's allocator entry points ---- */

void *_malloc_r(struct _reent *r, size_t n) {
    void *p = LOCKED(heap_malloc(n));
    if (!p) r->_errno = ENOMEM;
    return p;
}

void _free_r(struct _reent *r, void *p) {
    (void)r;
    uint32_t s = heap_lock();
    heap_free(p);
    heap_unlock(s);
}

void *_realloc_r(struct _reent *r, void *p, size_t n) {
    /* The copy outside the lock: it holds the interrupts off, and growing a
     * disk image's buffer copied hundreds of KB with them off - the HDMI
     * line IRQ, on this core in the ДВК firmware, missed its lines and the
     * picture stopped */
    if (p && n) {
        uint32_t s = heap_lock();
        const size_t old = is_psram(p) ? psram_usable_size(p) : mspace_usable_size(p);
        if (n <= old) { heap_unlock(s); return p; }
        void *q = heap_malloc(n);
        heap_unlock(s);
        if (!q) { r->_errno = ENOMEM; return NULL; }
        memcpy(q, p, old);
        s = heap_lock();
        heap_free(p);
        heap_unlock(s);
        return q;
    }
    void *q = LOCKED(heap_realloc(p, n));
    if (!q && n) r->_errno = ENOMEM;
    return q;
}

void *_calloc_r(struct _reent *r, size_t a, size_t b) {
    size_t n = a * b;
    if (b && n / b != a) {
        r->_errno = ENOMEM;
        return NULL;
    }
    void *p = _malloc_r(r, n);
    if (p) memset(p, 0, n);
    return p;
}

/* SRAM only: the PSRAM mspace (psram_heap.c) has no memalign. The HDMI
 * driver's x3 palette page (8 KB, 8 KB-aligned) is the one user. */
void *_memalign_r(struct _reent *r, size_t align, size_t n) {
    uint32_t s = heap_lock();
    if (!sram_msp) sram_init();
    void *p = NULL;
    if (sram_msp) {
        p = mspace_memalign(sram_msp, align, n);
        if (p) {
            sram_used += mspace_usable_size(p);
            if (sram_used > sram_peak) sram_peak = sram_used;
        }
    }
    heap_unlock(s);
    if (!p) r->_errno = ENOMEM;
    return p;
}

size_t _malloc_usable_size_r(struct _reent *r, void *p) {
    (void)r;
    return is_psram(p) ? psram_usable_size(p) : mspace_usable_size(p);
}

/* pico-speccy's HDMI driver allocates through this (NULL on failure, zeroed) */
void *tryCalloc(size_t n) {
    void *p = ecat_sram_malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

/* ...and this (NULL on failure; the vertical line map, read by the IRQ) */
void *tryMalloc(size_t n) {
    return ecat_sram_malloc(n);
}

/* A thrown C++ exception never lives in PSRAM. libsupc++ (built without
 * -fno-inline-atomics) counts its references with LDREX/STREX, and a STREX
 * to PSRAM never succeeds: the first exception allocated there once the SRAM
 * heap was full spun in __cxa_begin_catch for ever (Агат-9, loading its
 * configuration). With no SRAM left, malloc's NULL sends libsupc++ to its
 * emergency pool, which it took from the SRAM at start-up. */
extern void *__real___cxa_allocate_exception(size_t n);
void *__wrap___cxa_allocate_exception(size_t n) {
    sram_only++;
    void *p = __real___cxa_allocate_exception(n);
    sram_only--;
    return p;
}
