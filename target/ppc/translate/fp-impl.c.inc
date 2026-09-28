/*
 * translate-fp.c
 *
 * Standard FPU translation
 */

static inline void gen_reset_fpstatus(void)
{
    gen_helper_reset_fpstatus(tcg_env);
}

static inline void gen_compute_fprf_float64(TCGv_i64 arg)
{
    gen_helper_compute_fprf_float64(tcg_env, arg);
    gen_helper_float_check_status(tcg_env);
}

#if defined(TARGET_PPC64)
static void gen_set_cr1_from_fpscr(DisasContext *ctx)
{
    TCGv_i32 tmp = tcg_temp_new_i32();
    gen_helper_fpscr_sync(tcg_env);
    tcg_gen_trunc_tl_i32(tmp, cpu_fpscr);
    tcg_gen_shri_i32(cpu_crf[1], tmp, 28);
}
#else
static void gen_set_cr1_from_fpscr(DisasContext *ctx)
{
    gen_helper_fpscr_sync(tcg_env);
    tcg_gen_shri_tl(cpu_crf[1], cpu_fpscr, 28);
}
#endif

/*
 * FP instructions as host FP code (ctx->fp_jit, see tcg/tcg-fpop.h).
 * Operands are loaded from and stored back to env for each instruction;
 * TCG keeps them in host vector registers within a TB when it can.
 * Exception flags accumulate in the host FP status register and FPRF is
 * kept as the result that defines it: ppc_fpscr_sync() rebuilds FPSCR
 * whenever it is read.
 */
static TCGv_vec fpj_ld(int reg)
{
    TCGv_vec v = tcg_temp_new_vec(TCG_TYPE_V64);
    tcg_gen_ld_vec(v, tcg_env, fpr_offset(reg));
    return v;
}

static void fpj_set_fprf(TCGv_vec v)
{
    tcg_gen_st_vec(v, tcg_env, offsetof(CPUPPCState, fprf_val));
    tcg_gen_st_i32(tcg_constant_i32(PPC_FPRF_LAZY_RESULT), tcg_env,
                   offsetof(CPUPPCState, fprf_lazy));
}

static void fpj_st(int reg, TCGv_vec v, bool set_fprf)
{
    tcg_gen_st_vec(v, tcg_env, fpr_offset(reg));
    /* doubleword 1 of the VSR is 0, as set_fpr() does */
    tcg_gen_st_i64(tcg_constant_i64(0), tcg_env, vsr64_offset(reg, false));
    if (set_fprf) {
        fpj_set_fprf(v);
    } else {
        tcg_gen_st_i32(tcg_constant_i32(1), tcg_env,
                       offsetof(CPUPPCState, fp_host_used));
    }
}

static bool do_fpj_2(DisasContext *ctx, int t, int a, int b, bool rc,
                     unsigned fpop)
{
    TCGv_vec va = fpj_ld(a);
    TCGv_vec vb = fpj_ld(b);
    TCGv_vec vt = tcg_temp_new_vec(TCG_TYPE_V64);

    tcg_gen_fpop2_vec(MO_64, vt, va, vb, fpop | TCG_FPOP_F_DEFNAN);
    fpj_st(t, vt, true);
    if (unlikely(rc)) {
        gen_set_cr1_from_fpscr(ctx);
    }
    return true;
}

/*
 * DOUBLE(), the non-arithmetic single -> double conversion of loads, in
 * generated code: helper_todouble() is only called for denormals.
 */
static void gen_todouble(TCGv_i64 ret, TCGv_i32 arg)
{
    TCGv_i64 x = tcg_temp_new_i64();
    TCGv_i64 r = tcg_temp_new_i64();
    TCGv_i64 t = tcg_temp_new_i64();
    TCGv_i64 e = tcg_temp_new_i64();
    TCGv_i64 abs = tcg_temp_new_i64();
    TCGLabel *done = gen_new_label();

    tcg_gen_extu_i32_i64(x, arg);
    /* normal: WORD[0:1] || ~WORD[1] x3 || WORD[2:31] || 0 x29 */
    tcg_gen_shli_i64(r, x, 29);
    tcg_gen_andi_i64(r, r, MAKE_64BIT_MASK(29, 30));
    tcg_gen_andi_i64(t, x, 0xc0000000);
    tcg_gen_shli_i64(t, t, 32);
    tcg_gen_or_i64(r, r, t);
    tcg_gen_extract_i64(e, x, 23, 8);
    /* bits 59:61 are ones unless WORD[1] is set in a finite number */
    tcg_gen_extract_i64(t, x, 30, 1);
    tcg_gen_xori_i64(t, t, 1);
    tcg_gen_setcondi_i64(TCG_COND_EQ, abs, e, 255);
    tcg_gen_or_i64(t, t, abs);
    tcg_gen_neg_i64(t, t);
    tcg_gen_andi_i64(t, t, 7ull << 59);
    tcg_gen_or_i64(r, r, t);
    /* zero: just the sign */
    tcg_gen_andi_i64(abs, x, 0x7fffffff);
    tcg_gen_shli_i64(t, x, 32);
    tcg_gen_andi_i64(t, t, INT64_MIN);
    tcg_gen_movcond_i64(TCG_COND_EQ, r, abs, tcg_constant_i64(0), t, r);
    /* denormal: rare, use the helper */
    tcg_gen_brcondi_i64(TCG_COND_NE, e, 0, done);
    tcg_gen_brcondi_i64(TCG_COND_EQ, abs, 0, done);
    gen_helper_todouble(r, arg);
    gen_set_label(done);
    tcg_gen_mov_i64(ret, r);
}

/* SINGLE(), the conversion of stores; helper_tosingle() for denormals */
static void gen_tosingle(TCGv_i32 ret, TCGv_i64 arg)
{
    TCGv_i64 r = tcg_temp_new_i64();
    TCGv_i64 t = tcg_temp_new_i64();
    TCGv_i64 e = tcg_temp_new_i64();
    TCGv_i32 w = tcg_temp_new_i32();
    TCGLabel *done = gen_new_label();

    /* exp > 896: WORD = FRS[0:1] || FRS[5:34] */
    tcg_gen_extract_i64(r, arg, 62, 2);
    tcg_gen_shli_i64(r, r, 30);
    tcg_gen_extract_i64(t, arg, 29, 30);
    tcg_gen_or_i64(r, r, t);
    /* otherwise just the sign, unless in the denormal range */
    tcg_gen_extract_i64(e, arg, 52, 11);
    tcg_gen_shri_i64(t, arg, 63);
    tcg_gen_shli_i64(t, t, 31);
    tcg_gen_movcond_i64(TCG_COND_GTU, r, e, tcg_constant_i64(896), r, t);
    tcg_gen_extrl_i64_i32(w, r);
    tcg_gen_brcondi_i64(TCG_COND_GTU, e, 896, done);
    tcg_gen_brcondi_i64(TCG_COND_LTU, e, 874, done);
    gen_helper_tosingle(w, arg);
    gen_set_label(done);
    tcg_gen_mov_i32(ret, w);
}

