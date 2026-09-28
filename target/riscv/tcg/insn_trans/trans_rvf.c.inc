/*
 * RISC-V translation routines for the RV64F Standard Extension.
 *
 * Copyright (c) 2016-2017 Sagar Karandikar, sagark@eecs.berkeley.edu
 * Copyright (c) 2018 Peer Adelt, peer.adelt@hni.uni-paderborn.de
 *                    Bastian Koppelmann, kbastian@mail.uni-paderborn.de
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define REQUIRE_FPU do {\
    if (ctx->mstatus_fs == EXT_STATUS_DISABLED) {                           \
        ctx->virt_inst_excp = ctx->virt_enabled && ctx->cfg_ptr->ext_zfinx; \
        return false;                                                       \
    }                                                                       \
} while (0)

#define REQUIRE_ZFINX_OR_F(ctx) do {\
    if (!ctx->cfg_ptr->ext_zfinx) { \
        REQUIRE_EXT(ctx, RVF); \
    } \
} while (0)

#define REQUIRE_ZCF_OR_FC(ctx) do {                     \
    if (!ctx->cfg_ptr->ext_zcf) {                       \
        if (!has_ext(ctx, RVF) || !has_ext(ctx, RVC)) { \
            return false;                               \
        }                                               \
    }                                                   \
} while (0)

/*
 * F and D instructions in host FP code (user mode, riscv64), see
 * TCG_FPOP_F_RISCV in tcg/tcg-fpop.h, when they round to nearest even:
 * rm RNE, or DYN with frm RNE, which is in the TB flags (writing frm ends
 * the TB).  Their exception flags accumulate in the host FP status
 * register, see riscv_cpu_get_fflags().  The registers stay in their i64
 * globals, binary32 values NaN-boxed: the fpop*_i64 ops take them there.
 * Each fpj_* returns false to fall back to the helper.
 */
#if defined(CONFIG_USER_ONLY) && defined(TARGET_RISCV64)
/* rm < 0: no rounding, or rounding toward zero done by the op itself */
static bool fpj_ok(DisasContext *ctx, MemOp esz, unsigned fpop, int rm)
{
    if (!ctx->fpj || get_xl(ctx) != MXL_RV64) {
        return false;
    }
    if (rm >= 0 && rm != RISCV_FRM_RNE
        && !(rm == RISCV_FRM_DYN && ctx->fpj_frm == RISCV_FRM_RNE)) {
        return false;
    }
    return tcg_can_emit_fpop(fpop | TCG_FPOP_F_RISCV, TCG_TYPE_V64, esz);
}

/* a source register: a binary32 that is not NaN-boxed is the default NaN */
static TCGv_i64 fpj_src(MemOp esz, int rs)
{
    TCGv_i64 t;

    if (esz == MO_64) {
        return cpu_fpr[rs];
    }
    t = tcg_temp_new_i64();
    gen_check_nanbox_s(t, cpu_fpr[rs]);
    return t;
}

static void fpj_set(DisasContext *ctx, MemOp esz, int rd, TCGv_i64 t)
{
    if (esz == MO_64) {
        tcg_gen_mov_i64(cpu_fpr[rd], t);
    } else {
        gen_nanbox_s(cpu_fpr[rd], t);
    }
    mark_fs_dirty(ctx);
}

/* fadd, fsub, fmul, fdiv; fmin, fmax (MINNUM, MAXNUM) with rm = -1 */
static bool fpj_op2(DisasContext *ctx, MemOp esz, TCGFPOp op, int rd,
                    int rs1, int rs2, int rm)
{
    TCGv_i64 t;

    if (!fpj_ok(ctx, esz, op, rm)) {
        return false;
    }
    t = tcg_temp_new_i64();
    tcg_gen_fpop2_i64(esz, t, fpj_src(esz, rs1), fpj_src(esz, rs2),
                      op | TCG_FPOP_F_RISCV);
    fpj_set(ctx, esz, rd, t);
    return true;
}

static bool fpj_sqrt(DisasContext *ctx, MemOp esz, int rd, int rs1, int rm)
{
    TCGv_i64 t;

    if (!fpj_ok(ctx, esz, TCG_FPOP_SQRT, rm)) {
        return false;
    }
    t = tcg_temp_new_i64();
    tcg_gen_fpop1_i64(esz, t, fpj_src(esz, rs1),
                      TCG_FPOP_SQRT | TCG_FPOP_F_RISCV);
    fpj_set(ctx, esz, rd, t);
    return true;
}

