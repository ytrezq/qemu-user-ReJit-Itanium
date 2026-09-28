/* SPDX-License-Identifier: MIT */
/*
 * Host floating point for TCG on x86-64, see include/tcg/tcg-fpop.h.
 *
 * Everything here is EVEX-encoded and requires AVX-512 VL/BW/DQ:
 *  - opmask registers keep unused lanes from raising exceptions and make
 *    the NaN fix-ups branch-free on vectors;
 *  - xmm16-xmm31 are never handed out by the register allocator (it only
 *    knows xmm0-xmm15), so they are free scratch registers here.
 * Operations on 128-bit vectors only, so the core never has to lower its
 * clock for 512-bit instructions.
 */

#include "tcg/tcg-fpop.h"
#include "tcg/tcg-crypto.h"

/* Hardware xmm numbers: TCG vector registers are xmm0-15. */
#define FPX(r)          ((r) - TCG_REG_XMM0)
#define FPX_S0          16
#define FPX_S1          17
#define FPX_S2          18
#define FPX_S3          19

/* Opmask registers */
#define FPK1            1
#define FPK2            2
#define FPK3            3

/* SSE/AVX floating point; add fpo_pfx() for the element type. */
#define FPO_SQRT        (0x51 | P_EXT)
#define FPO_ADD         (0x58 | P_EXT)
#define FPO_MUL         (0x59 | P_EXT)
#define FPO_SUB         (0x5c | P_EXT)
#define FPO_MIN         (0x5d | P_EXT)
#define FPO_DIV         (0x5e | P_EXT)
#define FPO_MAX         (0x5f | P_EXT)
#define FPO_CMP         (0xc2 | P_EXT)

#define FPO_UCOMISD     (0x2e | P_EXT | P_DATA16 | P_VEXW)
#define FPO_COMISD      (0x2f | P_EXT | P_DATA16 | P_VEXW)
#define FPO_UCOMISS     (0x2e | P_EXT)
#define FPO_COMISS      (0x2f | P_EXT)

#define FPO_CVTSD2SS    (0x5a | P_EXT | P_SIMDF2 | P_VEXW)
#define FPO_CVTSS2SD    (0x5a | P_EXT | P_SIMDF3)
#define FPO_CVTPD2PS    (0x5a | P_EXT | P_DATA16 | P_VEXW)
#define FPO_CVTPS2PD    (0x5a | P_EXT)
#define FPO_CVTQQ2PD    (0xe6 | P_EXT | P_SIMDF3 | P_VEXW)
#define FPO_CVTUQQ2PD   (0x7a | P_EXT | P_SIMDF3 | P_VEXW)
#define FPO_CVTQQ2PS    (0x5b | P_EXT | P_VEXW)
#define FPO_CVTUQQ2PS   (0x7a | P_EXT | P_SIMDF2 | P_VEXW)
#define FPO_CVTPD2QQ    (0x7b | P_EXT | P_DATA16 | P_VEXW)
#define FPO_CVTTPD2QQ   (0x7a | P_EXT | P_DATA16 | P_VEXW)
#define FPO_CVTPD2UQQ   (0x79 | P_EXT | P_DATA16 | P_VEXW)
#define FPO_CVTTPD2UQQ  (0x78 | P_EXT | P_DATA16 | P_VEXW)
#define FPO_CVTPD2DQ    (0xe6 | P_EXT | P_SIMDF2 | P_VEXW)
#define FPO_CVTTPD2DQ   (0xe6 | P_EXT | P_DATA16 | P_VEXW)
#define FPO_CVTPD2UDQ   (0x79 | P_EXT | P_VEXW)
#define FPO_CVTTPD2UDQ  (0x78 | P_EXT | P_VEXW)
#define FPO_CVTDQ2PS    (0x5b | P_EXT)
#define FPO_CVTUDQ2PS   (0x7a | P_EXT | P_SIMDF2)
#define FPO_CVTPS2DQ    (0x5b | P_EXT | P_DATA16)
#define FPO_CVTTPS2DQ   (0x5b | P_EXT | P_SIMDF3)
#define FPO_CVTPS2UDQ   (0x79 | P_EXT)
#define FPO_CVTTPS2UDQ  (0x78 | P_EXT)
#define FPO_PMOVSXDQ    (0x25 | P_EXT38 | P_DATA16)
#define FPO_PMOVZXDQ    (0x35 | P_EXT38 | P_DATA16)

/* VRNDSCALE{PS,PD,SS,SD} */
#define FPO_RNDSCALEPS  (0x08 | P_EXT3A | P_DATA16)
#define FPO_RNDSCALEPD  (0x09 | P_EXT3A | P_DATA16 | P_VEXW)
#define FPO_RNDSCALESS  (0x0a | P_EXT3A | P_DATA16)
#define FPO_RNDSCALESD  (0x0b | P_EXT3A | P_DATA16 | P_VEXW)

/* VFMADD132 family: +1 scalar, +2 msub, +4 nmadd, +6 nmsub; W1 = double */
#define FPO_FMA132      (0x98 | P_EXT38 | P_DATA16)

#define FPO_PANDQ       (0xdb | P_EXT | P_DATA16 | P_VEXW)
#define FPO_PANDD       (0xdb | P_EXT | P_DATA16)
#define FPO_PORQ        (0xeb | P_EXT | P_DATA16 | P_VEXW)
#define FPO_PORD        (0xeb | P_EXT | P_DATA16)
#define FPO_PXORQ       (0xef | P_EXT | P_DATA16 | P_VEXW)
#define FPO_PXORD       (0xef | P_EXT | P_DATA16)
#define FPO_MOVDQA64    (0x6f | P_EXT | P_DATA16 | P_VEXW)
#define FPO_PBLENDMQ    (0x64 | P_EXT38 | P_DATA16 | P_VEXW)
#define FPO_PBLENDMD    (0x64 | P_EXT38 | P_DATA16)
#define FPO_FPCLASSSD   (0x67 | P_EXT3A | P_DATA16 | P_VEXW)
#define FPO_PERMI2B     (0x75 | P_EXT38 | P_DATA16)
#define FPO_RANGEP      (0x50 | P_EXT3A | P_DATA16)     /* +1 scalar, W1 double */
#define FPO_FPCLASSP    (0x66 | P_EXT3A | P_DATA16)     /* +1 scalar, W1 double */
#define FPO_PCMPQ       (0x1f | P_EXT3A | P_DATA16 | P_VEXW)
#define FPO_PCMPD       (0x1f | P_EXT3A | P_DATA16)
#define FPO_PMOVQ2M     (0x39 | P_EXT38 | P_SIMDF3 | P_VEXW)
#define FPO_PMOVD2M     (0x39 | P_EXT38 | P_SIMDF3)
#define FPO_INSERTPS    (0x21 | P_EXT3A | P_DATA16)
#define FPO_CVTTPS2QQ   (0x7a | P_EXT | P_DATA16)
#define FPO_CVTPS2QQ    (0x7b | P_EXT | P_DATA16)
#define FPO_CVTTPS2UQQ  (0x78 | P_EXT | P_DATA16)
#define FPO_CVTPS2UQQ   (0x79 | P_EXT | P_DATA16)

/* vpcmp predicates */
#define FPO_PCMP_EQ     0
#define FPO_PCMP_LT     1

/* EVEX static rounding, {rn,rd,ru,rz}-sae */
#define FPO_RC_RN       0
#define FPO_RC_RD       1
#define FPO_RC_RU       2
#define FPO_RC_RZ       3

/* Smallest normal numbers */
#define FPO_DBL_MIN     0x0010000000000000ull
#define FPO_FLT_MIN     0x00800000u
#define FPO_FLT_MIN_D   0x3810000000000000ull   /* FLT_MIN as a double */

#define FPO_MOVDQU8_LD  (0x6f | P_EXT | P_SIMDF2)
#define FPO_MOVDQU8_ST  (0x7f | P_EXT | P_SIMDF2)
#define FPO_KMOVW_KR    (0x92 | P_EXT)

/* Opmask instructions, VEX-encoded */
#define FPO_KANDB       (0x41 | P_EXT | P_DATA16 | P_VEXL)
#define FPO_KANDNB      (0x42 | P_EXT | P_DATA16 | P_VEXL)
#define FPO_KORB        (0x45 | P_EXT | P_DATA16 | P_VEXL)
#define FPO_KXNORB      (0x46 | P_EXT | P_DATA16 | P_VEXL)
#define FPO_KORTESTB    (0x98 | P_EXT | P_DATA16)
#define FPO_KSHIFTRB    (0x30 | P_EXT3A | P_DATA16)