/***                       Floating-Point arithmetic                       ***/
static bool do_helper_acb(DisasContext *ctx, arg_A *a,
                          void (*helper)(TCGv_i64, TCGv_ptr, TCGv_i64,
                                         TCGv_i64, TCGv_i64), unsigned fpop)
{
    TCGv_i64 t0, t1, t2, t3;
    REQUIRE_INSNS_FLAGS(ctx, FLOAT);
    REQUIRE_FPU(ctx);
    if (ctx->fp_jit) {
        /* FRT = +-(FRA * FRC +- FRB), NaN priority A, B, C */
        TCGv_vec va = fpj_ld(a->fra);
        TCGv_vec vc = fpj_ld(a->frc);
        TCGv_vec vb = fpj_ld(a->frb);
        TCGv_vec vt = tcg_temp_new_vec(TCG_TYPE_V64);
        tcg_gen_fpop3_vec(MO_64, vt, va, vc, vb, fpop | TCG_FPOP_FMA
                          | TCG_FPOP_F_DEFNAN | TCG_FPOP_F_NAN_ACB);
        fpj_st(a->frt, vt, true);
        if (unlikely(a->rc)) {
            gen_set_cr1_from_fpscr(ctx);
        }
        return true;
    }
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    t2 = tcg_temp_new_i64();
    t3 = tcg_temp_new_i64();
    gen_reset_fpstatus();
    get_fpr(t0, a->fra);
    get_fpr(t1, a->frc);
    get_fpr(t2, a->frb);
    helper(t3, tcg_env, t0, t1, t2);
    set_fpr(a->frt, t3);
    gen_compute_fprf_float64(t3);
    if (unlikely(a->rc)) {
        gen_set_cr1_from_fpscr(ctx);
    }
    return true;
}

static bool do_helper_ab(DisasContext *ctx, arg_A_tab *a,
                         void (*helper)(TCGv_i64, TCGv_ptr, TCGv_i64,
                                        TCGv_i64), unsigned fpop)
{
    TCGv_i64 t0, t1, t2;
    REQUIRE_INSNS_FLAGS(ctx, FLOAT);
    REQUIRE_FPU(ctx);
    if (ctx->fp_jit) {
        return do_fpj_2(ctx, a->frt, a->fra, a->frb, a->rc, fpop);
    }
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    t2 = tcg_temp_new_i64();
    gen_reset_fpstatus();
    get_fpr(t0, a->fra);
    get_fpr(t1, a->frb);
    helper(t2, tcg_env, t0, t1);
    set_fpr(a->frt, t2);
    gen_compute_fprf_float64(t2);
    if (unlikely(a->rc)) {
        gen_set_cr1_from_fpscr(ctx);
    }
    return true;
}

static bool do_helper_ac(DisasContext *ctx, arg_A_tac *a,
                         void (*helper)(TCGv_i64, TCGv_ptr, TCGv_i64,
                                        TCGv_i64), unsigned fpop)
{
    TCGv_i64 t0, t1, t2;
    REQUIRE_INSNS_FLAGS(ctx, FLOAT);
    REQUIRE_FPU(ctx);
    if (ctx->fp_jit) {
        return do_fpj_2(ctx, a->frt, a->fra, a->frc, a->rc, fpop);
    }
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    t2 = tcg_temp_new_i64();
    gen_reset_fpstatus();
    get_fpr(t0, a->fra);
    get_fpr(t1, a->frc);
    helper(t2, tcg_env, t0, t1);
    set_fpr(a->frt, t2);
    gen_compute_fprf_float64(t2);
    if (unlikely(a->rc)) {
        gen_set_cr1_from_fpscr(ctx);
    }
    return true;
}

static bool do_round_convert(DisasContext *ctx, arg_X_tb_rc *a,
                             void (*helper)(TCGv_i64, TCGv_env, TCGv_i64),
                             bool set_fprf, unsigned fpop)
{
    TCGv_i64 t0, t1;
    REQUIRE_FPU(ctx);
    if (ctx->fp_jit && fpop) {
        TCGv_vec vb = fpj_ld(a->rb);
        TCGv_vec vt = tcg_temp_new_vec(TCG_TYPE_V64);
        tcg_gen_fpop1_vec(MO_64, vt, vb, fpop);
        fpj_st(a->rt, vt, set_fprf);
        if (unlikely(a->rc)) {
            gen_set_cr1_from_fpscr(ctx);
        }
        return true;
    }
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    gen_reset_fpstatus();
    get_fpr(t0, a->rb);
    helper(t1, tcg_env, t0);
    set_fpr(a->rt, t1);
    if (set_fprf) {
        gen_helper_compute_fprf_float64(tcg_env, t1);
    }
    gen_helper_float_check_status(tcg_env);
    if (unlikely(a->rc)) {
        gen_set_cr1_from_fpscr(ctx);
    }
    return true;
}

static bool do_helper_bs(DisasContext *ctx, arg_A_tb *a,
                         void (*helper)(TCGv_i64, TCGv_ptr, TCGv_i64))
{
    TCGv_i64 t0, t1;
    REQUIRE_FPU(ctx);
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    gen_reset_fpstatus();
    get_fpr(t0, a->frb);
    helper(t1, tcg_env, t0);
    set_fpr(a->frt, t1);
    gen_compute_fprf_float64(t1);
    if (unlikely(a->rc)) {
        gen_set_cr1_from_fpscr(ctx);
    }
    return true;
}

