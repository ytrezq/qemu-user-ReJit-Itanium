/* Differential test of AArch64 FP: scalar S/D, vector 4S/2D/2S (3-operand, FMA, sqrt, rounding, conversions); results + FPSR/NZCV. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>

typedef uint64_t u64;
typedef uint32_t u32;
static u64 rs = 0x2545f4914f6cdd1dull;
static u64 rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

static const u32 fsp[] = {
    0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007fffff, 0x00800000, 0x80800000,
    0x3f800000, 0xbf800000, 0x3f000000, 0x3fc00000, 0x40200000, 0x7f7fffff, 0xff7fffff,
    0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7fa00001, 0xffa00123, 0x7fc01234,
    0x4f000000, 0xcf000000, 0x4f800000, 0x5f000000, 0xdf000000, 0x3f7fffff, 0x00400000,
    0x0ffffff0, 0x4b000000, 0x4affffff, 0x3effffff,
};
static const u64 dsp[] = {
    0, 0x8000000000000000ull, 1, 0x8000000000000001ull, 0x000fffffffffffffull,
    0x0010000000000000ull, 0x8010000000000000ull, 0x3ff0000000000000ull, 0xbff0000000000000ull,
    0x3fe0000000000000ull, 0x3ff8000000000000ull, 0x4004000000000000ull,
    0x7fefffffffffffffull, 0xffefffffffffffffull, 0x7ff0000000000000ull, 0xfff0000000000000ull,
    0x7ff8000000000000ull, 0xfff8000000000000ull, 0x7ff4000000000001ull, 0xfff4000000000123ull,
    0x7ff8000000001234ull, 0x41e0000000000000ull, 0xc1e0000000000000ull, 0x41f0000000000000ull,
    0x43e0000000000000ull, 0xc3e0000000000000ull, 0x41dfffffffe00000ull, 0x3fefffffffffffffull,
    0x3810000000000000ull, 0x380fffffffffffffull, 0x36a0000000000000ull, 0x47efffffe0000000ull,
    0x47effffff0000000ull, 0x3e00000000000000ull, 0x4330000000000000ull, 0x432fffffffffffffull,
};
static u32 rf(void)
{
    switch (rnd() % 8) {
    case 0: case 1: return fsp[rnd() % (sizeof(fsp) / 4)];
    case 2: return (u32)rnd();
    case 3: return ((u32)rnd() & 0x807fffff) | (u32)(rnd() % 6) << 23;   /* tiny */
    case 4: return ((u32)rnd() & 0x807fffff) | (u32)(250 + rnd() % 5) << 23; /* huge */
    default: return ((u32)rnd() & 0x807fffff) | (u32)(127 - 30 + rnd() % 60) << 23;
    }
}
static u64 rd(void)
{
    switch (rnd() % 8) {
    case 0: case 1: return dsp[rnd() % (sizeof(dsp) / 8)];
    case 2: return rnd();
    case 3: return (rnd() & 0x800fffffffffffffull) | (rnd() % 6) << 52;
    case 4: return (rnd() & 0x800fffffffffffffull) | (2040 + rnd() % 7) << 52;
    case 5: return (rnd() & 0x800fffffe0000000ull) | (1023 - 30 + rnd() % 60) << 52; /* float-ish */
    default: return (rnd() & 0x800fffffffffffffull) | (1023 - 60 + rnd() % 120) << 52;
    }
}

static inline void clrfpsr(void) { asm volatile("msr fpsr, xzr"); }
static inline u64 getfpsr(void) { u64 x; asm volatile("mrs %0, fpsr" : "=r"(x)); return x; }
static inline u64 getnzcv(void) { u64 x; asm volatile("mrs %0, nzcv" : "=r"(x)); return x; }

#define OP3S(nm, insn) static u32 nm(u32 a, u32 b) { float x, y, r; memcpy(&x,&a,4); memcpy(&y,&b,4); \
    asm volatile(insn " %s0, %s1, %s2" : "=w"(r) : "w"(x), "w"(y)); u32 o; memcpy(&o,&r,4); return o; }