/* vcmp predicates (quiet, signalling) for each TCGFPPred */
static const uint8_t fpo_cmp_pred[][2] = {
    [TCG_FPPRED_EQ]    = { 0x00, 0x10 },  /* EQ_OQ, EQ_OS */
    [TCG_FPPRED_LT]    = { 0x11, 0x01 },  /* LT_OQ, LT_OS */
    [TCG_FPPRED_LE]    = { 0x12, 0x02 },  /* LE_OQ, LE_OS */
    [TCG_FPPRED_GT]    = { 0x1e, 0x0e },  /* GT_OQ, GT_OS */
    [TCG_FPPRED_GE]    = { 0x1d, 0x0d },  /* GE_OQ, GE_OS */
    [TCG_FPPRED_UNORD] = { 0x03, 0x13 },  /* UNORD_Q, UNORD_S */
    [TCG_FPPRED_NE]    = { 0x04, 0x14 },  /* NEQ_UQ, NEQ_US */
    [TCG_FPPRED_LTGT]  = { 0x0c, 0x1c },  /* NEQ_OQ, NEQ_OS */
};
#define FPO_PRED_UNORD_Q    0x03
#define FPO_PRED_UNORD_S    0x13
#define FPO_PRED_ORD_Q      0x07
#define FPO_PRED_NEQ_OQ     0x0c
#define FPO_PRED_NGT_UQ     0x1a
#define FPO_PRED_GE_OQ      0x1d

/*
 * EVEX prefix + opcode.  @r, @v and @rm are hardware register numbers
 * (xmm0-31, k0-7 or a GPR, depending on the operand); @rm_reg says @rm
 * is a register (mod == 3) rather than memory.
 */
static void fpo_evex_opc_ll(TCGContext *s, int opc, int r, int v, int rm,
                            bool rm_reg, int aaa, bool z, bool bcst, int ll)
{
    int mm, pp, p0, p1, p2;

    tcg_debug_assert(have_avx512vl);
    mm = (opc & P_EXT3A) ? 3 : (opc & P_EXT38) ? 2 : 1;
    pp = (opc & P_DATA16) ? 1 : (opc & P_SIMDF3) ? 2 : (opc & P_SIMDF2) ? 3 : 0;

    p0 = mm;
    p0 |= (r & 8) ? 0 : 0x80;                   /* R */
    p0 |= (rm_reg && (rm & 16)) ? 0 : 0x40;     /* X: bit 4 of a reg rm */
    p0 |= (rm_reg && (rm & 8)) ? 0 : 0x20;      /* B */
    p0 |= (r & 16) ? 0 : 0x10;                  /* R' */
    p1 = pp | 0x04 | ((~v & 15) << 3) | ((opc & P_VEXW) ? 0x80 : 0);
    p2 = (aaa & 7) | ((v & 16) ? 0 : 0x08) | (bcst ? 0x10 : 0)
       | (ll << 5) | (z ? 0x80 : 0);

    tcg_out8(s, 0x62);
    tcg_out8(s, p0);
    tcg_out8(s, p1);
    tcg_out8(s, p2);
    tcg_out8(s, opc);
}

static void fpo_evex_opc(TCGContext *s, int opc, int r, int v, int rm,
                         bool rm_reg, int aaa, bool z, bool bcst)
{
    fpo_evex_opc_ll(s, opc, r, v, rm, rm_reg, aaa, z, bcst,
                    (opc & P_VEXL) ? 1 : 0);
}

/* op r{aaa}{z}, v, rm -- all registers */
static void fpo_rrr(TCGContext *s, int opc, int r, int v, int rm,
                    int aaa, bool z)
{
    fpo_evex_opc(s, opc, r, v, rm, true, aaa, z, false);
    tcg_out8(s, 0xc0 | ((r & 7) << 3) | (rm & 7));
}

static void fpo_rrri(TCGContext *s, int opc, int r, int v, int rm,
                     int aaa, bool z, uint8_t imm)
{
    fpo_rrr(s, opc, r, v, rm, aaa, z);
    tcg_out8(s, imm);
}

/*
 * op r, v, rm with static rounding @rc and all exceptions suppressed.
 * EVEX.b on a register operand turns L'L into the rounding control, so a
 * packed op works on the whole zmm registers: only for scratch results.
 */
static void fpo_rrr_er(TCGContext *s, int opc, int r, int v, int rm, int rc)
{
    fpo_evex_opc_ll(s, opc, r, v, rm, true, 0, false, true, rc);
    tcg_out8(s, 0xc0 | ((r & 7) << 3) | (rm & 7));
}

/* op r{aaa}{z}, [base]: vector register and memory at a GPR */
static void fpo_rm_base(TCGContext *s, int opc, int r, TCGReg base, int aaa,
                        bool z)
{
    int b = base & 7;

    /* the base register extension is EVEX.B, set from rm like a register */
    fpo_evex_opc(s, opc, r, 0, base & 8, true, aaa, z, false);
    if (b == 4) {                   /* rsp, r12: SIB */
        tcg_out8(s, ((r & 7) << 3) | 4);
        tcg_out8(s, 0x24);
    } else if (b == 5) {            /* rbp, r13: [base + 0] */
        tcg_out8(s, 0x40 | ((r & 7) << 3) | 5);
        tcg_out8(s, 0);
    } else {
        tcg_out8(s, ((r & 7) << 3) | b);
    }
}

/* op r, v, [rip + constant] without broadcast (scalar memory operand) */
static void fpo_rrm(TCGContext *s, int opc, int r, int v, uint64_t val)
{
    fpo_evex_opc(s, opc, r, v, 0, false, 0, false, false);
    tcg_out8(s, ((r & 7) << 3) | 5);
    tcg_out32(s, 0);
    new_pool_label(s, val, R_386_PC32, s->code_ptr - 4, -4);
}

/*
 * op r{aaa}{z}, v, [rip + constant]: the 64-bit constant is broadcast
 * ({1toN}) to every element of the EVEX.W size, whose low 32 bits are
 * used for 32-bit elements.
 */
static void fpo_rrc(TCGContext *s, int opc, int r, int v, int aaa, bool z,
                    uint64_t val)
{
    fpo_evex_opc(s, opc, r, v, 0, false, aaa, z, true);
    tcg_out8(s, ((r & 7) << 3) | 5);
    tcg_out32(s, 0);
    new_pool_label(s, val, R_386_PC32, s->code_ptr - 4, -4);
}

static void fpo_rrci(TCGContext *s, int opc, int r, int v, int aaa,
                     uint64_t val, uint8_t imm, bool bcst)
{
    fpo_evex_opc(s, opc, r, v, 0, false, aaa, false, bcst);
    tcg_out8(s, ((r & 7) << 3) | 5);
    tcg_out32(s, 0);
    new_pool_label(s, val, R_386_PC32, s->code_ptr - 4, -5);
    tcg_out8(s, imm);
}

/* r = constant broadcast to all elements (vpbroadcastq/d from memory) */
static void fpo_ldc(TCGContext *s, int r, bool dbl, uint64_t val)
{
    int opc = dbl ? (0x59 | P_EXT38 | P_DATA16 | P_VEXW)
                  : (0x58 | P_EXT38 | P_DATA16);

    fpo_evex_opc(s, opc, r, 0, 0, false, 0, false, false);
    tcg_out8(s, ((r & 7) << 3) | 5);
    tcg_out32(s, 0);
    new_pool_label(s, val, R_386_PC32, s->code_ptr - 4, -4);
}

/* Opmask ops: kd = op(k1, k2) */
static void fpo_kop(TCGContext *s, int opc, int kd, int k1, int k2)
{
    tcg_out_vex_modrm(s, opc, kd, k1, k2);
}

/* k = lane 0 only */
static void fpo_k_lane0(TCGContext *s, int k)
{
    fpo_kop(s, FPO_KXNORB, k, k, k);
    tcg_out_vex_modrm(s, FPO_KSHIFTRB, k, 0, k);
    tcg_out8(s, 7);
}

/* Short forward jumps within one op */
static uint8_t *fpo_jcc8(TCGContext *s, int cond)
{
    tcg_out8(s, cond == JCC_JMP ? OPC_JMP_short : OPC_JCC_short + cond);
    tcg_out8(s, 0);
    return s->code_ptr - 1;
}

static void fpo_patch8(TCGContext *s, uint8_t *p)
{
    ptrdiff_t d = s->code_ptr - (p + 1);
    tcg_debug_assert(d >= 0 && d < 128);
    *p = d;
}

/* The same with a 32-bit displacement, for longer fix-up sequences */
static uint8_t *fpo_jcc32(TCGContext *s, int cond)
{
    if (cond == JCC_JMP) {
        tcg_out8(s, OPC_JMP_long);
    } else {
        tcg_out8(s, 0x0f);
        tcg_out8(s, 0x80 + cond);           /* jcc rel32: 0f 8x */
    }
    tcg_out32(s, 0);
    return s->code_ptr - 4;
}