static bool trans_FSEL(DisasContext *ctx, arg_A *a)
{
    TCGv_i64 t0, t1, t2, c0, c1;

    REQUIRE_INSNS_FLAGS(ctx, FLOAT_FSEL);
    REQUIRE_FPU(ctx);

    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    t2 = tcg_temp_new_i64();
    c0 = tcg_temp_new_i64();
    c1 = tcg_temp_new_i64();

    get_fpr(t0, a->fra);
    get_fpr(t1, a->frb);
    get_fpr(t2, a->frc);

    /*
     * FRA >= 0 (+0 to +inf, or -0) and not a NaN selects FRC, as
     * helper_FSEL does, with integer ops on the encoding.
     */
    tcg_gen_setcondi_i64(TCG_COND_LEU, c0, t0, 0x7ff0000000000000ull);
    tcg_gen_setcondi_i64(TCG_COND_EQ, c1, t0, INT64_MIN);
    tcg_gen_or_i64(c0, c0, c1);
    tcg_gen_movcond_i64(TCG_COND_NE, t0, c0, tcg_constant_i64(0), t2, t1);
    set_fpr(a->frt, t0);
    if (a->rc) {
        gen_set_cr1_from_fpscr(ctx);
    }
    return true;
}

static bool do_helper_fsqrt(DisasContext *ctx, arg_A_tb *a,
                            void (*helper)(TCGv_i64, TCGv_ptr, TCGv_i64),
                            unsigned fpop)
{
    TCGv_i64 t0, t1;

    REQUIRE_INSNS_FLAGS(ctx, FLOAT_FSQRT);
    REQUIRE_FPU(ctx);

    if (ctx->fp_jit) {
        TCGv_vec vb = fpj_ld(a->frb);
        TCGv_vec vt = tcg_temp_new_vec(TCG_TYPE_V64);
        tcg_gen_fpop1_vec(MO_64, vt, vb, fpop | TCG_FPOP_F_DEFNAN);
        fpj_st(a->frt, vt, true);
        if (unlikely(a->rc != 0)) {
            gen_set_cr1_from_fpscr(ctx);
        }
        return true;
    }

    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();

    gen_reset_fpstatus();
    get_fpr(t0, a->frb);
    helper(t1, tcg_env, t0);
    set_fpr(a->frt, t1);
    gen_compute_fprf_float64(t1);
    if (unlikely(a->rc != 0)) {
        gen_set_cr1_from_fpscr(ctx);
    }
    return true;
}

#define SP_  TCG_FPOP_F_SP
TRANS(FADD, do_helper_ab, gen_helper_FADD, TCG_FPOP_ADD);
TRANS(FADDS, do_helper_ab, gen_helper_FADDS, TCG_FPOP_ADD | SP_);
TRANS(FSUB, do_helper_ab, gen_helper_FSUB, TCG_FPOP_SUB);
TRANS(FSUBS, do_helper_ab, gen_helper_FSUBS, TCG_FPOP_SUB | SP_);
TRANS(FDIV, do_helper_ab, gen_helper_FDIV, TCG_FPOP_DIV);
TRANS(FDIVS, do_helper_ab, gen_helper_FDIVS, TCG_FPOP_DIV | SP_);
TRANS(FMUL, do_helper_ac, gen_helper_FMUL, TCG_FPOP_MUL);
TRANS(FMULS, do_helper_ac, gen_helper_FMULS, TCG_FPOP_MUL | SP_);

/* the negated forms negate the rounded result, as the ISA says */
TRANS(FMADD, do_helper_acb, gen_helper_FMADD, 0);
TRANS(FMADDS, do_helper_acb, gen_helper_FMADDS, SP_);
TRANS(FMSUB, do_helper_acb, gen_helper_FMSUB, TCG_FPOP_F_NEG_ADD);
TRANS(FMSUBS, do_helper_acb, gen_helper_FMSUBS, TCG_FPOP_F_NEG_ADD | SP_);

TRANS(FNMADD, do_helper_acb, gen_helper_FNMADD, TCG_FPOP_F_NEG_RES);
TRANS(FNMADDS, do_helper_acb, gen_helper_FNMADDS, TCG_FPOP_F_NEG_RES | SP_);
TRANS(FNMSUB, do_helper_acb, gen_helper_FNMSUB,
      TCG_FPOP_F_NEG_ADD | TCG_FPOP_F_NEG_RES);
TRANS(FNMSUBS, do_helper_acb, gen_helper_FNMSUBS,
      TCG_FPOP_F_NEG_ADD | TCG_FPOP_F_NEG_RES | SP_);

TRANS_FLAGS(FLOAT_EXT, FRE, do_helper_bs, gen_helper_FRE);
TRANS_FLAGS(FLOAT_FRES, FRES, do_helper_bs, gen_helper_FRES);
TRANS_FLAGS(FLOAT_FRSQRTE, FRSQRTE, do_helper_bs, gen_helper_FRSQRTE);
TRANS_FLAGS(FLOAT_FRSQRTES, FRSQRTES, do_helper_bs, gen_helper_FRSQRTES);

TRANS(FSQRT, do_helper_fsqrt, gen_helper_FSQRT, TCG_FPOP_SQRT);
TRANS(FSQRTS, do_helper_fsqrt, gen_helper_FSQRTS, TCG_FPOP_SQRT | SP_);

/***                     Floating-Point round & convert                    ***/
/* fpop 0: always the helper (frin rounds ties away, which x86 cannot) */
#define RINT_(m)    (TCG_FPOP_RINT | TCG_FPOP_RMODE(m))
#define CVTI_(op)   (TCG_FPOP_CVTI_##op | TCG_FPOP_F_PPC_SAT)
#define CVTIZ_(op)  (CVTI_(op) | TCG_FPOP_F_TRUNC)
TRANS_FLAGS(FLOAT, FRSP, do_round_convert, gen_helper_FRSP, true,
            TCG_FPOP_RND_SP);
TRANS_FLAGS(FLOAT_EXT, FRIN, do_round_convert, gen_helper_FRIN, true, 0);
TRANS_FLAGS(FLOAT_EXT, FRIZ, do_round_convert, gen_helper_FRIZ, true,
            RINT_(TCG_FPRND_ZERO));
TRANS_FLAGS(FLOAT_EXT, FRIP, do_round_convert, gen_helper_FRIP, true,
            RINT_(TCG_FPRND_UP));
TRANS_FLAGS(FLOAT_EXT, FRIM, do_round_convert, gen_helper_FRIM, true,
            RINT_(TCG_FPRND_DOWN));

TRANS_FLAGS(FLOAT, FCTIW, do_round_convert, gen_helper_FCTIW, false,
            CVTI_(S32));
