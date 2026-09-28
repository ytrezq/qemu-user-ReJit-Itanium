/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Host floating-point environment used by FP instructions that TCG
 * compiles to host FP code (see include/tcg/tcg-fpop.h).
 *
 * The host control register keeps its default settings (all exceptions
 * masked, round-to-nearest): guest code only runs FP instructions as host
 * FP code under the same conditions (see FP_SOFT_MASK), which also keeps
 * softfloat's hardfloat paths right.  The exception flags raised by the
 * host FP code accumulate in the host status register until
 * ppc_fpscr_sync() folds them into FPSCR.
 */

#ifndef TARGET_PPC_FPU_HOST_H
#define TARGET_PPC_FPU_HOST_H

#include "tcg/tcg-fpop.h"

#if defined(__x86_64__)
#define PPC_HOST_FPENV 1

/* MXCSR */
#define PPC_HOST_FLAG_IE    0x01    /* invalid */
#define PPC_HOST_FLAG_DE    0x02    /* denormal operand (no PowerPC twin) */
#define PPC_HOST_FLAG_ZE    0x04    /* divide by zero */
#define PPC_HOST_FLAG_OE    0x08    /* overflow */
#define PPC_HOST_FLAG_UE    0x10    /* underflow */
#define PPC_HOST_FLAG_PE    0x20    /* inexact */
#define PPC_HOST_FLAGS      0x3f

static inline uint32_t ppc_host_fp_get(void)
{
    uint32_t m;
    asm volatile("stmxcsr %0" : "=m"(m));
    return m;
}

static inline void ppc_host_fp_set(uint32_t m)
{
    asm volatile("ldmxcsr %0" : : "m"(m));
}
#endif

/*
 * For helpers whose host FP flags must not reach FPSCR: softfloat's
 * hardfloat paths in the Altivec helpers (vaddfp...), which have no
 * FPSCR side effects.
 */
static inline uint32_t ppc_host_fp_save(void)
{
    return tcg_host_fpenv_save();
}

static inline void ppc_host_fp_restore(uint32_t saved)
{
    tcg_host_fpenv_restore(saved);
}

#endif /* TARGET_PPC_FPU_HOST_H */