#define OP3D(nm, insn) static u64 nm(u64 a, u64 b) { double x, y, r; memcpy(&x,&a,8); memcpy(&y,&b,8); \
    asm volatile(insn " %d0, %d1, %d2" : "=w"(r) : "w"(x), "w"(y)); u64 o; memcpy(&o,&r,8); return o; }
#define OP4S(nm, insn) static u32 nm(u32 a, u32 b, u32 c) { float x, y, z, r; memcpy(&x,&a,4); memcpy(&y,&b,4); memcpy(&z,&c,4); \
    asm volatile(insn " %s0, %s1, %s2, %s3" : "=w"(r) : "w"(x), "w"(y), "w"(z)); u32 o; memcpy(&o,&r,4); return o; }
#define OP4D(nm, insn) static u64 nm(u64 a, u64 b, u64 c) { double x, y, z, r; memcpy(&x,&a,8); memcpy(&y,&b,8); memcpy(&z,&c,8); \
    asm volatile(insn " %d0, %d1, %d2, %d3" : "=w"(r) : "w"(x), "w"(y), "w"(z)); u64 o; memcpy(&o,&r,8); return o; }
#define OP2S(nm, insn) static u32 nm(u32 a) { float x, r; memcpy(&x,&a,4); \
    asm volatile(insn " %s0, %s1" : "=w"(r) : "w"(x)); u32 o; memcpy(&o,&r,4); return o; }
#define OP2D(nm, insn) static u64 nm(u64 a) { double x, r; memcpy(&x,&a,8); \
    asm volatile(insn " %d0, %d1" : "=w"(r) : "w"(x)); u64 o; memcpy(&o,&r,8); return o; }

#define ALL3(insn) OP3S(s_##insn, #insn) OP3D(d_##insn, #insn)
ALL3(fadd) ALL3(fsub) ALL3(fmul) ALL3(fdiv) ALL3(fnmul) ALL3(fmax) ALL3(fmin) ALL3(fmaxnm) ALL3(fminnm)
ALL3(fabd) ALL3(fcmeq) ALL3(fcmge) ALL3(fcmgt) ALL3(facge) ALL3(facgt) ALL3(fmulx)
#define ALL4(insn) OP4S(s_##insn, #insn) OP4D(d_##insn, #insn)
ALL4(fmadd) ALL4(fmsub) ALL4(fnmadd) ALL4(fnmsub)
#define ALL2(insn) OP2S(s_##insn, #insn) OP2D(d_##insn, #insn)
ALL2(fsqrt) ALL2(frintn) ALL2(frintp) ALL2(frintm) ALL2(frintz) ALL2(frintx) ALL2(frinti) ALL2(frinta) ALL2(fabs) ALL2(fneg)

static u64 fcvt_ds(u32 a) { float x; double r; memcpy(&x,&a,4); asm volatile("fcvt %d0, %s1" : "=w"(r) : "w"(x)); u64 o; memcpy(&o,&r,8); return o; }
static u32 fcvt_sd(u64 a) { double x; float r; memcpy(&x,&a,8); asm volatile("fcvt %s0, %d1" : "=w"(r) : "w"(x)); u32 o; memcpy(&o,&r,4); return o; }
#define CVTI(nm, insn, rt, rr, ft, fr, fsz) static u64 nm(u64 a) { ft x; u64 r = 0; memcpy(&x,&a,fsz); \
    asm volatile(insn " %" rr "0, %" fr "1" : "=r"(r) : "w"(x)); return r; }