static void fpo_patch32(TCGContext *s, uint8_t *p)
{
    ptrdiff_t d = s->code_ptr - (p + 4);
    tcg_debug_assert(d >= 0 && d <= INT32_MAX);
    stl_le_p(p, d);
}

static int fpo_pfx(bool scalar, bool dbl)
{
    if (dbl) {
        return (scalar ? P_SIMDF2 : P_DATA16) | P_VEXW;
    }
    return scalar ? P_SIMDF3 : 0;
}

static uint64_t fpo_abs_mask(bool dbl)
{
    return dbl ? INT64_MAX : INT32_MAX;
}

static uint64_t fpo_sign_bit(bool dbl)
{
    return dbl ? INT64_MIN : 0x80000000u;
}

static uint64_t fpo_quiet_bit(bool dbl)
{
    return dbl ? 1ull << 51 : 1u << 22;
}

/* d{k} = constant broadcast to the elements of k */
static void fpo_ldc_k(TCGContext *s, int r, bool dbl, uint64_t val, int k)
{
    int opc = dbl ? (0x59 | P_EXT38 | P_DATA16 | P_VEXW)
                  : (0x58 | P_EXT38 | P_DATA16);

    fpo_evex_opc(s, opc, r, 0, 0, false, k, false, false);
    tcg_out8(s, ((r & 7) << 3) | 5);
    tcg_out32(s, 0);
    new_pool_label(s, val, R_386_PC32, s->code_ptr - 4, -4);
}

/* vfpclass classes */
#define FPO_CLS_QNAN    0x01
#define FPO_CLS_SNAN    0x80
#define FPO_CLS_NAN     0x81

/* k = fpclass(x) & classes: lane 0 for scalars, all the lanes otherwise */
static void fpo_class(TCGContext *s, int k, int x, bool scalar, bool dbl,
                      uint8_t cls)
{
    fpo_rrri(s, (FPO_FPCLASSP + (scalar ? 1 : 0)) | (dbl ? P_VEXW : 0),
             k, 0, x, 0, false, cls);
}

/* Scalar binary32 results: zero the rest of the register (lanes 1-3). */
static void fpo_zero_hi32(TCGContext *s, int d)
{
    fpo_rrri(s, FPO_INSERTPS, d, d, d, 0, false, 0x0e);
}

/*
 * x86 does not signal the invalid operation of 0 * inf + qNaN, which
 * PowerPC does (VXIMZ): in the lanes of @kl (0: all), raise the invalid
 * flag if in[0] * in[1] is NaN without a NaN operand.
 */
static void fpo_fma_imz(TCGContext *s, const int *in, bool scalar, bool dbl,
                        int kl)
{
    int cmp = FPO_CMP | fpo_pfx(scalar, dbl);

    /* the product, without flags */
    fpo_rrr_er(s, FPO_MUL | fpo_pfx(scalar, dbl), FPX_S3, in[0], in[1],
               FPO_RC_RN);
    fpo_rrri(s, cmp, FPK2, FPX_S3, FPX_S3, kl, false, FPO_PRED_UNORD_Q);
    fpo_rrri(s, cmp, FPK3, in[0], in[1], 0, false, FPO_PRED_ORD_Q);
    fpo_kop(s, FPO_KANDB, FPK2, FPK2, FPK3);
    /* signalling compare of that NaN: invalid, and only that */
    fpo_rrri(s, cmp, FPK3, FPX_S3, FPX_S3, FPK2, false, FPO_PRED_UNORD_S);
}

/*
 * The x86 default NaN is negative; make a NaN result positive when no
 * input was a NaN, i.e. when the operation generated it.  With @acb (FMA,
 * in[] = a, b, c) also give priority to c over b, as PowerPC does.
 */
static void fpo_fix_nan(TCGContext *s, int d, const int *in, int nin,
                        bool scalar, bool dbl, bool defnan, bool acb)
{
    int pand = dbl ? FPO_PANDQ : FPO_PANDD;
    int por = dbl ? FPO_PORQ : FPO_PORD;

    if (scalar) {
        int ucom = dbl ? FPO_UCOMISD : FPO_UCOMISS;
        uint8_t *done[6];
        int n = 0;

        /* Common case: the result is not a NaN. */
        fpo_rrr(s, ucom, d, 0, d, 0, false);
        done[n++] = fpo_jcc8(s, JCC_JNP);

        if (acb) {
            fpo_fma_imz(s, in, true, dbl, 0);
        }

        /* A NaN in the first input propagates, which x86 already did. */
        fpo_rrr(s, ucom, in[0], 0, in[0], 0, false);
        done[n++] = fpo_jcc8(s, JCC_JP);
        if (acb) {
            uint8_t *not_c;
            fpo_rrr(s, ucom, in[2], 0, in[2], 0, false);
            not_c = fpo_jcc8(s, JCC_JNP);
            fpo_rrc(s, por, d, in[2], 0, false, fpo_quiet_bit(dbl));
            done[n++] = fpo_jcc8(s, JCC_JMP);
            fpo_patch8(s, not_c);
            fpo_rrr(s, ucom, in[1], 0, in[1], 0, false);
            done[n++] = fpo_jcc8(s, JCC_JP);
        } else {
            for (int i = 1; i < nin; i++) {
                fpo_rrr(s, ucom, in[i], 0, in[i], 0, false);
                done[n++] = fpo_jcc8(s, JCC_JP);
            }
        }
        if (defnan) {
            fpo_rrc(s, pand, d, d, 0, false, fpo_abs_mask(dbl));
        }
        while (n > 0) {
            fpo_patch8(s, done[--n]);
        }
    } else {
        int cmp = FPO_CMP | fpo_pfx(false, dbl);
        uint8_t *done;

        fpo_rrri(s, cmp, FPK1, d, d, 0, false, FPO_PRED_UNORD_Q);
        tcg_out_vex_modrm(s, FPO_KORTESTB, FPK1, 0, FPK1);
        done = fpo_jcc8(s, JCC_JE);

        if (acb) {
            fpo_fma_imz(s, in, false, dbl, FPK1);
        }

        /* k2 = lanes where a is a NaN */
        fpo_rrri(s, cmp, FPK2, in[0], in[0], 0, false, FPO_PRED_UNORD_Q);
        if (acb) {
            /* k3 = c is NaN and a is not: the result is c, quieted */
            fpo_rrri(s, cmp, FPK3, in[2], in[2], 0, false, FPO_PRED_UNORD_Q);
            fpo_kop(s, FPO_KANDNB, FPK3, FPK2, FPK3);
            fpo_rrc(s, por, d, in[2], FPK3, false, fpo_quiet_bit(dbl));
            fpo_kop(s, FPO_KORB, FPK2, FPK2, FPK3);
        }
        for (int i = 1; i < nin; i++) {
            fpo_rrri(s, cmp, FPK3, in[i], in[i], 0, false, FPO_PRED_UNORD_Q);
            fpo_kop(s, FPO_KORB, FPK2, FPK2, FPK3);
        }
        if (defnan) {
            /* k1 = NaN results without NaN inputs */
            fpo_kop(s, FPO_KANDNB, FPK1, FPK2, FPK1);
            fpo_rrc(s, pand, d, d, FPK1, false, fpo_abs_mask(dbl));
        }
        fpo_patch8(s, done);
    }
}

/*
 * Arm NaN rules (TCG_FPOP_F_SNAN_FIRST), in the lanes where d is a NaN:
 * the first signalling NaN, else the first quiet NaN, of a, b (nin == 2)
 * or c, a, b (FMA, nin == 3), quieted; for FMA, 0 * inf + a quiet NaN
 * gives the default NaN (and invalid, which x86 does not raise); and a
 * NaN generated without NaN operands is the positive default NaN.
 * Branch-free with masks, also for scalars (the other lanes are don't
 * care, and these instructions raise no exceptions).
 */