TRANS_FLAGS2(FP_CVT_ISA206, FCTIWU, do_round_convert, gen_helper_FCTIWU, false,
             CVTI_(U32));
TRANS_FLAGS(FLOAT, FCTIWZ, do_round_convert, gen_helper_FCTIWZ, false,
            CVTIZ_(S32));
TRANS_FLAGS2(FP_CVT_ISA206, FCTIWUZ, do_round_convert, gen_helper_FCTIWUZ,
             false, CVTIZ_(U32));

TRANS_FLAGS2(FP_CVT_S64, FCTID, do_round_convert, gen_helper_FCTID, false,
             CVTI_(S));
TRANS_FLAGS2(FP_CVT_ISA206, FCTIDU, do_round_convert, gen_helper_FCTIDU, false,
             CVTI_(U));
TRANS_FLAGS2(FP_CVT_S64, FCTIDZ, do_round_convert, gen_helper_FCTIDZ, false,
             CVTIZ_(S));
TRANS_FLAGS2(FP_CVT_ISA206, FCTIDUZ, do_round_convert, gen_helper_FCTIDUZ,
             false, CVTIZ_(U));

TRANS_FLAGS2(FP_CVT_S64, FCFID, do_round_convert, gen_helper_FCFID, true,
             TCG_FPOP_CVT_S);
TRANS_FLAGS2(FP_CVT_ISA206, FCFIDS, do_round_convert, gen_helper_FCFIDS, false,
             TCG_FPOP_CVT_S | SP_);
TRANS_FLAGS2(FP_CVT_ISA206, FCFIDU, do_round_convert, gen_helper_FCFIDU, false,
             TCG_FPOP_CVT_U);
TRANS_FLAGS2(FP_CVT_ISA206, FCFIDUS, do_round_convert, gen_helper_FCFIDUS,
             false, TCG_FPOP_CVT_U | SP_);

static bool trans_FTDIV(DisasContext *ctx, arg_X_bf *a)
{
    TCGv_i64 t0, t1;
    REQUIRE_INSNS_FLAGS2(ctx, FP_TST_ISA206);
    REQUIRE_FPU(ctx);
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    get_fpr(t0, a->ra);
    get_fpr(t1, a->rb);
    gen_helper_FTDIV(cpu_crf[a->bf], t0, t1);
    return true;
}

static bool trans_FTSQRT(DisasContext *ctx, arg_X_bf_b *a)
{
    TCGv_i64 t0;
    REQUIRE_INSNS_FLAGS2(ctx, FP_TST_ISA206);
    REQUIRE_FPU(ctx);
    t0 = tcg_temp_new_i64();
    get_fpr(t0, a->rb);
    gen_helper_FTSQRT(cpu_crf[a->bf], t0);
    return true;
}

/***                         Floating-Point compare                        ***/
static bool do_helper_cmp(DisasContext *ctx, arg_X_bf *a,
                          void (*helper)(TCGv_env, TCGv_i64, TCGv_i64,
                                         TCGv_i32), bool ordered)
{
    TCGv_i32 crf;
    TCGv_i64 t0, t1;
    REQUIRE_INSNS_FLAGS(ctx, FLOAT);
    REQUIRE_FPU(ctx);
    if (ctx->fp_jit) {
        TCGv_vec va = fpj_ld(a->ra);
        TCGv_vec vb = fpj_ld(a->rb);
        TCGv_i32 lazy = tcg_temp_new_i32();
        TCGv t = tcg_temp_new();

        tcg_gen_fpcmpcc_vec(MO_64, cpu_crf[a->bf], va, vb,
                            ordered ? TCG_FPOP_F_SIGNAL : 0);
        /* FPSCR[FPCC] = CR field, the class bit C is left alone */
        tcg_gen_extu_i32_tl(t, cpu_crf[a->bf]);
        tcg_gen_deposit_tl(cpu_fpscr, cpu_fpscr, t, FPSCR_FPCC, 4);
        tcg_gen_ld_i32(lazy, tcg_env, offsetof(CPUPPCState, fprf_lazy));
        tcg_gen_ori_i32(lazy, lazy, PPC_FPRF_LAZY_FPCC);
        tcg_gen_st_i32(lazy, tcg_env, offsetof(CPUPPCState, fprf_lazy));
        return true;
    }
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    gen_reset_fpstatus();
    crf = tcg_constant_i32(a->bf);
    get_fpr(t0, a->ra);
    get_fpr(t1, a->rb);
    helper(tcg_env, t0, t1, crf);
    gen_helper_float_check_status(tcg_env);
    return true;
}

TRANS(FCMPU, do_helper_cmp, gen_helper_FCMPU, false);
TRANS(FCMPO, do_helper_cmp, gen_helper_FCMPO, true);

/***                         Floating-point move                           ***/

/* fmr  - fmr. */
/* XXX: beware that fmr never checks for NaNs nor update FPSCR */
static bool trans_FMR(DisasContext *ctx, arg_FMR *a)
{
    TCGv_i64 t0;
    REQUIRE_INSNS_FLAGS(ctx, FLOAT);
    REQUIRE_FPU(ctx);
    t0 = tcg_temp_new_i64();
    get_fpr(t0, a->rb);
    set_fpr(a->rt, t0);
    if (unlikely(a->rc)) {
        gen_set_cr1_from_fpscr(ctx);
    }
    return true;
}

/* XXX: beware that f{neg, abs, nabs} never checks for NaNs nor update FPSCR */
static bool do_move_b(DisasContext *ctx, arg_X_tb_rc *a, int64_t val,
                      void (*tcg_op)(TCGv_i64, TCGv_i64, int64_t))
{
    TCGv_i64 t0, t1;
    REQUIRE_INSNS_FLAGS(ctx, FLOAT);
    REQUIRE_FPU(ctx);
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    get_fpr(t0, a->rb);
    tcg_op(t1, t0, val);
    set_fpr(a->rt, t1);
    if (unlikely(a->rc)) {
        gen_set_cr1_from_fpscr(ctx);
    }
    return true;
}

TRANS(FNEG, do_move_b, 1ULL << 63, tcg_gen_xori_i64);
TRANS(FABS, do_move_b, ~(1ULL << 63), tcg_gen_andi_i64);
TRANS(FNABS, do_move_b, 1ULL << 63, tcg_gen_ori_i64);