/*
 * fmadd: neg 0; fmsub: TCG_FPOP_F_NEG_ADD; fnmsub: TCG_FPOP_F_NEG_PROD;
 * fnmadd: both
 */
static bool fpj_fma(DisasContext *ctx, MemOp esz, unsigned neg, int rd,
                    int rs1, int rs2, int rs3, int rm)
{
    unsigned fpop = TCG_FPOP_FMA | neg;
    TCGv_i64 t;

    if (!fpj_ok(ctx, esz, fpop, rm)) {
        return false;
    }
    t = tcg_temp_new_i64();
    tcg_gen_fpop3_i64(esz, t, fpj_src(esz, rs1), fpj_src(esz, rs2),
                      fpj_src(esz, rs3), fpop | TCG_FPOP_F_RISCV);
    fpj_set(ctx, esz, rd, t);
    return true;
}

/* feq (quiet), flt, fle (signalling): 0 or 1 in rd */
static bool fpj_cmp(DisasContext *ctx, MemOp esz, TCGFPPred pred, int rd,
                    int rs1, int rs2)
{
    unsigned fpop = TCG_FPOP_CMP | TCG_FPOP_PRED(pred)
                    | (pred != TCG_FPPRED_EQ ? TCG_FPOP_F_SIGNAL : 0);
    TCGv dest;

    if (!fpj_ok(ctx, esz, fpop, -1)) {
        return false;
    }
    dest = dest_gpr(ctx, rd);
    tcg_gen_fpop2_i64(esz, dest, fpj_src(esz, rs1), fpj_src(esz, rs2),
                      fpop | TCG_FPOP_F_RISCV);
    gen_set_gpr(ctx, rd, dest);
    return true;
}

/*
 * fcvt.{w,wu,l,lu}.{s,d}: @op the TCG_FPOP_CVTI_* of the formats, the
 * 32-bit results sign-extended (@sext32), even unsigned ones.
 */
static bool fpj_cvti(DisasContext *ctx, MemOp esz, TCGFPOp op, bool sext32,
                     int rd, int rs1, int rm)
{
    bool rtz = rm == RISCV_FRM_RTZ;
    unsigned fpop = op | (rtz ? TCG_FPOP_F_TRUNC : 0);
    TCGv dest;

    if (!fpj_ok(ctx, esz, fpop, rtz ? -1 : rm)) {
        return false;
    }
    dest = dest_gpr(ctx, rd);
    tcg_gen_fpop1_i64(esz, dest, fpj_src(esz, rs1), fpop | TCG_FPOP_F_RISCV);
    if (sext32) {
        tcg_gen_ext32s_i64(dest, dest);
    }
    gen_set_gpr(ctx, rd, dest);
    return true;
}

/*
 * fcvt.{s,d}.{w,wu,l,lu}: TCG_FPOP_CVT_S or CVT_U (@fpop, maybe with
 * F_RES32) of the integer of size @isz in rs1, its low 32 bits first
 * extended per @ext (EXT_SIGN, EXT_ZERO, or EXT_NONE), to the format @fsz.
 */
static bool fpj_cvtf(DisasContext *ctx, MemOp fsz, MemOp isz,
                     unsigned fpop, DisasExtend ext, int rd, int rs1, int rm)
{
    TCGv_i64 src = get_gpr(ctx, rs1, EXT_NONE);
    TCGv_i64 t;

    if (!fpj_ok(ctx, isz, fpop, rm)) {
        return false;
    }
    t = tcg_temp_new_i64();
    if (ext == EXT_SIGN) {
        tcg_gen_ext32s_i64(t, src);
        src = t;
    } else if (ext == EXT_ZERO) {
        tcg_gen_ext32u_i64(t, src);
        src = t;
    }
    tcg_gen_fpop1_i64(isz, t, src, fpop | TCG_FPOP_F_RISCV);
    fpj_set(ctx, fsz, rd, t);
    return true;
}

