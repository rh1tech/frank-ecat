/*
 * frank-ecat — eCat3 on RP2350 (FRANK M2)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The atomic operations of the eCat3 core (built with -fno-inline-atomics).
 * Its objects may be allocated in PSRAM, behind the XIP interface, where the
 * exclusive accesses (LDREX/STREX) that GCC inlines for std::atomic never
 * succeed: a fetch_add there spun forever (hw 2026-11-08, frame_complete()).
 * The emulator runs on core 0 alone, so an operation with the interrupts off
 * is atomic enough; loads and stores of a naturally aligned word are atomic
 * as they are.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hardware/sync.h"

#define ATOMIC_OPS(N, T)                                                                     \
    T __atomic_load_##N(const volatile void *p, int m) {                                     \
        (void)m;                                                                             \
        __dmb();                                                                             \
        T v = *(const volatile T *)p;                                                        \
        __dmb();                                                                             \
        return v;                                                                            \
    }                                                                                        \
    void __atomic_store_##N(volatile void *p, T v, int m) {                                  \
        (void)m;                                                                             \
        __dmb();                                                                             \
        *(volatile T *)p = v;                                                                \
        __dmb();                                                                             \
    }                                                                                        \
    T __atomic_exchange_##N(volatile void *p, T v, int m) {                                  \
        (void)m;                                                                             \
        uint32_t s = save_and_disable_interrupts();                                          \
        T old = *(volatile T *)p;                                                            \
        *(volatile T *)p = v;                                                                \
        restore_interrupts(s);                                                               \
        return old;                                                                          \
    }                                                                                        \
    bool __atomic_compare_exchange_##N(volatile void *p, void *expected, T desired, bool weak, \
                                       int ms, int mf) {                                     \
        (void)weak; (void)ms; (void)mf;                                                      \
        uint32_t s = save_and_disable_interrupts();                                          \
        T cur = *(volatile T *)p;                                                            \
        bool ok = cur == *(T *)expected;                                                     \
        if (ok) *(volatile T *)p = desired;                                                  \
        else *(T *)expected = cur;                                                           \
        restore_interrupts(s);                                                               \
        return ok;                                                                           \
    }                                                                                        \
    ATOMIC_RMW(N, T, fetch_add, add_fetch, +)                                                \
    ATOMIC_RMW(N, T, fetch_sub, sub_fetch, -)                                                \
    ATOMIC_RMW(N, T, fetch_and, and_fetch, &)                                                \
    ATOMIC_RMW(N, T, fetch_or, or_fetch, |)                                                  \
    ATOMIC_RMW(N, T, fetch_xor, xor_fetch, ^)

#define ATOMIC_RMW(N, T, FETCH_OP, OP_FETCH, OP)                                             \
    T __atomic_##FETCH_OP##_##N(volatile void *p, T v, int m) {                              \
        (void)m;                                                                             \
        uint32_t s = save_and_disable_interrupts();                                          \
        T old = *(volatile T *)p;                                                            \
        *(volatile T *)p = (T)(old OP v);                                                    \
        restore_interrupts(s);                                                               \
        return old;                                                                          \
    }                                                                                        \
    T __atomic_##OP_FETCH##_##N(volatile void *p, T v, int m) {                              \
        (void)m;                                                                             \
        uint32_t s = save_and_disable_interrupts();                                          \
        T now = (T)(*(volatile T *)p OP v);                                                  \
        *(volatile T *)p = now;                                                              \
        restore_interrupts(s);                                                               \
        return now;                                                                          \
    }

ATOMIC_OPS(1, uint8_t)
ATOMIC_OPS(2, uint16_t)
ATOMIC_OPS(4, uint32_t)
/* 8 bytes: pico_atomic (hardware spinlocks, no exclusive accesses either) */