/* fcpsgn: PowerPC 2.05 specification */
/* XXX: beware that fcpsgn never checks for NaNs nor update FPSCR */
static bool trans_FCPSGN(DisasContext *ctx, arg_FCPSGN *a)
{
    TCGv_i64 t0, t1, t2;
    REQUIRE_INSNS_FLAGS2(ctx, ISA205);
    REQUIRE_FPU(ctx);
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    t2 = tcg_temp_new_i64();
    get_fpr(t0, a->ra);
    get_fpr(t1, a->rb);
    tcg_gen_deposit_i64(t2, t0, t1, 0, 63);
    set_fpr(a->rt, t2);
    if (unlikely(a->rc)) {
        gen_set_cr1_from_fpscr(ctx);
    }
    return true;
}

static bool trans_FMRGEW(DisasContext *ctx, arg_FMRGEW *a)
{
    TCGv_i64 t0, t1, t2;
    REQUIRE_INSNS_FLAGS2(ctx, VSX207);
    REQUIRE_FPU(ctx);
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    t2 = tcg_temp_new_i64();
    get_fpr(t1, a->rb);
    tcg_gen_shri_i64(t0, t1, 32);
    get_fpr(t1, a->ra);
    tcg_gen_deposit_i64(t2, t1, t0, 0, 32);
    set_fpr(a->rt, t2);
    return true;
}

static bool trans_FMRGOW(DisasContext *ctx, arg_FMRGOW *a)
{
    TCGv_i64 t0, t1, t2;
    REQUIRE_INSNS_FLAGS2(ctx, VSX207);
    REQUIRE_FPU(ctx);
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    t2 = tcg_temp_new_i64();
    get_fpr(t0, a->rb);
    get_fpr(t1, a->ra);
    tcg_gen_deposit_i64(t2, t0, t1, 32, 32);
    set_fpr(a->rt, t2);
    return true;
}

/***                  Floating-Point status & ctrl register                ***/

/* mcrfs */
static void gen_mcrfs(DisasContext *ctx)
{
    TCGv tmp = tcg_temp_new();
    TCGv_i32 tmask;
    TCGv_i64 tnew_fpscr = tcg_temp_new_i64();
    int bfa;
    int nibble;
    int shift;

    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    bfa = crfS(ctx->opcode);
    nibble = 7 - bfa;
    shift = 4 * nibble;
    gen_helper_fpscr_sync(tcg_env);
    tcg_gen_shri_tl(tmp, cpu_fpscr, shift);
    tcg_gen_trunc_tl_i32(cpu_crf[crfD(ctx->opcode)], tmp);
    tcg_gen_andi_i32(cpu_crf[crfD(ctx->opcode)], cpu_crf[crfD(ctx->opcode)],
                     0xf);
    tcg_gen_extu_tl_i64(tnew_fpscr, cpu_fpscr);
    /* Only the exception bits (including FX) should be cleared if read */
    tcg_gen_andi_i64(tnew_fpscr, tnew_fpscr,
                     ~(MAKE_64BIT_MASK(shift, 4) & FP_EX_CLEAR_BITS));
    /* FEX and VX need to be updated, so don't set fpscr directly */
    tmask = tcg_constant_i32(1 << nibble);
    gen_helper_store_fpscr(tcg_env, tnew_fpscr, tmask);
}

static TCGv_i64 place_from_fpscr(int rt, uint64_t mask)
{
    TCGv_i64 fpscr = tcg_temp_new_i64();
    TCGv_i64 fpscr_masked = tcg_temp_new_i64();

    tcg_gen_extu_tl_i64(fpscr, cpu_fpscr);
    tcg_gen_andi_i64(fpscr_masked, fpscr, mask);
    set_fpr(rt, fpscr_masked);

    return fpscr;
}

static void store_fpscr_masked(TCGv_i64 fpscr, uint64_t clear_mask,
                               TCGv_i64 set_mask, uint32_t store_mask)
{
    TCGv_i64 fpscr_masked = tcg_temp_new_i64();
    TCGv_i32 st_mask = tcg_constant_i32(store_mask);

    tcg_gen_andi_i64(fpscr_masked, fpscr, ~clear_mask);
    tcg_gen_or_i64(fpscr_masked, fpscr_masked, set_mask);
    gen_helper_store_fpscr(tcg_env, fpscr_masked, st_mask);
}

static bool trans_MFFS_ISA207(DisasContext *ctx, arg_X_t_rc *a)
{
    if (!(ctx->insns_flags2 & PPC2_ISA300)) {
        /*
         * Before Power ISA v3.0, MFFS bits 11~15 were reserved, any instruction
         * with OPCD=63 and XO=583 should be decoded as MFFS.
         */
        return trans_MFFS(ctx, a);
    }
    /*
     * For Power ISA v3.0+, return false and let the pattern group
     * select the correct instruction.
     */
    return false;
}

static bool trans_MFFS(DisasContext *ctx, arg_X_t_rc *a)
{
    REQUIRE_FPU(ctx);

    gen_reset_fpstatus();
    place_from_fpscr(a->rt, UINT64_MAX);
    if (a->rc) {
        gen_set_cr1_from_fpscr(ctx);
    }
    return true;
}

static bool trans_MFFSCE(DisasContext *ctx, arg_X_t *a)
{
    TCGv_i64 fpscr;

    REQUIRE_FPU(ctx);

    gen_reset_fpstatus();
    fpscr = place_from_fpscr(a->rt, UINT64_MAX);
    store_fpscr_masked(fpscr, FP_ENABLES, tcg_constant_i64(0), 0x0003);
    return true;
}

static bool trans_MFFSCRN(DisasContext *ctx, arg_X_tb *a)
{
    TCGv_i64 t1, fpscr;

    REQUIRE_FPU(ctx);

    t1 = tcg_temp_new_i64();
    get_fpr(t1, a->rb);
    tcg_gen_andi_i64(t1, t1, FP_RN);

    gen_reset_fpstatus();
    fpscr = place_from_fpscr(a->rt, FP_DRN | FP_ENABLES | FP_NI | FP_RN);
    store_fpscr_masked(fpscr, FP_RN, t1, 0x0001);
    if (ctx->fp_jit) {
        /* a directed rounding mode needs the helpers (HFLAGS_FP_SOFT) */
        ctx->base.is_jmp = DISAS_CHAIN_UPDATE;
    }
    return true;
}