/* fcvt.s.d (to MO_32), fcvt.d.s (to MO_64) */
static bool fpj_cvt_fmt(DisasContext *ctx, MemOp to, int rd, int rs1, int rm)
{
    MemOp from = to == MO_32 ? MO_64 : MO_32;
    unsigned fpop = to == MO_32 ? TCG_FPOP_CVT_F32 : TCG_FPOP_CVT_F64;
    TCGv_i64 t;

    if (!fpj_ok(ctx, from, fpop, rm)) {
        return false;
    }
    t = tcg_temp_new_i64();
    tcg_gen_fpop1_i64(from, t, fpj_src(from, rs1), fpop | TCG_FPOP_F_RISCV);
    fpj_set(ctx, to, rd, t);
    return true;
}
#else
#define fpj_op2(ctx, esz, op, rd, rs1, rs2, rm)             false
#define fpj_sqrt(ctx, esz, rd, rs1, rm)                     false
#define fpj_fma(ctx, esz, neg, rd, rs1, rs2, rs3, rm)       false
#define fpj_cmp(ctx, esz, pred, rd, rs1, rs2)               false
#define fpj_cvti(ctx, esz, op, sext32, rd, rs1, rm)         false
#define fpj_cvtf(ctx, fsz, isz, fpop, ext, rd, rs1, rm)     false
#define fpj_cvt_fmt(ctx, to, rd, rs1, rm)                   false
#endif

static bool trans_flw(DisasContext *ctx, arg_flw *a)
{
    TCGv_i64 dest;
    TCGv addr;
    MemOp memop = MO_UL;

    REQUIRE_FPU;
    REQUIRE_EXT(ctx, RVF);

    memop |= ctx->mo_endianness;
    if (ctx->cfg_ptr->ext_zama16b) {
        memop |= MO_ATOM_WITHIN16;
    }

    decode_save_opc(ctx, 0);
    addr = get_address(ctx, a->rs1, a->imm);
    dest = cpu_fpr[a->rd];
    tcg_gen_qemu_ld_i64(dest, addr, ctx->mem_idx, memop);
    gen_nanbox_s(dest, dest);

    mark_fs_dirty(ctx);
    return true;
}

static bool trans_fsw(DisasContext *ctx, arg_fsw *a)
{
    TCGv addr;
    MemOp memop = MO_UL;

    REQUIRE_FPU;
    REQUIRE_EXT(ctx, RVF);

    memop |= ctx->mo_endianness;
    if (ctx->cfg_ptr->ext_zama16b) {
        memop |= MO_ATOM_WITHIN16;
    }

    decode_save_opc(ctx, 0);
    addr = get_address(ctx, a->rs1, a->imm);
    tcg_gen_qemu_st_i64(cpu_fpr[a->rs2], addr, ctx->mem_idx, memop);
    return true;
}

static bool trans_c_flw(DisasContext *ctx, arg_flw *a)
{
    REQUIRE_ZCF_OR_FC(ctx);
    return trans_flw(ctx, a);
}

static bool trans_c_fsw(DisasContext *ctx, arg_fsw *a)
{
    REQUIRE_ZCF_OR_FC(ctx);
    return trans_fsw(ctx, a);
}

static bool trans_fmadd_s(DisasContext *ctx, arg_fmadd_s *a)
{
    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    if (fpj_fma(ctx, MO_32, 0, a->rd, a->rs1, a->rs2, a->rs3, a->rm)) {
        return true;
    }

    TCGv_i64 dest = dest_fpr(ctx, a->rd);
    TCGv_i64 src1 = get_fpr_hs(ctx, a->rs1);
    TCGv_i64 src2 = get_fpr_hs(ctx, a->rs2);
    TCGv_i64 src3 = get_fpr_hs(ctx, a->rs3);

    gen_set_rm(ctx, a->rm);
    gen_helper_fmadd_s(dest, tcg_env, src1, src2, src3);
    gen_set_fpr_hs(ctx, a->rd, dest);
    mark_fs_dirty(ctx);
    return true;
}

static bool trans_fmsub_s(DisasContext *ctx, arg_fmsub_s *a)
{
    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    if (fpj_fma(ctx, MO_32, TCG_FPOP_F_NEG_ADD, a->rd, a->rs1, a->rs2, a->rs3,
            a->rm)) {
        return true;
    }

    TCGv_i64 dest = dest_fpr(ctx, a->rd);
    TCGv_i64 src1 = get_fpr_hs(ctx, a->rs1);
    TCGv_i64 src2 = get_fpr_hs(ctx, a->rs2);
    TCGv_i64 src3 = get_fpr_hs(ctx, a->rs3);

    gen_set_rm(ctx, a->rm);
    gen_helper_fmsub_s(dest, tcg_env, src1, src2, src3);
    gen_set_fpr_hs(ctx, a->rd, dest);
    mark_fs_dirty(ctx);
    return true;
}