static void fpo_fix_nan_arm(TCGContext *s, int d, const int *in, int nin,
                            bool scalar, bool dbl)
{
    int por = dbl ? FPO_PORQ : FPO_PORD;
    int cmp = FPO_CMP | fpo_pfx(scalar, dbl);
    uint64_t qb = fpo_quiet_bit(dbl);
    int order[3], n = 0;
    uint8_t *done;

    /* k1 = NaN results (lane 0 only for scalars); nothing to do without */
    fpo_rrri(s, cmp, FPK1, d, d, 0, false, FPO_PRED_UNORD_Q);
    tcg_out_vex_modrm(s, FPO_KORTESTB, FPK1, 0, FPK1);
    done = fpo_jcc32(s, JCC_JE);

    /* operands by increasing priority */
    if (nin == 3) {
        order[n++] = in[1];
        order[n++] = in[0];
        order[n++] = in[2];
    } else {
        for (int i = nin - 1; i >= 0; i--) {
            order[n++] = in[i];
        }
    }
    /* generated NaNs: k2 = results without NaN operands */
    fpo_class(s, FPK2, in[0], scalar, dbl, FPO_CLS_NAN);
    for (int i = 1; i < nin; i++) {
        fpo_class(s, FPK3, in[i], scalar, dbl, FPO_CLS_NAN);
        fpo_kop(s, FPO_KORB, FPK2, FPK2, FPK3);
    }
    fpo_kop(s, FPO_KANDNB, FPK2, FPK2, FPK1);
    fpo_rrc(s, dbl ? FPO_PANDQ : FPO_PANDD, d, d, FPK2, false,
            fpo_abs_mask(dbl));

    /* quiet NaNs, then signalling NaNs, the last one written wins */
    for (int i = 0; i < n; i++) {
        fpo_class(s, FPK3, order[i], scalar, dbl, FPO_CLS_NAN);
        fpo_rrc(s, por, d, order[i], FPK3, false, qb);
    }
    for (int i = 0; i < n; i++) {
        fpo_class(s, FPK3, order[i], scalar, dbl, FPO_CLS_SNAN);
        fpo_rrc(s, por, d, order[i], FPK3, false, qb);
    }

    if (nin == 3) {
        /* k2 = inf * 0, which also raises invalid */
        fpo_fma_imz(s, in, scalar, dbl, 0);
        fpo_class(s, FPK3, in[2], scalar, dbl, FPO_CLS_QNAN);
        fpo_kop(s, FPO_KANDB, FPK2, FPK2, FPK3);
        fpo_ldc_k(s, d, dbl, dbl ? 0x7ff8000000000000ull : 0x7fc00000u, FPK2);
    }
    fpo_patch32(s, done);
}

static void fpo_round_sp(TCGContext *s, int d, int a, bool scalar)
{
    if (scalar) {
        fpo_rrr(s, FPO_CVTSD2SS, d, a, a, 0, false);
        fpo_rrr(s, FPO_CVTSS2SD, d, d, d, 0, false);
    } else {
        fpo_rrr(s, FPO_CVTPD2PS, d, 0, a, 0, false);
        fpo_rrr(s, FPO_CVTPS2PD, d, 0, d, 0, false);
    }
}

/*
 * binary32 results of binary64 operands (TCG_FPOP_F_SP) must be rounded
 * once, from the exact result: rounding it to binary64 first can create a
 * tie (e.g. for operands that are not binary32 values, which PowerPC
 * accepts).  Rounding to odd instead is innocuous, since binary64 has more
 * than 24 + 1 bits.  With d = the op rounded to nearest (which raised the
 * flags), S0 = rounded up and S1 = rounded down (no flags), leave in S1
 * the exact result, or the result rounded toward zero with its last bit
 * set if it was inexact.
 */
static void fpo_round_odd(TCGContext *s, int d, bool scalar, bool dbl)
{
    int blend = dbl ? FPO_PBLENDMQ : FPO_PBLENDMD;

    /* k1 = inexact (+0 and -0 are one exact zero; NaNs never differ) */
    fpo_rrri(s, FPO_CMP | fpo_pfx(scalar, dbl), FPK1, FPX_S0, FPX_S1,
             0, false, FPO_PRED_NEQ_OQ);
    /* rounded toward zero: down if positive, up if negative */
    fpo_rrr(s, dbl ? FPO_PMOVQ2M : FPO_PMOVD2M, FPK2, 0, FPX_S1, 0, false);
    fpo_rrr(s, blend, FPX_S1, FPX_S1, FPX_S0, FPK2, false);
    fpo_rrc(s, dbl ? FPO_PORQ : FPO_PORD, FPX_S1, FPX_S1, 0, false, 1);
    /* S1 = inexact ? S1 : d */
    fpo_rrr(s, blend, FPX_S1, d, FPX_S1, FPK1, false);
}

/* Emit into @dst the operation, rounded toward zero, without flags. */
typedef struct FPOReop {
    int opc, a, b, c;       /* c < 0: binary op; else FMA132 on a, c, b */
} FPOReop;

static void fpo_reop_rz(TCGContext *s, int dst, const FPOReop *op)
{
    if (op->c < 0) {
        fpo_rrr_er(s, op->opc, dst, op->a, op->b, FPO_RC_RZ);
    } else {
        fpo_rrr(s, FPO_MOVDQA64, dst, 0, op->a, 0, false);
        fpo_rrr_er(s, op->opc, dst, op->c, op->b, FPO_RC_RZ);
    }
}

/*
 * PowerPC detects tininess before rounding, x86 after: a result that was
 * rounded up to the smallest normal number underflowed on PowerPC but
 * not here.  If |d| is that number, look at the exact result, @pre, or
 * the operation redone rounding toward zero, and raise the underflow (and
 * inexact) flags if it was below.  @dbl is the format of d and pre.
 */
static void fpo_tiny(TCGContext *s, int d, bool scalar, bool dbl,
                     uint64_t min, int pre, const FPOReop *op)
{
    int pand = dbl ? FPO_PANDQ : FPO_PANDD;
    uint64_t abs = fpo_abs_mask(dbl);
    uint8_t *skip[3];
    int n = 0;

    fpo_rrc(s, pand, FPX_S3, d, 0, false, abs);
    if (scalar) {
        fpo_rrm(s, dbl ? FPO_UCOMISD : FPO_UCOMISS, FPX_S3, 0, min);
        skip[n++] = fpo_jcc8(s, JCC_JNE);
        skip[n++] = fpo_jcc8(s, JCC_JP);
    } else {
        fpo_rrci(s, dbl ? FPO_PCMPQ : FPO_PCMPD, FPK1, FPX_S3, 0, min,
                 FPO_PCMP_EQ, true);
        tcg_out_vex_modrm(s, FPO_KORTESTB, FPK1, 0, FPK1);
        skip[n++] = fpo_jcc8(s, JCC_JE);
    }

    /* rare: |d| == min */
    if (pre < 0) {
        fpo_reop_rz(s, FPX_S3, op);
        pre = FPX_S3;
    }
    fpo_rrc(s, pand, FPX_S3, pre, 0, false, abs);
    if (scalar) {
        fpo_rrm(s, dbl ? FPO_UCOMISD : FPO_UCOMISS, FPX_S3, 0, min);
        skip[n++] = fpo_jcc8(s, JCC_JAE);
    } else {
        fpo_rrci(s, dbl ? FPO_PCMPQ : FPO_PCMPD, FPK2, FPX_S3, FPK1, min,
                 FPO_PCMP_LT, true);
        tcg_out_vex_modrm(s, FPO_KORTESTB, FPK2, 0, FPK2);
        skip[n++] = fpo_jcc8(s, JCC_JE);
    }
    /* DBL_MIN * DBL_MIN: underflow and inexact */
    fpo_ldc(s, FPX_S3, true, FPO_DBL_MIN);
    fpo_rrr(s, FPO_MUL | fpo_pfx(true, true), FPX_S3, FPX_S3, FPX_S3,
            0, false);
    while (n > 0) {
        fpo_patch8(s, skip[--n]);
    }
}

