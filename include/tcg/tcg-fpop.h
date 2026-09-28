/* SPDX-License-Identifier: MIT */
/*
 * Host floating-point operations for TCG
 *
 * The fpop*_vec opcodes let a front end emit IEEE 754 arithmetic that the
 * backend maps onto host floating-point instructions, instead of calling
 * softfloat helpers for every guest FP instruction.
 *
 * Floating-point environment: these operations round according to the
 * host floating-point control register (MXCSR on x86) and accumulate
 * their exception flags in the host status register.  The front end owns
 * that state: it must program the rounding mode and harvest the flags
 * itself (see target/ppc/fpu_helper.c for a lazy FPSCR built this way).
 * Front ends must only use these ops when tcg_can_emit_fpop() says so,
 * and fall back to their helpers otherwise.
 *
 * Element types: vece MO_32 (binary32) or MO_64 (binary64).  With
 * TCG_TYPE_V64 the op works on a single scalar element (lane 0); for MO_32
 * the other 32 bits of the V64 result are zero, as a write of a single
 * register zero-extends it on Arm.
 * Lanes outside the TCG type never raise exceptions.
 *
 * NaN results: a NaN operand propagates quieted; with several NaN operands
 * the first one in operand order wins (a, then b, then c) unless a flag
 * below says otherwise.  Generated NaNs (invalid operations) are the host
 * default NaN unless TCG_FPOP_F_DEFNAN is given.
 */

#ifndef TCG_FPOP_H
#define TCG_FPOP_H

/*
 * fpop1_i64, fpop2_i64, fpop3_i64 are the scalar ops of TCG_TYPE_V64 on
 * i64 temps instead of vectors, for front ends that keep FP registers in
 * i64 globals: a MO_32 element is the low half of the i64 (the high half
 * is ignored), and a binary32 or int32 result is zero-extended.  With
 * fpop2_i64, TCG_FPOP_CMP gives 0 or 1.
 */

typedef enum TCGFPOp {
    /* fpop2_vec d, a, b */
    TCG_FPOP_ADD,
    TCG_FPOP_SUB,
    TCG_FPOP_MUL,
    TCG_FPOP_DIV,
    TCG_FPOP_MAXC,      /* a > b ? a : b  (b if unordered or equal) */
    TCG_FPOP_MINC,      /* a < b ? a : b  (b if unordered or equal) */
    TCG_FPOP_CMP,       /* all-ones element mask if TCG_FPOP_PRED holds */
    TCG_FPOP_MAXNUM,    /* IEEE 754-2008 maxNum: -0 < +0, a quiet NaN loses */
    TCG_FPOP_MINNUM,    /* IEEE 754-2008 minNum */
    TCG_FPOP_MAX,       /* IEEE 754-2019 maximum: -0 < +0, NaNs propagate */
    TCG_FPOP_MIN,       /* IEEE 754-2019 minimum */

    /* fpop1_vec d, a */
    TCG_FPOP_SQRT,
    TCG_FPOP_RECIP,     /* 1 / a */
    TCG_FPOP_RSQRT,     /* 1 / sqrt(a), with two roundings */
    TCG_FPOP_RINT,      /* round to integral, TCG_FPOP_RMODE; inexact only
                           with TCG_FPRND_CURRENT */
    TCG_FPOP_RND_SP,    /* MO_64 only: round to binary32 precision */
    TCG_FPOP_CVT_S,     /* signed integer element -> fp of the same size */
    TCG_FPOP_CVT_U,     /* unsigned integer element -> fp of the same size */
    TCG_FPOP_CVTI_S,    /* fp -> signed integer element, saturating */
    TCG_FPOP_CVTI_U,    /* fp -> unsigned integer element, saturating */
    TCG_FPOP_CVTI_S32,  /* MO_64: binary64 -> int32, sign-extended to 64 */
    TCG_FPOP_CVTI_U32,  /* MO_64: binary64 -> uint32, zero-extended to 64 */
    TCG_FPOP_CVTI_S64,  /* MO_32, scalar: binary32 -> int64 */
    TCG_FPOP_CVTI_U64,  /* MO_32, scalar: binary32 -> uint64 */
    TCG_FPOP_CVT_F64,   /* MO_32, scalar: binary32 -> binary64 */
    TCG_FPOP_CVT_F32,   /* MO_64, scalar: binary64 -> binary32 (MO_32 result) */

    /* fpop3_vec d, a, b, c */
    TCG_FPOP_FMA,       /* d = a * b + c, single rounding */

    TCG_FPOP_NB_OPS
} TCGFPOp;