static bool trans_fnmsub_s(DisasContext *ctx, arg_fnmsub_s *a)
{
    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    if (fpj_fma(ctx, MO_32, TCG_FPOP_F_NEG_PROD, a->rd, a->rs1, a->rs2,
            a->rs3, a->rm)) {
        return true;
    }

    TCGv_i64 dest = dest_fpr(ctx, a->rd);
    TCGv_i64 src1 = get_fpr_hs(ctx, a->rs1);
    TCGv_i64 src2 = get_fpr_hs(ctx, a->rs2);
    TCGv_i64 src3 = get_fpr_hs(ctx, a->rs3);

    gen_set_rm(ctx, a->rm);
    gen_helper_fnmsub_s(dest, tcg_env, src1, src2, src3);
    gen_set_fpr_hs(ctx, a->rd, dest);
    mark_fs_dirty(ctx);
    return true;
}

static bool trans_fnmadd_s(DisasContext *ctx, arg_fnmadd_s *a)
{
    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    if (fpj_fma(ctx, MO_32, TCG_FPOP_F_NEG_PROD | TCG_FPOP_F_NEG_ADD, a->rd,
            a->rs1, a->rs2, a->rs3, a->rm)) {
        return true;
    }

    TCGv_i64 dest = dest_fpr(ctx, a->rd);
    TCGv_i64 src1 = get_fpr_hs(ctx, a->rs1);
    TCGv_i64 src2 = get_fpr_hs(ctx, a->rs2);
    TCGv_i64 src3 = get_fpr_hs(ctx, a->rs3);

    gen_set_rm(ctx, a->rm);
    gen_helper_fnmadd_s(dest, tcg_env, src1, src2, src3);
    gen_set_fpr_hs(ctx, a->rd, dest);
    mark_fs_dirty(ctx);
    return true;
}

static bool trans_fadd_s(DisasContext *ctx, arg_fadd_s *a)
{
    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    if (fpj_op2(ctx, MO_32, TCG_FPOP_ADD, a->rd, a->rs1, a->rs2, a->rm)) {
        return true;
    }

    TCGv_i64 dest = dest_fpr(ctx, a->rd);
    TCGv_i64 src1 = get_fpr_hs(ctx, a->rs1);
    TCGv_i64 src2 = get_fpr_hs(ctx, a->rs2);

    gen_set_rm(ctx, a->rm);
    gen_helper_fadd_s(dest, tcg_env, src1, src2);
    gen_set_fpr_hs(ctx, a->rd, dest);
    mark_fs_dirty(ctx);
    return true;
}

static bool trans_fsub_s(DisasContext *ctx, arg_fsub_s *a)
{
    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    if (fpj_op2(ctx, MO_32, TCG_FPOP_SUB, a->rd, a->rs1, a->rs2, a->rm)) {
        return true;
    }

    TCGv_i64 dest = dest_fpr(ctx, a->rd);
    TCGv_i64 src1 = get_fpr_hs(ctx, a->rs1);
    TCGv_i64 src2 = get_fpr_hs(ctx, a->rs2);

    gen_set_rm(ctx, a->rm);
    gen_helper_fsub_s(dest, tcg_env, src1, src2);
    gen_set_fpr_hs(ctx, a->rd, dest);
    mark_fs_dirty(ctx);
    return true;
}

static bool trans_fmul_s(DisasContext *ctx, arg_fmul_s *a)
{
    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    if (fpj_op2(ctx, MO_32, TCG_FPOP_MUL, a->rd, a->rs1, a->rs2, a->rm)) {
        return true;
    }

    TCGv_i64 dest = dest_fpr(ctx, a->rd);
    TCGv_i64 src1 = get_fpr_hs(ctx, a->rs1);
    TCGv_i64 src2 = get_fpr_hs(ctx, a->rs2);

    gen_set_rm(ctx, a->rm);
    gen_helper_fmul_s(dest, tcg_env, src1, src2);
    gen_set_fpr_hs(ctx, a->rd, dest);
    mark_fs_dirty(ctx);
    return true;
}