static void tcg_out_fpop2(TCGContext *s, TCGType type, unsigned vece,
                          int d, int a, int b, unsigned fpop)
{
    bool scalar = type == TCG_TYPE_V64;
    bool dbl = vece == MO_64;
    int pfx = fpo_pfx(scalar, dbl);
    int in[2] = { a, b };
    int opc;

    switch (TCG_FPOP_OP(fpop)) {
    case TCG_FPOP_ADD:
        opc = FPO_ADD;
        break;
    case TCG_FPOP_SUB:
        opc = FPO_SUB;
        break;
    case TCG_FPOP_MUL:
        opc = FPO_MUL;
        break;
    case TCG_FPOP_DIV:
        opc = FPO_DIV;
        break;
    case TCG_FPOP_MAXC:
        fpo_rrr(s, FPO_MAX | pfx, d, a, b, 0, false);
        return;
    case TCG_FPOP_MAXNUM:
    case TCG_FPOP_MINNUM:
        {
            /*
             * vrange orders -0 < +0 (sign from the selected operand); NaNs
             * are fixed up afterwards: a quiet NaN loses against a number,
             * otherwise the result is the first NaN operand, quieted.
             */
            int range = (FPO_RANGEP + (scalar ? 1 : 0)) | (dbl ? P_VEXW : 0);
            int cls = (FPO_FPCLASSP + (scalar ? 1 : 0)) | (dbl ? P_VEXW : 0);
            int blend = dbl ? FPO_PBLENDMQ : FPO_PBLENDMD;
            int por = dbl ? FPO_PORQ : FPO_PORD;
            bool max = TCG_FPOP_OP(fpop) == TCG_FPOP_MAXNUM;
            uint8_t *done;

            fpo_rrri(s, range, d, a, b, 0, false, max ? 0x05 : 0x04);
            fpo_rrri(s, cls, FPK1, 0, a, 0, false, 0x81);          /* a NaN */
            fpo_rrri(s, cls, FPK2, 0, b, 0, false, 0x81);          /* b NaN */
            tcg_out_vex_modrm(s, FPO_KORTESTB, FPK1, 0, FPK2);
            done = fpo_jcc32(s, JCC_JE);
            /* NaN lanes: quiet(a) if a is a NaN, else quiet(b) */
            fpo_rrc(s, por, d, b, FPK2, false, fpo_quiet_bit(dbl));
            fpo_rrc(s, por, d, a, FPK1, false, fpo_quiet_bit(dbl));
            /* a quiet NaN against a number: the number */
            fpo_rrri(s, cls, FPK3, 0, a, 0, false, 0x01);          /* a qNaN */
            fpo_kop(s, FPO_KANDNB, FPK3, FPK2, FPK3);              /* & !b NaN */
            fpo_rrr(s, blend, d, d, b, FPK3, false);
            fpo_rrri(s, cls, FPK3, 0, b, 0, false, 0x01);          /* b qNaN */
            fpo_kop(s, FPO_KANDNB, FPK3, FPK1, FPK3);              /* & !a NaN */
            fpo_rrr(s, blend, d, d, a, FPK3, false);
            if (fpop & TCG_FPOP_F_SNAN_FIRST) {
                /* Arm: a signalling b beats a quiet a */
                fpo_class(s, FPK3, b, scalar, dbl, FPO_CLS_SNAN);
                fpo_class(s, FPK1, a, scalar, dbl, FPO_CLS_SNAN);
                fpo_kop(s, FPO_KANDNB, FPK3, FPK1, FPK3);
                fpo_rrc(s, por, d, b, FPK3, false, fpo_quiet_bit(dbl));
            }
            fpo_patch32(s, done);
        }
        return;
    case TCG_FPOP_MAX:
    case TCG_FPOP_MIN:
        {
            /*
             * vrange orders -0 < +0 and signals for signalling NaNs; with
             * a NaN operand the result is the first NaN, quieted (Arm:
             * the first signalling NaN, if any).
             */
            int range = (FPO_RANGEP + (scalar ? 1 : 0)) | (dbl ? P_VEXW : 0);
            int por = dbl ? FPO_PORQ : FPO_PORD;
            bool max = TCG_FPOP_OP(fpop) == TCG_FPOP_MAX;
            uint8_t *done;

            fpo_rrri(s, range, d, a, b, 0, false, max ? 0x05 : 0x04);
            fpo_class(s, FPK1, a, scalar, dbl, FPO_CLS_NAN);
            fpo_class(s, FPK2, b, scalar, dbl, FPO_CLS_NAN);
            tcg_out_vex_modrm(s, FPO_KORTESTB, FPK1, 0, FPK2);
            done = fpo_jcc32(s, JCC_JE);
            fpo_rrc(s, por, d, b, FPK2, false, fpo_quiet_bit(dbl));
            fpo_rrc(s, por, d, a, FPK1, false, fpo_quiet_bit(dbl));
            if (fpop & TCG_FPOP_F_SNAN_FIRST) {
                fpo_class(s, FPK3, b, scalar, dbl, FPO_CLS_SNAN);
                fpo_class(s, FPK1, a, scalar, dbl, FPO_CLS_SNAN);
                fpo_kop(s, FPO_KANDNB, FPK3, FPK1, FPK3);
                fpo_rrc(s, por, d, b, FPK3, false, fpo_quiet_bit(dbl));
            }
            fpo_patch32(s, done);
        }
        return;
    case TCG_FPOP_MINC:
        fpo_rrr(s, FPO_MIN | pfx, d, a, b, 0, false);
        return;
    case TCG_FPOP_CMP:
        {
            unsigned pred = TCG_FPOP_GET_PRED(fpop);
            bool sig = fpop & TCG_FPOP_F_SIGNAL;
            int k = FPK1;
            /* Compare into k1, then expand k1 into an all-ones mask. */
            fpo_rrri(s, FPO_CMP | pfx, k, a, b, 0, false,
                     fpo_cmp_pred[pred][sig]);
            tcg_out_vex_modrm(s, (dbl ? OPC_VPMOVM2Q : OPC_VPMOVM2D),
                              d + TCG_REG_XMM0, 0, k);
        }
        return;
    default:
        g_assert_not_reached();
    }

    opc |= pfx;
    fpo_rrr(s, opc, d, a, b, 0, false);
    if (fpop & TCG_FPOP_F_SNAN_FIRST) {
        fpo_fix_nan_arm(s, d, in, 2, scalar, dbl);
    } else if (fpop & TCG_FPOP_F_DEFNAN) {
        fpo_fix_nan(s, d, in, 2, scalar, dbl, true, false);
    }
    if (fpop & TCG_FPOP_F_SP) {
        fpo_rrr_er(s, opc, FPX_S0, a, b, FPO_RC_RU);
        fpo_rrr_er(s, opc, FPX_S1, a, b, FPO_RC_RD);
        fpo_round_odd(s, d, scalar, dbl);
        fpo_round_sp(s, d, FPX_S1, scalar);
        fpo_tiny(s, d, scalar, dbl, FPO_FLT_MIN_D, FPX_S1, NULL);
    } else if (TCG_FPOP_OP(fpop) == TCG_FPOP_MUL
               || TCG_FPOP_OP(fpop) == TCG_FPOP_DIV) {
        /* the exact sum of two numbers below min normal is representable */
        FPOReop op = { opc, a, b, -1 };
        fpo_tiny(s, d, scalar, dbl, dbl ? FPO_DBL_MIN : FPO_FLT_MIN, -1, &op);
    }
}

static void tcg_out_fpop3(TCGContext *s, TCGType type, unsigned vece,
                          int d, int a, int b, int c, unsigned fpop)
{
    bool scalar = type == TCG_TYPE_V64;
    bool dbl = vece == MO_64;
    bool sp = fpop & TCG_FPOP_F_SP;
    int variant = (fpop & TCG_FPOP_F_NEG_PROD ? 4 : 0)
                + (fpop & TCG_FPOP_F_NEG_ADD ? 2 : 0);
    int fma = (FPO_FMA132 + variant + (scalar ? 1 : 0)) | (dbl ? P_VEXW : 0);
    int r = d, in[3] = { a, b, c };

    tcg_debug_assert(TCG_FPOP_OP(fpop) == TCG_FPOP_FMA);

    /* 132 form: d = d * b + c, with d = a first */
    fpo_rrr(s, FPO_MOVDQA64, d, 0, a, 0, false);
    fpo_rrr(s, fma, d, c, b, 0, false);
    if (sp) {
        /* binary64 FMA rounded to odd, then to binary32 */
        tcg_debug_assert(scalar && dbl);
        fpo_rrr(s, FPO_MOVDQA64, FPX_S0, 0, a, 0, false);
        fpo_rrr_er(s, fma, FPX_S0, c, b, FPO_RC_RU);
        fpo_rrr(s, FPO_MOVDQA64, FPX_S1, 0, a, 0, false);
        fpo_rrr_er(s, fma, FPX_S1, c, b, FPO_RC_RD);
        fpo_round_odd(s, d, scalar, dbl);
        r = FPX_S1;
    }

    if (fpop & TCG_FPOP_F_NEG_RES) {
        /* -(x) for the non-NaN results only */
        int cmp = FPO_CMP | fpo_pfx(scalar, dbl);
        fpo_rrri(s, cmp, FPK1, r, r, 0, false, FPO_PRED_ORD_Q);
        fpo_rrc(s, dbl ? FPO_PXORQ : FPO_PXORD, r, r, FPK1, false,
                fpo_sign_bit(dbl));
    }
    if (fpop & TCG_FPOP_F_SNAN_FIRST) {
        fpo_fix_nan_arm(s, r, in, 3, scalar, dbl);
    } else if (fpop & (TCG_FPOP_F_DEFNAN | TCG_FPOP_F_NAN_ACB)) {
        fpo_fix_nan(s, r, in, 3, scalar, dbl, fpop & TCG_FPOP_F_DEFNAN,
                    fpop & TCG_FPOP_F_NAN_ACB);
    }
    if (sp) {
        fpo_round_sp(s, d, r, scalar);
        fpo_tiny(s, d, scalar, dbl, FPO_FLT_MIN_D, r, NULL);
    } else {
        FPOReop op = { fma, a, b, c };
        fpo_tiny(s, d, scalar, dbl, dbl ? FPO_DBL_MIN : FPO_FLT_MIN, -1, &op);
    }
}