#define TCG_FPOP_OP(x)          ((x) & 0xff)

/*
 * Generated NaNs are the positive quiet NaN with an all-zero payload
 * (0x7ff8000000000000 / 0x7fc00000), as on PowerPC, Arm and RISC-V.
 */
#define TCG_FPOP_F_DEFNAN       (1u << 8)
/*
 * MO_64 only: the result is rounded to binary32 precision and range, and
 * returned in binary64 format.  For FMA, the multiply-add is done in
 * binary32 after converting the operands (exact for binary32 operands).
 */
#define TCG_FPOP_F_SP           (1u << 9)
/* FMA: with several NaN operands, prefer a, then c, then b (PowerPC). */
#define TCG_FPOP_F_NAN_ACB      (1u << 10)
/* FMA: negate the product, the addend, or the rounded non-NaN result. */
#define TCG_FPOP_F_NEG_PROD     (1u << 11)
#define TCG_FPOP_F_NEG_ADD      (1u << 12)
#define TCG_FPOP_F_NEG_RES      (1u << 13)
/* CVTI_*: round toward zero instead of using the current rounding mode. */
#define TCG_FPOP_F_TRUNC        (1u << 14)
/*
 * CVTI_*: PowerPC results for out-of-range operands, i.e. the saturated
 * value, and for NaN 0x8000... (signed) or 0 (unsigned).  For CVTI_S32 a
 * NaN gives 0x0000000080000000.  Without it, the host result is kept.
 */
#define TCG_FPOP_F_PPC_SAT      (1u << 15)
/* CMP and fpcmpcc: signalling compare (invalid for any NaN operand). */
#define TCG_FPOP_F_SIGNAL       (1u << 16)
/* CVTI_S32, CVTI_U32: the 32-bit result in both halves of the element. */
#define TCG_FPOP_F_DUP32        (1u << 17)
/*
 * Arm NaN propagation (FPProcessNaNs, FPProcessNaNs3): signalling NaNs
 * take priority over quiet ones, then operand order, which for FMA is
 * c (the addend), a, b; and 0 * inf + a quiet NaN is an invalid operation
 * that gives the default NaN.  Also for MAX, MIN, MAXNUM and MINNUM.
 */
#define TCG_FPOP_F_SNAN_FIRST   (1u << 18)
/* CVT_S, CVT_U of MO_64 integers: binary32 result, as with MO_32 */
#define TCG_FPOP_F_RES32        (1u << 19)
/* CVTI_*: a NaN converts to 0 (Arm) */
#define TCG_FPOP_F_NAN_ZERO     (1u << 27)
/* fpcmpcc_vec: Arm NZCV in bits 31..28 instead of TCG_FPCC_* */
#define TCG_FPOP_F_CC_NZCV      (1u << 28)
/*
 * RISC-V rules, scalar (TCG_TYPE_V64) only:
 *  - every NaN result is the default NaN (0x7ff8000000000000 /
 *    0x7fc00000), and 0 * inf + a quiet NaN signals invalid (FMA);
 *  - tininess is detected after rounding, as on x86;
 *  - MAXNUM, MINNUM are IEEE 754-2019 maximumNumber, minimumNumber: any
 *    NaN, signalling (which signals invalid) or not, loses against a
 *    number, and two NaNs give the default NaN;
 *  - CVTI_*: out-of-range values saturate, and a NaN converts to the
 *    maximum integer.
 * Not with the other NaN, rounding or saturation flags.
 */