CVTI(zs_ws, "fcvtzs", u32, "w", float, "s", 4) CVTI(zs_xs, "fcvtzs", u64, "x", float, "s", 4)
CVTI(zs_wd, "fcvtzs", u32, "w", double, "d", 8) CVTI(zs_xd, "fcvtzs", u64, "x", double, "d", 8)
CVTI(zu_ws, "fcvtzu", u32, "w", float, "s", 4) CVTI(zu_xs, "fcvtzu", u64, "x", float, "s", 4)
CVTI(zu_wd, "fcvtzu", u32, "w", double, "d", 8) CVTI(zu_xd, "fcvtzu", u64, "x", double, "d", 8)
CVTI(ns_xd, "fcvtns", u64, "x", double, "d", 8) CVTI(as_wd, "fcvtas", u32, "w", double, "d", 8)
#define CVTF(nm, insn, fr, ft, fsz, rr) static u64 nm(u64 a) { ft r; u64 o = 0; \
    asm volatile(insn " %" fr "0, %" rr "1" : "=w"(r) : "r"(a)); memcpy(&o,&r,fsz); return o; }
CVTF(scvtf_sw, "scvtf", "s", float, 4, "w") CVTF(scvtf_sx, "scvtf", "s", float, 4, "x")
CVTF(scvtf_dw, "scvtf", "d", double, 8, "w") CVTF(scvtf_dx, "scvtf", "d", double, 8, "x")
CVTF(ucvtf_sw, "ucvtf", "s", float, 4, "w") CVTF(ucvtf_sx, "ucvtf", "s", float, 4, "x")
CVTF(ucvtf_dw, "ucvtf", "d", double, 8, "w") CVTF(ucvtf_dx, "ucvtf", "d", double, 8, "x")

typedef struct { u64 lo, hi; } v128;
#define VOP3(nm, insn, arr) static v128 nm(v128 a, v128 b) { __uint128_t x, y, r; memcpy(&x,&a,16); memcpy(&y,&b,16); \
    asm volatile(insn " %0." arr ", %1." arr ", %2." arr : "=w"(r) : "w"(x), "w"(y)); v128 o; memcpy(&o,&r,16); return o; }
#define VOPACC(nm, insn, arr) static v128 nm(v128 a, v128 b, v128 c) { __uint128_t x, y, r; memcpy(&x,&a,16); memcpy(&y,&b,16); memcpy(&r,&c,16); \
    asm volatile(insn " %0." arr ", %1." arr ", %2." arr : "+w"(r) : "w"(x), "w"(y)); v128 o; memcpy(&o,&r,16); return o; }
#define VALL(insn) VOP3(v4_##insn, #insn, "4s") VOP3(v2_##insn, #insn, "2d") VOP3(v2s_##insn, #insn, "2s")
VALL(fadd) VALL(fsub) VALL(fmul) VALL(fdiv) VALL(fmax) VALL(fmin) VALL(fmaxnm) VALL(fminnm) VALL(fabd)
VALL(fcmeq) VALL(fcmge) VALL(fcmgt) VALL(facge) VALL(facgt)
VOPACC(v4_fmla, "fmla", "4s") VOPACC(v2_fmla, "fmla", "2d") VOPACC(v4_fmls, "fmls", "4s") VOPACC(v2_fmls, "fmls", "2d")
#define VOP2(nm, insn, arr) static v128 nm(v128 a) { __uint128_t x, r; memcpy(&x,&a,16); \
    asm volatile(insn " %0." arr ", %1." arr : "=w"(r) : "w"(x)); v128 o; memcpy(&o,&r,16); return o; }
#define VALL2(insn) VOP2(w4_##insn, #insn, "4s") VOP2(w2_##insn, #insn, "2d") VOP2(w2s_##insn, #insn, "2s")
VALL2(fsqrt) VALL2(frintn) VALL2(frintp) VALL2(frintm) VALL2(frintz) VALL2(frinta) VALL2(frinti) VALL2(frintx)
VALL2(scvtf) VALL2(ucvtf) VALL2(fcvtzs) VALL2(fcvtzu)
/* by element */
#define VIDX(nm, insn, arr, el) static v128 nm(v128 a, v128 b, v128 c) { __uint128_t x, y, r; memcpy(&x,&a,16); memcpy(&y,&b,16); memcpy(&r,&c,16); \
    asm volatile(insn " %0." arr ", %1." arr ", %2." el : "+w"(r) : "w"(x), "w"(y)); v128 o; memcpy(&o,&r,16); return o; }