static bool trans_MFFSCDRN(DisasContext *ctx, arg_X_tb *a)
{
    TCGv_i64 t1, fpscr;

    REQUIRE_FPU(ctx);

    t1 = tcg_temp_new_i64();
    get_fpr(t1, a->rb);
    tcg_gen_andi_i64(t1, t1, FP_DRN);

    gen_reset_fpstatus();
    fpscr = place_from_fpscr(a->rt, FP_DRN | FP_ENABLES | FP_NI | FP_RN);
    store_fpscr_masked(fpscr, FP_DRN, t1, 0x0100);
    return true;
}

static bool trans_MFFSCRNI(DisasContext *ctx, arg_X_imm2 *a)
{
    TCGv_i64 t1, fpscr;

    REQUIRE_FPU(ctx);

    t1 = tcg_temp_new_i64();
    tcg_gen_movi_i64(t1, a->imm);

    gen_reset_fpstatus();
    fpscr = place_from_fpscr(a->rt, FP_DRN | FP_ENABLES | FP_NI | FP_RN);
    store_fpscr_masked(fpscr, FP_RN, t1, 0x0001);
    if (ctx->fp_jit && a->imm) {
        ctx->base.is_jmp = DISAS_CHAIN_UPDATE;
    }
    return true;
}

static bool trans_MFFSCDRNI(DisasContext *ctx, arg_X_imm3 *a)
{
    TCGv_i64 t1, fpscr;

    REQUIRE_FPU(ctx);

    t1 = tcg_temp_new_i64();
    tcg_gen_movi_i64(t1, (uint64_t)a->imm << FPSCR_DRN0);

    gen_reset_fpstatus();
    fpscr = place_from_fpscr(a->rt, FP_DRN | FP_ENABLES | FP_NI | FP_RN);
    store_fpscr_masked(fpscr, FP_DRN, t1, 0x0100);
    return true;
}

static bool trans_MFFSL(DisasContext *ctx, arg_X_t *a)
{
    REQUIRE_FPU(ctx);

    gen_reset_fpstatus();
    place_from_fpscr(a->rt, FP_DRN | FP_STATUS | FP_ENABLES | FP_NI | FP_RN);
    return true;
}

/* mtfsb0 */
static void gen_mtfsb0(DisasContext *ctx)
{
    uint8_t crb;

    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    crb = 31 - crbD(ctx->opcode);
    gen_reset_fpstatus();
    if (likely(crb != FPSCR_FEX && crb != FPSCR_VX)) {
        gen_helper_fpscr_clrbit(tcg_env, tcg_constant_i32(crb));
    }
    if (unlikely(Rc(ctx->opcode) != 0)) {
        tcg_gen_trunc_tl_i32(cpu_crf[1], cpu_fpscr);
        tcg_gen_shri_i32(cpu_crf[1], cpu_crf[1], FPSCR_OX);
    }
}

/* mtfsb1 */
static void gen_mtfsb1(DisasContext *ctx)
{
    uint8_t crb;

    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    crb = 31 - crbD(ctx->opcode);
    /* XXX: we pretend we can only do IEEE floating-point computations */
    if (likely(crb != FPSCR_FEX && crb != FPSCR_VX && crb != FPSCR_NI)) {
        gen_helper_fpscr_setbit(tcg_env, tcg_constant_i32(crb));
        if (ctx->fp_jit && (FP_SOFT_MASK & (1ull << crb))) {
            /* FP exception or directed rounding: next TBs use the helpers */
            ctx->base.is_jmp = DISAS_CHAIN_UPDATE;
        }
    }
    if (unlikely(Rc(ctx->opcode) != 0)) {
        tcg_gen_trunc_tl_i32(cpu_crf[1], cpu_fpscr);
        tcg_gen_shri_i32(cpu_crf[1], cpu_crf[1], FPSCR_OX);
    }
    /* We can raise a deferred exception */
    gen_helper_fpscr_check_status(tcg_env);
}

/* mtfsf */
static void gen_mtfsf(DisasContext *ctx)
{
    TCGv_i32 t0;
    TCGv_i64 t1;
    int flm, l, w;

    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    flm = FPFLM(ctx->opcode);
    l = FPL(ctx->opcode);
    w = FPW(ctx->opcode);
    if (unlikely(w & !(ctx->insns_flags2 & PPC2_ISA205))) {
        gen_inval_exception(ctx, POWERPC_EXCP_INVAL_INVAL);
        return;
    }
    if (!l) {
        t0 = tcg_constant_i32(flm << (w * 8));
    } else if (ctx->insns_flags2 & PPC2_ISA205) {
        t0 = tcg_constant_i32(0xffff);
    } else {
        t0 = tcg_constant_i32(0xff);
    }
    t1 = tcg_temp_new_i64();
    get_fpr(t1, rB(ctx->opcode));
    gen_helper_store_fpscr(tcg_env, t1, t0);
    if (ctx->fp_jit && (l || (flm << (w * 8)) & 3)) {
        /* nibbles 0-1 hold RN and the enables: the TB flags may change */
        ctx->base.is_jmp = DISAS_CHAIN_UPDATE;
    }
    if (unlikely(Rc(ctx->opcode) != 0)) {
        tcg_gen_trunc_tl_i32(cpu_crf[1], cpu_fpscr);
        tcg_gen_shri_i32(cpu_crf[1], cpu_crf[1], FPSCR_OX);
    }
    /* We can raise a deferred exception */
    gen_helper_fpscr_check_status(tcg_env);
}

/* mtfsfi */
static void gen_mtfsfi(DisasContext *ctx)
{
    int bf, sh, w;
    TCGv_i64 t0;
    TCGv_i32 t1;

    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    w = FPW(ctx->opcode);
    bf = FPBF(ctx->opcode);
    if (unlikely(w & !(ctx->insns_flags2 & PPC2_ISA205))) {
        gen_inval_exception(ctx, POWERPC_EXCP_INVAL_INVAL);
        return;
    }
    sh = (8 * w) + 7 - bf;
    t0 = tcg_constant_i64(((uint64_t)FPIMM(ctx->opcode)) << (4 * sh));
    t1 = tcg_constant_i32(1 << sh);
    gen_helper_store_fpscr(tcg_env, t0, t1);
    if (ctx->fp_jit
        && ((((uint64_t)FPIMM(ctx->opcode)) << (4 * sh)) & FP_SOFT_MASK)) {
        ctx->base.is_jmp = DISAS_CHAIN_UPDATE;
    }
    if (unlikely(Rc(ctx->opcode) != 0)) {
        tcg_gen_trunc_tl_i32(cpu_crf[1], cpu_fpscr);
        tcg_gen_shri_i32(cpu_crf[1], cpu_crf[1], FPSCR_OX);
    }
    /* We can raise a deferred exception */
    gen_helper_fpscr_check_status(tcg_env);
}