static bool trans_fdiv_s(DisasContext *ctx, arg_fdiv_s *a)
{
    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    if (fpj_op2(ctx, MO_32, TCG_FPOP_DIV, a->rd, a->rs1, a->rs2, a->rm)) {
        return true;
    }

    TCGv_i64 dest = dest_fpr(ctx, a->rd);
    TCGv_i64 src1 = get_fpr_hs(ctx, a->rs1);
    TCGv_i64 src2 = get_fpr_hs(ctx, a->rs2);

    gen_set_rm(ctx, a->rm);
    gen_helper_fdiv_s(dest, tcg_env, src1, src2);
    gen_set_fpr_hs(ctx, a->rd, dest);
    mark_fs_dirty(ctx);
    return true;
}

static bool trans_fsqrt_s(DisasContext *ctx, arg_fsqrt_s *a)
{
    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    if (fpj_sqrt(ctx, MO_32, a->rd, a->rs1, a->rm)) {
        return true;
    }

    TCGv_i64 dest = dest_fpr(ctx, a->rd);
    TCGv_i64 src1 = get_fpr_hs(ctx, a->rs1);

    gen_set_rm(ctx, a->rm);
    gen_helper_fsqrt_s(dest, tcg_env, src1);
    gen_set_fpr_hs(ctx, a->rd, dest);
    mark_fs_dirty(ctx);
    return true;
}

static bool trans_fsgnj_s(DisasContext *ctx, arg_fsgnj_s *a)
{
    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    TCGv_i64 dest = dest_fpr(ctx, a->rd);
    TCGv_i64 src1 = get_fpr_hs(ctx, a->rs1);

    if (a->rs1 == a->rs2) { /* FMOV */
        if (!ctx->cfg_ptr->ext_zfinx) {
            gen_check_nanbox_s(dest, src1);
        } else {
            tcg_gen_ext32s_i64(dest, src1);
        }
    } else { /* FSGNJ */
        TCGv_i64 src2 = get_fpr_hs(ctx, a->rs2);

        if (!ctx->cfg_ptr->ext_zfinx) {
            TCGv_i64 rs1 = tcg_temp_new_i64();
            TCGv_i64 rs2 = tcg_temp_new_i64();
            gen_check_nanbox_s(rs1, src1);
            gen_check_nanbox_s(rs2, src2);

            /* This formulation retains the nanboxing of rs2 in normal 'F'. */
            tcg_gen_deposit_i64(dest, rs2, rs1, 0, 31);
        } else {
            tcg_gen_deposit_i64(dest, src2, src1, 0, 31);
            tcg_gen_ext32s_i64(dest, dest);
        }
    }
    gen_set_fpr_hs(ctx, a->rd, dest);
    mark_fs_dirty(ctx);
    return true;
}

static bool trans_fsgnjn_s(DisasContext *ctx, arg_fsgnjn_s *a)
{
    TCGv_i64 rs1, rs2, mask;

    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    TCGv_i64 dest = dest_fpr(ctx, a->rd);
    TCGv_i64 src1 = get_fpr_hs(ctx, a->rs1);

    rs1 = tcg_temp_new_i64();
    if (!ctx->cfg_ptr->ext_zfinx) {
        gen_check_nanbox_s(rs1, src1);
    } else {
        tcg_gen_mov_i64(rs1, src1);
    }
    if (a->rs1 == a->rs2) { /* FNEG */
        tcg_gen_xori_i64(dest, rs1, MAKE_64BIT_MASK(31, 1));
    } else {
        TCGv_i64 src2 = get_fpr_hs(ctx, a->rs2);
        rs2 = tcg_temp_new_i64();
        if (!ctx->cfg_ptr->ext_zfinx) {
            gen_check_nanbox_s(rs2, src2);
        } else {
            tcg_gen_mov_i64(rs2, src2);
        }

        /*
         * Replace bit 31 in rs1 with inverse in rs2.
         * This formulation retains the nanboxing of rs1.
         */
        mask = tcg_constant_i64(~MAKE_64BIT_MASK(31, 1));
        tcg_gen_nor_i64(rs2, rs2, mask);
        tcg_gen_and_i64(dest, mask, rs1);
        tcg_gen_or_i64(dest, dest, rs2);
    }
    /* signed-extended instead of nanboxing for result if enable zfinx */
    if (ctx->cfg_ptr->ext_zfinx) {
        tcg_gen_ext32s_i64(dest, dest);
    }
    gen_set_fpr_hs(ctx, a->rd, dest);
    mark_fs_dirty(ctx);
    return true;
}