VIDX(x4_fmul1, "fmul", "4s", "s[1]") VIDX(x4_fmla3, "fmla", "4s", "s[3]") VIDX(x4_fmls0, "fmls", "4s", "s[0]")
VIDX(x2_fmul1, "fmul", "2d", "d[1]") VIDX(x2_fmla0, "fmla", "2d", "d[0]") VIDX(x2_fmls1, "fmls", "2d", "d[1]")
VIDX(x2s_fmla2, "fmla", "2s", "s[2]")

static u64 fcmp_s(u32 a, u32 b, int e) { float x, y; memcpy(&x,&a,4); memcpy(&y,&b,4);
    if (e) asm volatile("fcmpe %s0, %s1" :: "w"(x), "w"(y) : "cc"); else asm volatile("fcmp %s0, %s1" :: "w"(x), "w"(y) : "cc"); return getnzcv(); }
static u64 fcmp_d(u64 a, u64 b, int e) { double x, y; memcpy(&x,&a,8); memcpy(&y,&b,8);
    if (e) asm volatile("fcmpe %d0, %d1" :: "w"(x), "w"(y) : "cc"); else asm volatile("fcmp %d0, %d1" :: "w"(x), "w"(y) : "cc"); return getnzcv(); }
static u64 fcmp0_d(u64 a) { double x; memcpy(&x,&a,8); asm volatile("fcmp %d0, #0.0" :: "w"(x) : "cc"); return getnzcv(); }

static unsigned long long nlines;
#define P(fmt, ...) do { printf(fmt " fpsr %02llx\n", __VA_ARGS__, (unsigned long long)getfpsr()); nlines++; } while (0)