static void gen_qemu_ld32fs(DisasContext *ctx, TCGv_i64 dest, TCGv addr)
{
    TCGv_i32 tmp = tcg_temp_new_i32();
    tcg_gen_qemu_ld_i32(tmp, addr, ctx->mem_idx, DEF_MEMOP(MO_UL));
    gen_todouble(dest, tmp);
}

/* lfdepx (external PID lfdx) */
static void gen_lfdepx(DisasContext *ctx)
{
    TCGv EA;
    TCGv_i64 t0;
    CHK_SV(ctx);
    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    gen_set_access_type(ctx, ACCESS_FLOAT);
    EA = tcg_temp_new();
    t0 = tcg_temp_new_i64();
    gen_addr_reg_index(ctx, EA);
    tcg_gen_qemu_ld_i64(t0, EA, PPC_TLB_EPID_LOAD, DEF_MEMOP(MO_UQ));
    set_fpr(rD(ctx->opcode), t0);
}

/* lfdp */
static void gen_lfdp(DisasContext *ctx)
{
    TCGv EA;
    TCGv_i64 t0;
    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    gen_set_access_type(ctx, ACCESS_FLOAT);
    EA = tcg_temp_new();
    gen_addr_imm_index(ctx, EA, 0);
    t0 = tcg_temp_new_i64();
    /*
     * We only need to swap high and low halves. gen_qemu_ld64_i64
     * does necessary 64-bit byteswap already.
     */
    if (unlikely(ctx->le_mode)) {
        gen_qemu_ld64_i64(ctx, t0, EA);
        set_fpr(rD(ctx->opcode) + 1, t0);
        tcg_gen_addi_tl(EA, EA, 8);
        gen_qemu_ld64_i64(ctx, t0, EA);
        set_fpr(rD(ctx->opcode), t0);
    } else {
        gen_qemu_ld64_i64(ctx, t0, EA);
        set_fpr(rD(ctx->opcode), t0);
        tcg_gen_addi_tl(EA, EA, 8);
        gen_qemu_ld64_i64(ctx, t0, EA);
        set_fpr(rD(ctx->opcode) + 1, t0);
    }
}

/* lfdpx */
static void gen_lfdpx(DisasContext *ctx)
{
    TCGv EA;
    TCGv_i64 t0;
    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    gen_set_access_type(ctx, ACCESS_FLOAT);
    EA = tcg_temp_new();
    gen_addr_reg_index(ctx, EA);
    t0 = tcg_temp_new_i64();
    /*
     * We only need to swap high and low halves. gen_qemu_ld64_i64
     * does necessary 64-bit byteswap already.
     */
    if (unlikely(ctx->le_mode)) {
        gen_qemu_ld64_i64(ctx, t0, EA);
        set_fpr(rD(ctx->opcode) + 1, t0);
        tcg_gen_addi_tl(EA, EA, 8);
        gen_qemu_ld64_i64(ctx, t0, EA);
        set_fpr(rD(ctx->opcode), t0);
    } else {
        gen_qemu_ld64_i64(ctx, t0, EA);
        set_fpr(rD(ctx->opcode), t0);
        tcg_gen_addi_tl(EA, EA, 8);
        gen_qemu_ld64_i64(ctx, t0, EA);
        set_fpr(rD(ctx->opcode) + 1, t0);
    }
}

/* lfiwax */
static void gen_lfiwax(DisasContext *ctx)
{
    TCGv EA;
    TCGv t0;
    TCGv_i64 t1;
    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    gen_set_access_type(ctx, ACCESS_FLOAT);
    EA = tcg_temp_new();
    t0 = tcg_temp_new();
    t1 = tcg_temp_new_i64();
    gen_addr_reg_index(ctx, EA);
    gen_qemu_ld32s(ctx, t0, EA);
    tcg_gen_ext_tl_i64(t1, t0);
    set_fpr(rD(ctx->opcode), t1);
}

/* lfiwzx */
static void gen_lfiwzx(DisasContext *ctx)
{
    TCGv EA;
    TCGv_i64 t0;
    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    gen_set_access_type(ctx, ACCESS_FLOAT);
    EA = tcg_temp_new();
    t0 = tcg_temp_new_i64();
    gen_addr_reg_index(ctx, EA);
    gen_qemu_ld32u_i64(ctx, t0, EA);
    set_fpr(rD(ctx->opcode), t0);
}