/* PowerPC saturation of fp -> int conversions, see TCG_FPOP_F_PPC_SAT */
static void fpo_cvti_sat(TCGContext *s, int d, int a, bool scalar,
                         bool dbl_src, bool sgn, unsigned ibits, int kl,
                         bool trunc)
{
    int cmp = FPO_CMP | fpo_pfx(scalar, dbl_src);
    bool q = ibits == 64;
    uint64_t lim;

    if (sgn) {
        /*
         * positive overflow gives 0x80..0 on x86: saturate.  Rounding to
         * nearest even, 2^31 - 0.5 already overflows int32; the other
         * limits are the next representable numbers anyway.
         */
        lim = dbl_src ? (ibits == 64 ? 0x43e0000000000000ull
                         : trunc ? 0x41e0000000000000ull
                         : 0x41dfffffffe00000ull)
                      : (ibits == 64 ? 0x5f000000u : 0x4f000000u);
        fpo_rrci(s, cmp, FPK2, a, kl, lim, FPO_PRED_GE_OQ, !scalar);
        fpo_rrc(s, q ? FPO_PBLENDMQ : FPO_PBLENDMD, d, d, FPK2, false,
                q ? INT64_MAX : INT32_MAX);
    } else {
        /* NaN, negative (overflow) give all-ones on x86: they become 0 */
        fpo_rrci(s, cmp, FPK2, a, kl, 0, FPO_PRED_NGT_UQ, !scalar);
        fpo_rrc(s, q ? FPO_PBLENDMQ : FPO_PBLENDMD, d, d, FPK2, false, 0);
    }
}

/* TCG_FPOP_F_NAN_ZERO: d = 0 in the lanes (of kl) where a is a NaN */
static void fpo_cvti_nan_zero(TCGContext *s, int d, int a, bool scalar,
                              bool dbl_src, bool q, int kl)
{
    fpo_rrri(s, FPO_CMP | fpo_pfx(scalar, dbl_src), FPK2, a, a, kl, false,
             FPO_PRED_UNORD_Q);
    fpo_rrc(s, q ? FPO_PBLENDMQ : FPO_PBLENDMD, d, d, FPK2, false, 0);
}

/*
 * binary64 -> binary32: underflow as with tininess before rounding (Arm,
 * PowerPC), when the result was rounded up to FLT_MIN from below.
 */
static void fpo_tiny_cvt(TCGContext *s, int d, int a)
{
    uint8_t *skip[3];
    int n = 0;

    fpo_rrc(s, FPO_PANDD, FPX_S3, d, 0, false, INT32_MAX);
    fpo_rrm(s, FPO_UCOMISS, FPX_S3, 0, FPO_FLT_MIN);
    skip[n++] = fpo_jcc8(s, JCC_JNE);
    skip[n++] = fpo_jcc8(s, JCC_JP);
    fpo_rrc(s, FPO_PANDQ, FPX_S3, a, 0, false, INT64_MAX);
    fpo_rrm(s, FPO_UCOMISD, FPX_S3, 0, FPO_FLT_MIN_D);
    skip[n++] = fpo_jcc8(s, JCC_JAE);
    /* DBL_MIN * DBL_MIN: underflow and inexact */
    fpo_ldc(s, FPX_S3, true, FPO_DBL_MIN);
    fpo_rrr(s, FPO_MUL | fpo_pfx(true, true), FPX_S3, FPX_S3, FPX_S3,
            0, false);
    while (n > 0) {
        fpo_patch8(s, skip[--n]);
    }
}

static void tcg_out_fpop1(TCGContext *s, TCGType type, unsigned vece,
                          int d, int a, unsigned fpop)
{
    bool scalar = type == TCG_TYPE_V64;
    bool dbl = vece == MO_64;
    bool trunc = fpop & TCG_FPOP_F_TRUNC;
    bool sat = fpop & TCG_FPOP_F_PPC_SAT;
    int pfx = fpo_pfx(scalar, dbl);
    int kl = 0;
    int opc;

    switch (TCG_FPOP_OP(fpop)) {
    case TCG_FPOP_SQRT:
        fpo_rrr(s, FPO_SQRT | pfx, d, scalar ? a : 0, a, 0, false);
        if (fpop & TCG_FPOP_F_SNAN_FIRST) {
            fpo_fix_nan_arm(s, d, &a, 1, scalar, dbl);
        } else if (fpop & TCG_FPOP_F_DEFNAN) {
            fpo_fix_nan(s, d, &a, 1, scalar, dbl, true, false);
        }
        if (fpop & TCG_FPOP_F_SP) {
            fpo_rrr_er(s, FPO_SQRT | pfx, FPX_S0, scalar ? a : 0, a,
                       FPO_RC_RU);
            fpo_rrr_er(s, FPO_SQRT | pfx, FPX_S1, scalar ? a : 0, a,
                       FPO_RC_RD);
            fpo_round_odd(s, d, scalar, dbl);
            fpo_round_sp(s, d, FPX_S1, scalar);
            fpo_tiny(s, d, scalar, dbl, FPO_FLT_MIN_D, FPX_S1, NULL);
        }
        return;

    case TCG_FPOP_RECIP:
    case TCG_FPOP_RSQRT:
        if (TCG_FPOP_OP(fpop) == TCG_FPOP_RSQRT) {
            fpo_rrr(s, FPO_SQRT | pfx, FPX_S0, scalar ? a : 0, a, 0, false);
            if (fpop & TCG_FPOP_F_DEFNAN) {
                fpo_fix_nan(s, FPX_S0, &a, 1, scalar, dbl, true, false);
            }
            a = FPX_S0;
        }
        /* d = 1.0 / a: a NaN only comes from a */
        fpo_ldc(s, FPX_S2, dbl, dbl ? 0x3ff0000000000000ull : 0x3f800000u);
        fpo_rrr(s, FPO_DIV | pfx, d, FPX_S2, a, 0, false);
        {
            FPOReop op = { FPO_DIV | pfx, FPX_S2, a, -1 };
            fpo_tiny(s, d, scalar, dbl, dbl ? FPO_DBL_MIN : FPO_FLT_MIN,
                     -1, &op);
        }
        return;

    case TCG_FPOP_RINT:
        {
            static const uint8_t mode[] = {
                [TCG_FPRND_NEAREST_EVEN] = 0x08,
                [TCG_FPRND_DOWN] = 0x09,
                [TCG_FPRND_UP] = 0x0a,
                [TCG_FPRND_ZERO] = 0x0b,
                [TCG_FPRND_CURRENT] = 0x04,
            };
            uint8_t imm = mode[TCG_FPOP_GET_RMODE(fpop)];
            if (scalar) {
                opc = dbl ? FPO_RNDSCALESD : FPO_RNDSCALESS;
                fpo_rrri(s, opc, d, a, a, 0, false, imm);
            } else {
                opc = dbl ? FPO_RNDSCALEPD : FPO_RNDSCALEPS;
                fpo_rrri(s, opc, d, 0, a, 0, false, imm);
            }
        }
        return;

    case TCG_FPOP_RND_SP:
        tcg_debug_assert(dbl);
        fpo_round_sp(s, d, a, scalar);
        fpo_tiny(s, d, scalar, dbl, FPO_FLT_MIN_D, a, NULL);
        return;

    case TCG_FPOP_CVT_S:
    case TCG_FPOP_CVT_U:
        {
            bool u = TCG_FPOP_OP(fpop) == TCG_FPOP_CVT_U;
            bool sp = fpop & TCG_FPOP_F_SP;
            if (scalar) {
                /* Packed conversion of lane 0 only: no spurious flags */
                kl = FPK1;
                fpo_k_lane0(s, kl);
                if (!dbl) {
                    opc = u ? FPO_CVTUDQ2PS : FPO_CVTDQ2PS;
                    fpo_rrr(s, opc, d, 0, a, kl, true);
                } else if (sp || (fpop & TCG_FPOP_F_RES32)) {
                    opc = u ? FPO_CVTUQQ2PS : FPO_CVTQQ2PS;
                    fpo_rrr(s, opc, d, 0, a, kl, true);
                    if (sp) {
                        fpo_rrr(s, FPO_CVTSS2SD, d, d, d, 0, false);
                    }
                } else {
                    opc = u ? FPO_CVTUQQ2PD : FPO_CVTQQ2PD;
                    fpo_rrr(s, opc, d, 0, a, kl, true);
                }
            } else if (dbl) {
                opc = u ? FPO_CVTUQQ2PD : FPO_CVTQQ2PD;
                fpo_rrr(s, opc, d, 0, a, 0, false);
                if (sp) {
                    fpo_round_sp(s, d, d, false);
                }
            } else {
                opc = u ? FPO_CVTUDQ2PS : FPO_CVTDQ2PS;
                fpo_rrr(s, opc, d, 0, a, 0, false);
            }
        }
        return;

    case TCG_FPOP_CVTI_S:
    case TCG_FPOP_CVTI_U:
        {
            bool u = TCG_FPOP_OP(fpop) == TCG_FPOP_CVTI_U;
            if (scalar) {
                kl = FPK1;
                fpo_k_lane0(s, kl);
            }
            if (dbl) {
                opc = u ? (trunc ? FPO_CVTTPD2UQQ : FPO_CVTPD2UQQ)
                        : (trunc ? FPO_CVTTPD2QQ : FPO_CVTPD2QQ);
            } else {
                opc = u ? (trunc ? FPO_CVTTPS2UDQ : FPO_CVTPS2UDQ)
                        : (trunc ? FPO_CVTTPS2DQ : FPO_CVTPS2DQ);
            }
            fpo_rrr(s, opc, d, 0, a, kl, kl != 0);
            if (sat) {
                fpo_cvti_sat(s, d, a, scalar, dbl, !u, dbl ? 64 : 32, kl,
                             trunc);
            }
            if ((fpop & TCG_FPOP_F_NAN_ZERO) && !u) {
                fpo_cvti_nan_zero(s, d, a, scalar, dbl, dbl, kl);
            }
        }
        return;

    case TCG_FPOP_CVTI_S64:
    case TCG_FPOP_CVTI_U64:
        {
            bool u = TCG_FPOP_OP(fpop) == TCG_FPOP_CVTI_U64;
            tcg_debug_assert(scalar && !dbl);
            kl = FPK1;
            fpo_k_lane0(s, kl);
            opc = u ? (trunc ? FPO_CVTTPS2UQQ : FPO_CVTPS2UQQ)
                    : (trunc ? FPO_CVTTPS2QQ : FPO_CVTPS2QQ);
            fpo_rrr(s, opc, d, 0, a, kl, true);
            if (sat) {
                fpo_cvti_sat(s, d, a, true, false, !u, 64, kl, trunc);
            }
            if ((fpop & TCG_FPOP_F_NAN_ZERO) && !u) {
                fpo_cvti_nan_zero(s, d, a, true, false, true, kl);
            }
        }
        return;

    case TCG_FPOP_CVT_F64:
        /* exact; a signalling NaN is quieted, with invalid */
        tcg_debug_assert(scalar && !dbl);
        fpo_rrr(s, FPO_CVTSS2SD, d, a, a, 0, false);
        return;

    case TCG_FPOP_CVT_F32:
        tcg_debug_assert(scalar && dbl);
        fpo_rrr(s, FPO_CVTSD2SS, d, a, a, 0, false);
        fpo_tiny_cvt(s, d, a);
        return;

    case TCG_FPOP_CVTI_S32:
    case TCG_FPOP_CVTI_U32:
        {
            bool u = TCG_FPOP_OP(fpop) == TCG_FPOP_CVTI_U32;
            tcg_debug_assert(scalar && dbl);
            kl = FPK1;
            fpo_k_lane0(s, kl);
            opc = u ? (trunc ? FPO_CVTTPD2UDQ : FPO_CVTPD2UDQ)
                    : (trunc ? FPO_CVTTPD2DQ : FPO_CVTPD2DQ);
            fpo_rrr(s, opc, d, 0, a, kl, true);
            if (sat) {
                fpo_cvti_sat(s, d, a, true, true, !u, 32, kl, trunc);
            }
            if (fpop & TCG_FPOP_F_DUP32) {
                /* vpshufd d, d, 0: dword 0 everywhere */
                fpo_rrri(s, 0x70 | P_EXT | P_DATA16, d, 0, d, 0, false, 0);
                return;
            }
            if ((fpop & TCG_FPOP_F_NAN_ZERO) && !u) {
                fpo_cvti_nan_zero(s, d, a, true, true, false, kl);
            }
            fpo_rrr(s, u ? FPO_PMOVZXDQ : FPO_PMOVSXDQ, d, 0, d, 0, false);
            if (sat && !u && !(fpop & TCG_FPOP_F_NAN_ZERO)) {
                /* PowerPC: a NaN gives 0x0000000080000000 */
                fpo_rrri(s, FPO_CMP | fpo_pfx(true, true), FPK2, a, a,
                         0, false, FPO_PRED_UNORD_Q);
                fpo_rrc(s, FPO_PBLENDMQ, d, d, FPK2, false, 0x80000000u);
            }
        }
        return;

    default:
        g_assert_not_reached();
    }
}