#define TCG_FPOP_F_RISCV        (1u << 29)

/* CMP predicates */
typedef enum TCGFPPred {
    TCG_FPPRED_EQ,      /* a == b, false if unordered */
    TCG_FPPRED_LT,      /* a < b */
    TCG_FPPRED_LE,      /* a <= b */
    TCG_FPPRED_GT,      /* a > b */
    TCG_FPPRED_GE,      /* a >= b */
    TCG_FPPRED_UNORD,   /* a or b is NaN */
    TCG_FPPRED_NE,      /* !(a == b), true if unordered */
    TCG_FPPRED_LTGT,    /* a < b || a > b, false if unordered */
} TCGFPPred;

#define TCG_FPOP_PRED(p)        ((unsigned)(p) << 20)
#define TCG_FPOP_GET_PRED(x)    (((x) >> 20) & 0xf)

/* RINT rounding modes */
typedef enum TCGFPRound {
    TCG_FPRND_NEAREST_EVEN,
    TCG_FPRND_DOWN,
    TCG_FPRND_UP,
    TCG_FPRND_ZERO,
    TCG_FPRND_CURRENT,      /* host control register, may be inexact */
} TCGFPRound;

#define TCG_FPOP_RMODE(m)       ((unsigned)(m) << 24)
#define TCG_FPOP_GET_RMODE(x)   (((x) >> 24) & 0x7)

/*
 * fpcmpcc_vec produces an i32 with exactly one of these bits set,
 * which is the layout of a PowerPC CR field.
 */
#define TCG_FPCC_LT             8
#define TCG_FPCC_GT             4
#define TCG_FPCC_EQ             2
#define TCG_FPCC_UN             1

/*
 * The host exception flags, where the fpops accumulate theirs, in a
 * portable encoding; tcg_host_fpexc_clear() clears them.
 */
#define TCG_FPEXC_INVALID       1
#define TCG_FPEXC_DIVZERO       2
#define TCG_FPEXC_OVERFLOW      4
#define TCG_FPEXC_UNDERFLOW     8
#define TCG_FPEXC_INEXACT       16

#if defined(__x86_64__)
static inline unsigned tcg_host_fpexc_get(void)
{
    uint32_t m;
    asm volatile("stmxcsr %0" : "=m"(m));
    /* MXCSR: IE 0x01, DE 0x02, ZE 0x04, OE 0x08, UE 0x10, PE 0x20 */
    return (m & 1) | ((m >> 1) & 0x1e);
}

static inline void tcg_host_fpexc_clear(void)
{
    uint32_t m;
    asm volatile("stmxcsr %0" : "=m"(m));
    if (m & 0x3f) {
        m &= ~0x3fu;
        asm volatile("ldmxcsr %0" : : "m"(m));
    }
}
#else
static inline unsigned tcg_host_fpexc_get(void)
{
    return 0;
}

static inline void tcg_host_fpexc_clear(void)
{
}
#endif

/*
 * Host C code, e.g. glib in the translator itself, may raise host FP
 * exception flags as well.  Code that runs between the generated
 * instructions of a front end that harvests those flags lazily brackets
 * itself with these so that it does not leave any behind.
 */
#if defined(__x86_64__)
static inline uint32_t tcg_host_fpenv_save(void)
{
    uint32_t m;
    asm volatile("stmxcsr %0" : "=m"(m));
    return m;
}

static inline void tcg_host_fpenv_restore(uint32_t saved)
{
    uint32_t m;
    asm volatile("stmxcsr %0" : "=m"(m));
    if (m != saved) {
        asm volatile("ldmxcsr %0" : : "m"(saved));
    }
}
#else
static inline uint32_t tcg_host_fpenv_save(void)
{
    return 0;
}

static inline void tcg_host_fpenv_restore(uint32_t saved)
{
}
#endif

#endif /* TCG_FPOP_H */