static void run(int n)
{
    for (int it = 0; it < n; it++) {
        u32 a = rf(), b = rf(), c = rf();
        u64 A = rd(), B = rd(), C = rd();
        if (rnd() % 8 == 0) b = a ^ (u32)(rnd() & 1);  /* close operands */
        if (rnd() % 8 == 0) B = A ^ (rnd() & 1);
#define T3(f) clrfpsr(); P(#f "s %08x %08x -> %08x", a, b, s_##f(a, b)); clrfpsr(); P(#f "d %016llx %016llx -> %016llx", (unsigned long long)A, (unsigned long long)B, (unsigned long long)d_##f(A, B));
        T3(fadd) T3(fsub) T3(fmul) T3(fdiv) T3(fnmul) T3(fmax) T3(fmin) T3(fmaxnm) T3(fminnm) T3(fabd)
        T3(fcmeq) T3(fcmge) T3(fcmgt) T3(facge) T3(facgt) T3(fmulx)
#define T4(f) clrfpsr(); P(#f "s %08x %08x %08x -> %08x", a, b, c, s_##f(a, b, c)); clrfpsr(); P(#f "d %016llx %016llx %016llx -> %016llx", (unsigned long long)A, (unsigned long long)B, (unsigned long long)C, (unsigned long long)d_##f(A, B, C));
        T4(fmadd) T4(fmsub) T4(fnmadd) T4(fnmsub)
#define T2(f) clrfpsr(); P(#f "s %08x -> %08x", a, s_##f(a)); clrfpsr(); P(#f "d %016llx -> %016llx", (unsigned long long)A, (unsigned long long)d_##f(A));
        T2(fsqrt) T2(frintn) T2(frintp) T2(frintm) T2(frintz) T2(frintx) T2(frinti) T2(frinta) T2(fabs) T2(fneg)
        clrfpsr(); P("fcvtds %08x -> %016llx", a, (unsigned long long)fcvt_ds(a));
        clrfpsr(); P("fcvtsd %016llx -> %08x", (unsigned long long)A, fcvt_sd(A));
#define TC(f, src) clrfpsr(); P(#f " %016llx -> %016llx", (unsigned long long)(src), (unsigned long long)f(src));
        TC(zs_ws, a) TC(zs_xs, a) TC(zs_wd, A) TC(zs_xd, A) TC(zu_ws, a) TC(zu_xs, a) TC(zu_wd, A) TC(zu_xd, A)
        TC(ns_xd, A) TC(as_wd, A)
        {
            u64 I = rnd(); if (rnd() % 3 == 0) I >>= rnd() % 64; if (rnd() % 4 == 0) I = -I;
            TC(scvtf_sw, I) TC(scvtf_sx, I) TC(scvtf_dw, I) TC(scvtf_dx, I)
            TC(ucvtf_sw, I) TC(ucvtf_sx, I) TC(ucvtf_dw, I) TC(ucvtf_dx, I)
        }
        clrfpsr(); P("fcmps %08x %08x -> %llx", a, b, (unsigned long long)fcmp_s(a, b, 0));
        clrfpsr(); P("fcmpes %08x %08x -> %llx", a, b, (unsigned long long)fcmp_s(a, b, 1));
        clrfpsr(); P("fcmpd %016llx %016llx -> %llx", (unsigned long long)A, (unsigned long long)B, (unsigned long long)fcmp_d(A, B, 0));
        clrfpsr(); P("fcmped %016llx %016llx -> %llx", (unsigned long long)A, (unsigned long long)B, (unsigned long long)fcmp_d(A, B, 1));
        clrfpsr(); P("fcmp0d %016llx -> %llx", (unsigned long long)A, (unsigned long long)fcmp0_d(A));
        {
            v128 va = { (u64)rf() | (u64)rf() << 32, (u64)rf() | (u64)rf() << 32 };
            v128 vb = { (u64)rf() | (u64)rf() << 32, (u64)rf() | (u64)rf() << 32 };
            v128 vc = { (u64)rf() | (u64)rf() << 32, (u64)rf() | (u64)rf() << 32 };
            v128 da = { rd(), rd() }, db = { rd(), rd() }, dc = { rd(), rd() }, r;
#define TV(f) clrfpsr(); r = v4_##f(va, vb); P(#f "4s %016llx%016llx -> %016llx%016llx", (unsigned long long)va.hi, (unsigned long long)va.lo, (unsigned long long)r.hi, (unsigned long long)r.lo); \
              clrfpsr(); r = v2_##f(da, db); P(#f "2d %016llx%016llx -> %016llx%016llx", (unsigned long long)da.hi, (unsigned long long)da.lo, (unsigned long long)r.hi, (unsigned long long)r.lo); \
              clrfpsr(); r = v2s_##f(va, vb); P(#f "2s %016llx -> %016llx%016llx", (unsigned long long)va.lo, (unsigned long long)r.hi, (unsigned long long)r.lo);
            TV(fadd) TV(fsub) TV(fmul) TV(fdiv) TV(fmax) TV(fmin) TV(fmaxnm) TV(fminnm) TV(fabd)
            TV(fcmeq) TV(fcmge) TV(fcmgt) TV(facge) TV(facgt)
            clrfpsr(); r = v4_fmla(va, vb, vc); P("fmla4s %016llx%016llx -> %016llx%016llx", (unsigned long long)vc.hi, (unsigned long long)vc.lo, (unsigned long long)r.hi, (unsigned long long)r.lo);
            clrfpsr(); r = v4_fmls(va, vb, vc); P("fmls4s %016llx%016llx -> %016llx%016llx", (unsigned long long)vc.hi, (unsigned long long)vc.lo, (unsigned long long)r.hi, (unsigned long long)r.lo);
            clrfpsr(); r = v2_fmla(da, db, dc); P("fmla2d %016llx%016llx -> %016llx%016llx", (unsigned long long)dc.hi, (unsigned long long)dc.lo, (unsigned long long)r.hi, (unsigned long long)r.lo);
            clrfpsr(); r = v2_fmls(da, db, dc); P("fmls2d %016llx%016llx -> %016llx%016llx", (unsigned long long)dc.hi, (unsigned long long)dc.lo, (unsigned long long)r.hi, (unsigned long long)r.lo);
#define TW(f, x4, x2) clrfpsr(); r = w4_##f(x4); P(#f "4s %016llx%016llx -> %016llx%016llx", (unsigned long long)x4.hi, (unsigned long long)x4.lo, (unsigned long long)r.hi, (unsigned long long)r.lo); \
              clrfpsr(); r = w2_##f(x2); P(#f "2d %016llx%016llx -> %016llx%016llx", (unsigned long long)x2.hi, (unsigned long long)x2.lo, (unsigned long long)r.hi, (unsigned long long)r.lo); \
              clrfpsr(); r = w2s_##f(x4); P(#f "2s %016llx -> %016llx%016llx", (unsigned long long)x4.lo, (unsigned long long)r.hi, (unsigned long long)r.lo);
            TW(fsqrt, va, da) TW(frintn, va, da) TW(frintp, va, da) TW(frintm, va, da) TW(frintz, va, da)
            TW(frinta, va, da) TW(frinti, va, da) TW(frintx, va, da) TW(fcvtzs, va, da) TW(fcvtzu, va, da)
#define TX(f, x, y, z) clrfpsr(); r = f(x, y, z); P(#f " %016llx%016llx %016llx%016llx -> %016llx%016llx", (unsigned long long)x.hi, (unsigned long long)x.lo, \
              (unsigned long long)y.hi, (unsigned long long)y.lo, (unsigned long long)r.hi, (unsigned long long)r.lo);
            TX(x4_fmul1, va, vb, vc) TX(x4_fmla3, va, vb, vc) TX(x4_fmls0, va, vb, vc) TX(x2s_fmla2, va, vb, vc)
            TX(x2_fmul1, da, db, dc) TX(x2_fmla0, da, db, dc) TX(x2_fmls1, da, db, dc)
            {
                u64 i0 = rnd(), i1 = rnd();
                if (rnd() % 2) { i0 >>= rnd() % 64; i1 >>= rnd() % 64; }
                if (rnd() % 4 == 0) { i0 = -i0; }
                v128 vi = { i0, i1 };
                TW(scvtf, vi, vi) TW(ucvtf, vi, vi)
            }
        }
        /* flags accumulate across ops */
        clrfpsr(); s_fadd(a, b); d_fmul(A, B); s_fsqrt(c); d_fdiv(A, C); P("acc %08x %016llx -> %d", a, (unsigned long long)A, 0);
    }
}