static bool trans_fsgnjx_s(DisasContext *ctx, arg_fsgnjx_s *a)
{
    TCGv_i64 rs1, rs2;

    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    TCGv_i64 dest = dest_fpr(ctx, a->rd);
    TCGv_i64 src1 = get_fpr_hs(ctx, a->rs1);
    rs1 = tcg_temp_new_i64();

    if (!ctx->cfg_ptr->ext_zfinx) {
        gen_check_nanbox_s(rs1, src1);
    } else {
        tcg_gen_mov_i64(rs1, src1);
    }

    if (a->rs1 == a->rs2) { /* FABS */
        tcg_gen_andi_i64(dest, rs1, ~MAKE_64BIT_MASK(31, 1));
    } else {
        TCGv_i64 src2 = get_fpr_hs(ctx, a->rs2);
        rs2 = tcg_temp_new_i64();

        if (!ctx->cfg_ptr->ext_zfinx) {
            gen_check_nanbox_s(rs2, src2);
        } else {
            tcg_gen_mov_i64(rs2, src2);
        }

        /*
         * Xor bit 31 in rs1 with that in rs2.
         * This formulation retains the nanboxing of rs1.
         */
        tcg_gen_andi_i64(dest, rs2, MAKE_64BIT_MASK(31, 1));
        tcg_gen_xor_i64(dest, rs1, dest);
    }
    /* signed-extended instead of nanboxing for result if enable zfinx */
    if (ctx->cfg_ptr->ext_zfinx) {
        tcg_gen_ext32s_i64(dest, dest);
    }
    gen_set_fpr_hs(ctx, a->rd, dest);
    mark_fs_dirty(ctx);
    return true;
}

static bool trans_fmin_s(DisasContext *ctx, arg_fmin_s *a)
{
    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    if (ctx->priv_ver >= PRIV_VERSION_1_11_0 && fpj_op2(ctx, MO_32,
            TCG_FPOP_MINNUM, a->rd, a->rs1, a->rs2, -1)) {
        return true;
    }

    TCGv_i64 dest = dest_fpr(ctx, a->rd);
    TCGv_i64 src1 = get_fpr_hs(ctx, a->rs1);
    TCGv_i64 src2 = get_fpr_hs(ctx, a->rs2);

    gen_helper_fmin_s(dest, tcg_env, src1, src2);
    gen_set_fpr_hs(ctx, a->rd, dest);
    mark_fs_dirty(ctx);
    return true;
}

static bool trans_fmax_s(DisasContext *ctx, arg_fmax_s *a)
{
    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    if (ctx->priv_ver >= PRIV_VERSION_1_11_0 && fpj_op2(ctx, MO_32,
            TCG_FPOP_MAXNUM, a->rd, a->rs1, a->rs2, -1)) {
        return true;
    }

    TCGv_i64 dest = dest_fpr(ctx, a->rd);
    TCGv_i64 src1 = get_fpr_hs(ctx, a->rs1);
    TCGv_i64 src2 = get_fpr_hs(ctx, a->rs2);

    gen_helper_fmax_s(dest, tcg_env, src1, src2);
    gen_set_fpr_hs(ctx, a->rd, dest);
    mark_fs_dirty(ctx);
    return true;
}

static bool trans_fcvt_w_s(DisasContext *ctx, arg_fcvt_w_s *a)
{
    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    if (fpj_cvti(ctx, MO_32, TCG_FPOP_CVTI_S, true, a->rd, a->rs1, a->rm)) {
        return true;
    }

    TCGv dest = dest_gpr(ctx, a->rd);
    TCGv_i64 src1 = get_fpr_hs(ctx, a->rs1);

    gen_set_rm(ctx, a->rm);
    gen_helper_fcvt_w_s(dest, tcg_env, src1);
    gen_set_gpr(ctx, a->rd, dest);
    return true;
}