/* i32 d = TCG_FPCC_* of the scalar compare of a and b */
static void tcg_out_fpcmpcc(TCGContext *s, unsigned vece, TCGReg d,
                            int a, int b, unsigned fpop)
{
    bool dbl = vece == MO_64;
    bool sig = fpop & TCG_FPOP_F_SIGNAL;
    bool nzcv = fpop & TCG_FPOP_F_CC_NZCV;
    /* GT, then the flags that override it: CF, ZF, PF */
    static const uint32_t val[2][4] = {
        { TCG_FPCC_GT, TCG_FPCC_LT, TCG_FPCC_EQ, TCG_FPCC_UN },
        { 0x20000000, 0x80000000, 0x60000000, 0x30000000 },  /* Arm NZCV */
    };
    static const int cc[] = {
        JCC_JB,     /* CF: less, or unordered */
        JCC_JE,     /* ZF: equal, or unordered */
        JCC_JP,     /* PF: unordered */
    };

    fpo_rrr(s, dbl ? (sig ? FPO_COMISD : FPO_UCOMISD)
                   : (sig ? FPO_COMISS : FPO_UCOMISS), a, 0, b, 0, false);
    /* mov does not touch the flags; each cmov overrides the previous */
    tcg_out_opc(s, OPC_MOVL_Iv + LOWREGMASK(d), 0, d, 0);
    tcg_out32(s, val[nzcv][0]);
    for (int i = 0; i < ARRAY_SIZE(cc); i++) {
        tcg_out_opc(s, OPC_CMOVCC | cc[i], d, 0, 0);
        tcg_out8(s, (LOWREGMASK(d) << 3) | 5);
        tcg_out32(s, 0);
        new_pool_label(s, val[nzcv][i + 1], R_386_PC32, s->code_ptr - 4, -4);
    }
}

static bool tcg_target_can_emit_fpop(unsigned fpop, TCGType type,
                                     unsigned vece)
{
    bool scalar = type == TCG_TYPE_V64;

    if (!(have_avx512bw && have_avx512dq)) {
        return false;
    }
    if ((type != TCG_TYPE_V64 && type != TCG_TYPE_V128)
        || (vece != MO_32 && vece != MO_64)) {
        return false;
    }
    if ((fpop & TCG_FPOP_F_SP) && (vece != MO_64 || !scalar)) {
        /* rounded to odd with scalar ops only */
        return false;
    }

    switch (TCG_FPOP_OP(fpop)) {
    case TCG_FPOP_ADD:
    case TCG_FPOP_SUB:
    case TCG_FPOP_MUL:
    case TCG_FPOP_DIV:
    case TCG_FPOP_MAXC:
    case TCG_FPOP_MINC:
    case TCG_FPOP_MAXNUM:
    case TCG_FPOP_MINNUM:
    case TCG_FPOP_MAX:
    case TCG_FPOP_MIN:
    case TCG_FPOP_CMP:
    case TCG_FPOP_SQRT:
    case TCG_FPOP_RINT:
    case TCG_FPOP_CVTI_S:
    case TCG_FPOP_CVTI_U:
        return true;
    case TCG_FPOP_CVTI_S64:
    case TCG_FPOP_CVTI_U64:
    case TCG_FPOP_CVT_F64:
        return scalar && vece == MO_32;
    case TCG_FPOP_CVT_F32:
        return scalar && vece == MO_64;
    case TCG_FPOP_RECIP:
    case TCG_FPOP_RSQRT:
        /* no F_SP: it would need rounding to odd as well */
        return !(fpop & TCG_FPOP_F_SP);
    case TCG_FPOP_FMA:
        return !(fpop & TCG_FPOP_F_SP) || scalar;
    case TCG_FPOP_RND_SP:
        return vece == MO_64;
    case TCG_FPOP_CVT_S:
    case TCG_FPOP_CVT_U:
        if (fpop & TCG_FPOP_F_RES32) {
            return scalar && vece == MO_64;
        }
        return !(fpop & TCG_FPOP_F_SP) || vece == MO_64;
    case TCG_FPOP_CVTI_S32:
    case TCG_FPOP_CVTI_U32:
        return scalar && vece == MO_64;
    default:
        return false;
    }
}