#define GEN_STXF(name, stop, opc2, opc3, type)                                \
static void glue(gen_, name##x)(DisasContext *ctx)                            \
{                                                                             \
    TCGv EA;                                                                  \
    TCGv_i64 t0;                                                              \
    if (unlikely(!ctx->fpu_enabled)) {                                        \
        gen_exception(ctx, POWERPC_EXCP_FPU);                                 \
        return;                                                               \
    }                                                                         \
    gen_set_access_type(ctx, ACCESS_FLOAT);                                   \
    EA = tcg_temp_new();                                                      \
    t0 = tcg_temp_new_i64();                                                  \
    gen_addr_reg_index(ctx, EA);                                              \
    get_fpr(t0, rS(ctx->opcode));                                             \
    gen_qemu_##stop(ctx, t0, EA);                                             \
}

static void gen_qemu_st32fs(DisasContext *ctx, TCGv_i64 src, TCGv addr)
{
    TCGv_i32 tmp = tcg_temp_new_i32();
    gen_tosingle(tmp, src);
    tcg_gen_qemu_st_i32(tmp, addr, ctx->mem_idx, DEF_MEMOP(MO_UL));
}

/* stfdepx (external PID lfdx) */
static void gen_stfdepx(DisasContext *ctx)
{
    TCGv EA;
    TCGv_i64 t0;
    CHK_SV(ctx);
    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    gen_set_access_type(ctx, ACCESS_FLOAT);
    EA = tcg_temp_new();
    t0 = tcg_temp_new_i64();
    gen_addr_reg_index(ctx, EA);
    get_fpr(t0, rD(ctx->opcode));
    tcg_gen_qemu_st_i64(t0, EA, PPC_TLB_EPID_STORE, DEF_MEMOP(MO_UQ));
}

/* stfdp */
static void gen_stfdp(DisasContext *ctx)
{
    TCGv EA;
    TCGv_i64 t0;
    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    gen_set_access_type(ctx, ACCESS_FLOAT);
    EA = tcg_temp_new();
    t0 = tcg_temp_new_i64();
    gen_addr_imm_index(ctx, EA, 0);
    /*
     * We only need to swap high and low halves. gen_qemu_st64_i64
     * does necessary 64-bit byteswap already.
     */
    if (unlikely(ctx->le_mode)) {
        get_fpr(t0, rD(ctx->opcode) + 1);
        gen_qemu_st64_i64(ctx, t0, EA);
        tcg_gen_addi_tl(EA, EA, 8);
        get_fpr(t0, rD(ctx->opcode));
        gen_qemu_st64_i64(ctx, t0, EA);
    } else {
        get_fpr(t0, rD(ctx->opcode));
        gen_qemu_st64_i64(ctx, t0, EA);
        tcg_gen_addi_tl(EA, EA, 8);
        get_fpr(t0, rD(ctx->opcode) + 1);
        gen_qemu_st64_i64(ctx, t0, EA);
    }
}

/* stfdpx */
static void gen_stfdpx(DisasContext *ctx)
{
    TCGv EA;
    TCGv_i64 t0;
    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    gen_set_access_type(ctx, ACCESS_FLOAT);
    EA = tcg_temp_new();
    t0 = tcg_temp_new_i64();
    gen_addr_reg_index(ctx, EA);
    /*
     * We only need to swap high and low halves. gen_qemu_st64_i64
     * does necessary 64-bit byteswap already.
     */
    if (unlikely(ctx->le_mode)) {
        get_fpr(t0, rD(ctx->opcode) + 1);
        gen_qemu_st64_i64(ctx, t0, EA);
        tcg_gen_addi_tl(EA, EA, 8);
        get_fpr(t0, rD(ctx->opcode));
        gen_qemu_st64_i64(ctx, t0, EA);
    } else {
        get_fpr(t0, rD(ctx->opcode));
        gen_qemu_st64_i64(ctx, t0, EA);
        tcg_gen_addi_tl(EA, EA, 8);
        get_fpr(t0, rD(ctx->opcode) + 1);
        gen_qemu_st64_i64(ctx, t0, EA);
    }
}

/* Optional: */
static inline void gen_qemu_st32fiw(DisasContext *ctx, TCGv_i64 arg1, TCGv arg2)
{
    TCGv t0 = tcg_temp_new();
    tcg_gen_trunc_i64_tl(t0, arg1),
    gen_qemu_st32(ctx, t0, arg2);
}
/* stfiwx */
GEN_STXF(stfiw, st32fiw, 0x17, 0x1E, PPC_FLOAT_STFIWX);

/*            Floating-point Load/Store Instructions                         */
static bool do_lsfpsd(DisasContext *ctx, int rt, int ra, TCGv displ,
                      bool update, bool store, bool single)
{
    TCGv ea;
    TCGv_i64 t0;
    REQUIRE_INSNS_FLAGS(ctx, FLOAT);
    REQUIRE_FPU(ctx);
    if (update && ra == 0) {
        gen_invalid(ctx);
        return true;
    }
    gen_set_access_type(ctx, ACCESS_FLOAT);
    t0 = tcg_temp_new_i64();
    ea = do_ea_calc(ctx, ra, displ);
    if (store) {
        get_fpr(t0, rt);
        if (single) {
            gen_qemu_st32fs(ctx, t0, ea);
        } else {
            gen_qemu_st64_i64(ctx, t0, ea);
        }
    } else {
        if (single) {
            gen_qemu_ld32fs(ctx, t0, ea);
        } else {
            gen_qemu_ld64_i64(ctx, t0, ea);
        }
        set_fpr(rt, t0);
    }
    if (update) {
        tcg_gen_mov_tl(cpu_gpr[ra], ea);
    }
    return true;
}

static bool do_lsfp_D(DisasContext *ctx, arg_D *a, bool update, bool store,
                      bool single)
{
    return do_lsfpsd(ctx, a->rt, a->ra, tcg_constant_tl(a->si), update, store,
                     single);
}

static bool do_lsfp_PLS_D(DisasContext *ctx, arg_PLS_D *a, bool update,
                          bool store, bool single)
{
    arg_D d;
    if (!resolve_PLS_D(ctx, &d, a)) {
        return true;
    }
    return do_lsfp_D(ctx, &d, update, store, single);
}

static bool do_lsfp_X(DisasContext *ctx, arg_X *a, bool update,
                      bool store, bool single)
{
    return do_lsfpsd(ctx, a->rt, a->ra, cpu_gpr[a->rb], update, store, single);
}

TRANS(LFS, do_lsfp_D, false, false, true)
TRANS(LFSU, do_lsfp_D, true, false, true)
TRANS(LFSX, do_lsfp_X, false, false, true)
TRANS(LFSUX, do_lsfp_X, true, false, true)
TRANS(PLFS, do_lsfp_PLS_D, false, false, true)

TRANS(LFD, do_lsfp_D, false, false, false)
TRANS(LFDU, do_lsfp_D, true, false, false)
TRANS(LFDX, do_lsfp_X, false, false, false)
TRANS(LFDUX, do_lsfp_X, true, false, false)
TRANS(PLFD, do_lsfp_PLS_D, false, false, false)

TRANS(STFS, do_lsfp_D, false, true, true)
TRANS(STFSU, do_lsfp_D, true, true, true)
TRANS(STFSX, do_lsfp_X, false, true, true)
TRANS(STFSUX, do_lsfp_X, true, true, true)
TRANS(PSTFS, do_lsfp_PLS_D, false, true, true)

TRANS(STFD, do_lsfp_D, false, true, false)
TRANS(STFDU, do_lsfp_D, true, true, false)
TRANS(STFDX, do_lsfp_X, false, true, false)
TRANS(STFDUX, do_lsfp_X, true, true, false)
TRANS(PSTFD, do_lsfp_PLS_D, false, true, false)

#undef GEN_LDF
#undef GEN_LDUF
#undef GEN_LDUXF
#undef GEN_LDXF
#undef GEN_LDFS

#undef GEN_STF
#undef GEN_STUF
#undef GEN_STUXF
#undef GEN_STXF
#undef GEN_STFS