static bool trans_fcvt_wu_s(DisasContext *ctx, arg_fcvt_wu_s *a)
{
    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    if (fpj_cvti(ctx, MO_32, TCG_FPOP_CVTI_U, true, a->rd, a->rs1, a->rm)) {
        return true;
    }

    TCGv dest = dest_gpr(ctx, a->rd);
    TCGv_i64 src1 = get_fpr_hs(ctx, a->rs1);

    gen_set_rm(ctx, a->rm);
    gen_helper_fcvt_wu_s(dest, tcg_env, src1);
    gen_set_gpr(ctx, a->rd, dest);
    return true;
}

static bool trans_fmv_x_w(DisasContext *ctx, arg_fmv_x_w *a)
{
    /* NOTE: This was FMV.X.S in an earlier version of the ISA spec! */
    REQUIRE_FPU;
    /* Zfinx explicitly excludes the FMV transfer instructions. */
    REQUIRE_EXT(ctx, RVF);

    TCGv dest = dest_gpr(ctx, a->rd);
    TCGv_i64 src1 = get_fpr_hs(ctx, a->rs1);
#if defined(TARGET_RISCV64)
    tcg_gen_ext32s_tl(dest, src1);
#else
    tcg_gen_extrl_i64_i32(dest, src1);
#endif

    gen_set_gpr(ctx, a->rd, dest);
    return true;
}

static bool trans_feq_s(DisasContext *ctx, arg_feq_s *a)
{
    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    if (fpj_cmp(ctx, MO_32, TCG_FPPRED_EQ, a->rd, a->rs1, a->rs2)) {
        return true;
    }

    TCGv dest = dest_gpr(ctx, a->rd);
    TCGv_i64 src1 = get_fpr_hs(ctx, a->rs1);
    TCGv_i64 src2 = get_fpr_hs(ctx, a->rs2);

    gen_helper_feq_s(dest, tcg_env, src1, src2);
    gen_set_gpr(ctx, a->rd, dest);
    return true;
}

static bool trans_flt_s(DisasContext *ctx, arg_flt_s *a)
{
    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    if (fpj_cmp(ctx, MO_32, TCG_FPPRED_LT, a->rd, a->rs1, a->rs2)) {
        return true;
    }

    TCGv dest = dest_gpr(ctx, a->rd);
    TCGv_i64 src1 = get_fpr_hs(ctx, a->rs1);
    TCGv_i64 src2 = get_fpr_hs(ctx, a->rs2);

    gen_helper_flt_s(dest, tcg_env, src1, src2);
    gen_set_gpr(ctx, a->rd, dest);
    return true;
}

static bool trans_fle_s(DisasContext *ctx, arg_fle_s *a)
{
    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    if (fpj_cmp(ctx, MO_32, TCG_FPPRED_LE, a->rd, a->rs1, a->rs2)) {
        return true;
    }

    TCGv dest = dest_gpr(ctx, a->rd);
    TCGv_i64 src1 = get_fpr_hs(ctx, a->rs1);
    TCGv_i64 src2 = get_fpr_hs(ctx, a->rs2);

    gen_helper_fle_s(dest, tcg_env, src1, src2);
    gen_set_gpr(ctx, a->rd, dest);
    return true;
}

static bool trans_fclass_s(DisasContext *ctx, arg_fclass_s *a)
{
    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    TCGv dest = dest_gpr(ctx, a->rd);
    TCGv_i64 src1 = get_fpr_hs(ctx, a->rs1);

    gen_helper_fclass_s(dest, tcg_env, src1);
    gen_set_gpr(ctx, a->rd, dest);
    return true;
}

static bool trans_fcvt_s_w(DisasContext *ctx, arg_fcvt_s_w *a)
{
    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    if (fpj_cvtf(ctx, MO_32, MO_32, TCG_FPOP_CVT_S, EXT_SIGN, a->rd, a->rs1,
            a->rm)) {
        return true;
    }

    TCGv_i64 dest = dest_fpr(ctx, a->rd);
    TCGv src = get_gpr(ctx, a->rs1, EXT_SIGN);

    gen_set_rm(ctx, a->rm);
    gen_helper_fcvt_s_w(dest, tcg_env, src);
    gen_set_fpr_hs(ctx, a->rd, dest);
    mark_fs_dirty(ctx);
    return true;
}