/*
 * crypto_vec (tcg-crypto.h) with SHA-NI.  Its instructions have only a
 * legacy SSE encoding, which reaches xmm0-15, and SHA256RNDS2 takes its
 * third operand in xmm0: the operands are copied to xmm16-19, xmm0-2 are
 * saved in xmm20-22 while the SHA instructions use them, and the result
 * is built in xmm23 (EVEX moves reach all of them; the register
 * allocator only knows xmm0-15).
 */
#define FPO_SHUFPS      (0xc6 | P_EXT)
#define FPO_PSHUFD      (0x70 | P_EXT | P_DATA16)
#define FPO_PALIGNR     (0x0f | P_EXT3A | P_DATA16)
#define FPO_PADDD       (0xfe | P_EXT | P_DATA16)

/* legacy 0f 38 op xmm(r), xmm(rm), both in xmm0-7 */
static void fpo_sha(TCGContext *s, uint8_t op, int r, int rm)
{
    tcg_debug_assert(r < 8 && rm < 8);
    tcg_out8(s, 0x0f);
    tcg_out8(s, 0x38);
    tcg_out8(s, op);
    tcg_out8(s, 0xc0 | (r << 3) | rm);
}

#define FPO_SHA256RNDS2 0xcb
#define FPO_SHA256MSG1  0xcc
#define FPO_SHA256MSG2  0xcd

static void tcg_out_crypto(TCGContext *s, int d, int a, int b, int c,
                           unsigned op)
{
    const int res = 23;

    fpo_rrr(s, FPO_MOVDQA64, 17, 0, a, 0, false);
    fpo_rrr(s, FPO_MOVDQA64, 18, 0, b, 0, false);
    fpo_rrr(s, FPO_MOVDQA64, 19, 0, c, 0, false);
    for (int i = 0; i < 3; i++) {
        fpo_rrr(s, FPO_MOVDQA64, 20 + i, 0, i, 0, false);
    }

    switch (op) {
    case TCG_CRYPTO_SHA256H:
    case TCG_CRYPTO_SHA256H2:
        {
            /*
             * Arm keeps a in lane 0 of abcd and e in lane 0 of efgh; x86
             * works on ABEF = (F, E, B, A) and CDGH = (H, G, D, C), lane 0
             * first, two rounds at a time; after two rounds, CDGH is the
             * previous ABEF.
             */
            bool h2 = op == TCG_CRYPTO_SHA256H2;
            int x = h2 ? 18 : 17, y = h2 ? 17 : 18;

            fpo_rrr(s, FPO_MOVDQA64, 0, 0, 19, 0, false);   /* WK0, WK1 */
            fpo_rrri(s, FPO_SHUFPS, 1, y, x, 0, false, 0x11); /* ABEF */
            fpo_rrri(s, FPO_SHUFPS, 2, y, x, 0, false, 0xbb); /* CDGH */
            fpo_sha(s, FPO_SHA256RNDS2, 2, 1);              /* ABEF2 */
            fpo_rrri(s, FPO_PSHUFD, 0, 0, 19, 0, false, 0x0e); /* WK2, WK3 */
            fpo_sha(s, FPO_SHA256RNDS2, 1, 2);              /* ABEF4 */
            /* abcd = (A4, B4, A2, B2), efgh = (E4, F4, E2, F2) */
            fpo_rrri(s, FPO_SHUFPS, res, 1, 2, 0, false, h2 ? 0x11 : 0xbb);
        }
        break;
    case TCG_CRYPTO_SHA256SU0:
        /* SHA256MSG1 is SHA256SU0 */
        fpo_rrr(s, FPO_MOVDQA64, 1, 0, 17, 0, false);
        fpo_rrr(s, FPO_MOVDQA64, 2, 0, 18, 0, false);
        fpo_sha(s, FPO_SHA256MSG1, 1, 2);
        fpo_rrr(s, FPO_MOVDQA64, res, 0, 1, 0, false);
        break;
    case TCG_CRYPTO_SHA256SU1:
        /* SHA256MSG2(a + (b1, b2, b3, c0), c) */
        fpo_rrri(s, FPO_PALIGNR, 1, 19, 18, 0, false, 4);
        fpo_rrr(s, FPO_PADDD, 1, 1, 17, 0, false);
        fpo_rrr(s, FPO_MOVDQA64, 2, 0, 19, 0, false);
        fpo_sha(s, FPO_SHA256MSG2, 1, 2);
        fpo_rrr(s, FPO_MOVDQA64, res, 0, 1, 0, false);
        break;
    default:
        g_assert_not_reached();
    }

    for (int i = 0; i < 3; i++) {
        fpo_rrr(s, FPO_MOVDQA64, i, 0, 20 + i, 0, false);
    }
    fpo_rrr(s, FPO_MOVDQA64, d, 0, res, 0, false);
}

/* Is the result of a scalar (V64) op a binary32 or int32 element? */
static bool fpo_res32(unsigned vece, unsigned fpop)
{
    switch (TCG_FPOP_OP(fpop)) {
    case TCG_FPOP_CVT_F32:
        return true;
    case TCG_FPOP_CVT_F64:
    case TCG_FPOP_CVTI_S64:
    case TCG_FPOP_CVTI_U64:
    case TCG_FPOP_CVTI_S32:
    case TCG_FPOP_CVTI_U32:
        return false;
    case TCG_FPOP_CVT_S:
    case TCG_FPOP_CVT_U:
        return vece == MO_32 || (fpop & TCG_FPOP_F_RES32);
    default:
        return vece == MO_32;
    }
}

/* Dispatch from tcg_out_vec_op(); returns false if @opc is not ours. */
static bool tcg_out_fpop_vec(TCGContext *s, TCGOpcode opc, TCGType type,
                             unsigned vece, const TCGArg *args)
{
    switch (opc) {
    case INDEX_op_fpop1_vec:
        tcg_out_fpop1(s, type, vece, FPX(args[0]), FPX(args[1]), args[2]);
        if (type == TCG_TYPE_V64 && fpo_res32(vece, args[2])) {
            fpo_zero_hi32(s, FPX(args[0]));
        }
        return true;
    case INDEX_op_fpop2_vec:
        tcg_out_fpop2(s, type, vece, FPX(args[0]), FPX(args[1]),
                      FPX(args[2]), args[3]);
        if (type == TCG_TYPE_V64 && fpo_res32(vece, args[3])) {
            fpo_zero_hi32(s, FPX(args[0]));
        }
        return true;
    case INDEX_op_fpop3_vec:
        tcg_out_fpop3(s, type, vece, FPX(args[0]), FPX(args[1]),
                      FPX(args[2]), FPX(args[3]), args[4]);
        if (type == TCG_TYPE_V64 && fpo_res32(vece, args[4])) {
            fpo_zero_hi32(s, FPX(args[0]));
        }
        return true;
    case INDEX_op_fpcmpcc_vec:
        tcg_out_fpcmpcc(s, vece, args[0], FPX(args[1]), FPX(args[2]),
                        args[3]);
        return true;
    case INDEX_op_ldm_vec:
        /* vmovdqu8 d{k1}{z}, [ptr]: masked-off bytes do not fault */
        tcg_out_vex_modrm(s, FPO_KMOVW_KR, FPK1, 0, args[2]);
        fpo_rm_base(s, FPO_MOVDQU8_LD, FPX(args[0]), args[1], FPK1, true);
        return true;
    case INDEX_op_crypto_vec:
        tcg_out_crypto(s, FPX(args[0]), FPX(args[1]), FPX(args[2]),
                       FPX(args[3]), args[4]);
        return true;
    case INDEX_op_ld32_vec:
        /* vmovd x, [base + ofs] */
        tcg_out_vex_modrm_offset(s, OPC_MOVD_VyEy, args[0], 0, args[1],
                                 args[2]);
        return true;
    case INDEX_op_st32_vec:
        /* vmovd [base + ofs], x */
        tcg_out_vex_modrm_offset(s, OPC_MOVD_EyVy, args[0], 0, args[1],
                                 args[2]);
        return true;
    case INDEX_op_stm_vec:
        tcg_out_vex_modrm(s, FPO_KMOVW_KR, FPK1, 0, args[2]);
        fpo_rm_base(s, FPO_MOVDQU8_ST, FPX(args[0]), args[1], FPK1, false);
        return true;
    case INDEX_op_perm2b_vec:
        /* d = idx, then d = (a:b)[d] */
        fpo_rrr(s, FPO_MOVDQA64, FPX(args[0]), 0, FPX(args[3]), 0, false);
        fpo_rrr(s, FPO_PERMI2B, FPX(args[0]), FPX(args[1]), FPX(args[2]),
                0, false);
        return true;
    default:
        return false;
    }
}