static volatile int got;
static void handler(int sig) { double x = 1e308; volatile double y = x * 10; (void)y; got++; }

static void *thr(void *arg) { run(200); return NULL; }

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 2000;
    if (getenv("UNBUF")) setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 2) rs ^= strtoull(argv[2], NULL, 0) * 0x9e3779b97f4a7c15ull;
    run(n);
    /* signal: flags of the handler do not leak, the interrupted ones survive */
    signal(SIGUSR1, handler);
    clrfpsr(); s_fdiv(0x3f800000, 0);  /* DZC */
    raise(SIGUSR1);
    P("signal %d -> %d", got, 0);
    /* FPCR non-default: helpers; then back */
    for (int m = 0; m < 4; m++) {
        static const u64 fpcr[] = { 1u << 24, 1u << 25, 3u << 22, 1u << 22 };
        asm volatile("msr fpcr, %0" :: "r"(fpcr[m]));
        for (int i = 0; i < 300; i++) {
            u32 a = rf(), b = rf(); u64 A = rd(), B = rd();
            T3(fadd) T3(fmul) T3(fdiv) T3(fmaxnm)
        }
        asm volatile("msr fpcr, xzr");
    }
    /* thread */
    pthread_t t;
    pthread_create(&t, NULL, thr, NULL);
    pthread_join(t, NULL);
    fprintf(stderr, "%llu lines\n", nlines);
    return 0;
}
