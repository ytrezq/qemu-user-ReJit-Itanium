/*
 * The per-CPU TranslationBlock jump cache.
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ACCEL_TCG_TB_JMP_CACHE_H
#define ACCEL_TCG_TB_JMP_CACHE_H

#include "qemu/rcu.h"
#include "exec/cpu-common.h"

#ifdef CONFIG_SOFTMMU
/* Emptied by every TLB flush: small, so that clearing it stays cheap. */
#define TB_JMP_CACHE_BITS 12
#else
/* User mode only empties it on a (rare) tb_flush: larger programs fit. */
#define TB_JMP_CACHE_BITS 14
#endif
#define TB_JMP_CACHE_SIZE (1 << TB_JMP_CACHE_BITS)
/*
 * User mode hash: with x = pc >> 1, x ^ (x >> 1) maps the low bits of x
 * one to one onto the table index, both for 2-byte aligned instructions
 * (Thumb, s390x) and 4-byte aligned ones (where it is y ^ (y << 1) of
 * y = pc >> 2): the alignment bits are never wasted.  The higher bits
 * are folded in with one more shift.
 */
#define TB_JMP_CACHE_HASH_S1   1
#define TB_JMP_CACHE_HASH_S2   2
#define TB_JMP_CACHE_HASH_S3   (TB_JMP_CACHE_BITS + 2)
#define TB_JMP_CACHE_HASH_CS_MUL 0x9e3779b1u

/*
 * Invalidated in parallel; all accesses to 'tb' must be atomic.
 * A valid entry is read/written by a single CPU, therefore there is
 * no need for qatomic_rcu_read() and pc is always consistent with a
 * non-NULL value of 'tb'.  Strictly speaking pc is only needed for
 * CF_PCREL, but it's used always for simplicity.
 */
typedef struct CPUJumpCache {
    struct rcu_head rcu;
    struct {
        TranslationBlock *tb;
        vaddr pc;
    } array[TB_JMP_CACHE_SIZE];
} CPUJumpCache;

#endif /* ACCEL_TCG_TB_JMP_CACHE_H */