static bool trans_fcvt_s_wu(DisasContext *ctx, arg_fcvt_s_wu *a)
{
    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    if (fpj_cvtf(ctx, MO_32, MO_32, TCG_FPOP_CVT_U, EXT_ZERO, a->rd, a->rs1,
            a->rm)) {
        return true;
    }

    TCGv_i64 dest = dest_fpr(ctx, a->rd);
    TCGv src = get_gpr(ctx, a->rs1, EXT_ZERO);

    gen_set_rm(ctx, a->rm);
    gen_helper_fcvt_s_wu(dest, tcg_env, src);
    gen_set_fpr_hs(ctx, a->rd, dest);
    mark_fs_dirty(ctx);
    return true;
}

static bool trans_fmv_w_x(DisasContext *ctx, arg_fmv_w_x *a)
{
    /* NOTE: This was FMV.S.X in an earlier version of the ISA spec! */
    REQUIRE_FPU;
    /* Zfinx explicitly excludes the FMV transfer instructions. */
    REQUIRE_EXT(ctx, RVF);

    TCGv_i64 dest = dest_fpr(ctx, a->rd);
    TCGv src = get_gpr(ctx, a->rs1, EXT_ZERO);

    tcg_gen_extu_tl_i64(dest, src);
    gen_nanbox_s(dest, dest);
    gen_set_fpr_hs(ctx, a->rd, dest);
    mark_fs_dirty(ctx);
    return true;
}

static bool trans_fcvt_l_s(DisasContext *ctx, arg_fcvt_l_s *a)
{
    REQUIRE_64BIT(ctx);
    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    if (fpj_cvti(ctx, MO_32, TCG_FPOP_CVTI_S64, false, a->rd, a->rs1, a->rm)) {
        return true;
    }

    TCGv dest = dest_gpr(ctx, a->rd);
    TCGv_i64 src1 = get_fpr_hs(ctx, a->rs1);

    gen_set_rm(ctx, a->rm);
    gen_helper_fcvt_l_s(dest, tcg_env, src1);
    gen_set_gpr(ctx, a->rd, dest);
    return true;
}

static bool trans_fcvt_lu_s(DisasContext *ctx, arg_fcvt_lu_s *a)
{
    REQUIRE_64BIT(ctx);
    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    if (fpj_cvti(ctx, MO_32, TCG_FPOP_CVTI_U64, false, a->rd, a->rs1, a->rm)) {
        return true;
    }

    TCGv dest = dest_gpr(ctx, a->rd);
    TCGv_i64 src1 = get_fpr_hs(ctx, a->rs1);

    gen_set_rm(ctx, a->rm);
    gen_helper_fcvt_lu_s(dest, tcg_env, src1);
    gen_set_gpr(ctx, a->rd, dest);
    return true;
}

static bool trans_fcvt_s_l(DisasContext *ctx, arg_fcvt_s_l *a)
{
    REQUIRE_64BIT(ctx);
    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    if (fpj_cvtf(ctx, MO_32, MO_64, TCG_FPOP_CVT_S | TCG_FPOP_F_RES32,
            EXT_NONE, a->rd, a->rs1, a->rm)) {
        return true;
    }

    TCGv_i64 dest = dest_fpr(ctx, a->rd);
    TCGv src = get_gpr(ctx, a->rs1, EXT_SIGN);

    gen_set_rm(ctx, a->rm);
    gen_helper_fcvt_s_l(dest, tcg_env, src);
    gen_set_fpr_hs(ctx, a->rd, dest);
    mark_fs_dirty(ctx);
    return true;
}

static bool trans_fcvt_s_lu(DisasContext *ctx, arg_fcvt_s_lu *a)
{
    REQUIRE_64BIT(ctx);
    REQUIRE_FPU;
    REQUIRE_ZFINX_OR_F(ctx);

    if (fpj_cvtf(ctx, MO_32, MO_64, TCG_FPOP_CVT_U | TCG_FPOP_F_RES32,
            EXT_NONE, a->rd, a->rs1, a->rm)) {
        return true;
    }

    TCGv_i64 dest = dest_fpr(ctx, a->rd);
    TCGv src = get_gpr(ctx, a->rs1, EXT_ZERO);

    gen_set_rm(ctx, a->rm);
    gen_helper_fcvt_s_lu(dest, tcg_env, src);
    gen_set_fpr_hs(ctx, a->rd, dest);
    mark_fs_dirty(ctx);
    return true;
}
