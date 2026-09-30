/* Differential test of AArch32 VFP: results + FPSCR. */
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
    0x4f000000, 0xcf000000, 0x4f800000, 0x3f7fffff, 0x4b000000, 0x4affffff, 0x3effffff,
};
static const u64 dsp[] = {
    0, 0x8000000000000000ull, 1, 0x000fffffffffffffull, 0x0010000000000000ull,
    0x3ff0000000000000ull, 0xbff0000000000000ull, 0x3fe0000000000000ull, 0x3ff8000000000000ull,
    0x7fefffffffffffffull, 0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull,
    0xfff8000000000000ull, 0x7ff4000000000001ull, 0xfff4000000000123ull, 0x41e0000000000000ull,
    0xc1e0000000000000ull, 0x41f0000000000000ull, 0x41dfffffffe00000ull, 0x3810000000000000ull,
    0x380fffffffffffffull, 0x36a0000000000000ull, 0x47efffffe0000000ull, 0x47effffff0000000ull,
    0x4330000000000000ull,
};
static u32 rf(void)
{
    switch (rnd() % 8) {
    case 0: case 1: return fsp[rnd() % (sizeof(fsp) / 4)];
    case 2: return (u32)rnd();
    case 3: return ((u32)rnd() & 0x807fffff) | (u32)(rnd() % 6) << 23;
    case 4: return ((u32)rnd() & 0x807fffff) | (u32)(250 + rnd() % 5) << 23;
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
    case 5: return (rnd() & 0x800fffffe0000000ull) | (1023 - 30 + rnd() % 60) << 52;
    default: return (rnd() & 0x800fffffffffffffull) | (1023 - 60 + rnd() % 120) << 52;
    }
}
static inline void clr(void) { u32 z = 0; asm volatile("vmsr fpscr, %0" :: "r"(z)); }
static inline u32 fpscr(void) { u32 x; asm volatile("vmrs %0, fpscr" : "=r"(x)); return x; }

#define S3(nm, insn) static u32 nm(u32 a, u32 b) { float x, y, r; memcpy(&x,&a,4); memcpy(&y,&b,4); \
    asm volatile(insn " %0, %1, %2" : "=t"(r) : "t"(x), "t"(y)); u32 o; memcpy(&o,&r,4); return o; }
#define D3(nm, insn) static u64 nm(u64 a, u64 b) { double x, y, r; memcpy(&x,&a,8); memcpy(&y,&b,8); \
    asm volatile(insn " %P0, %P1, %P2" : "=w"(r) : "w"(x), "w"(y)); u64 o; memcpy(&o,&r,8); return o; }
#define SA(nm, insn) static u32 nm(u32 a, u32 b, u32 c) { float x, y, r; memcpy(&x,&a,4); memcpy(&y,&b,4); memcpy(&r,&c,4); \
    asm volatile(insn " %0, %1, %2" : "+t"(r) : "t"(x), "t"(y)); u32 o; memcpy(&o,&r,4); return o; }
#define DA(nm, insn) static u64 nm(u64 a, u64 b, u64 c) { double x, y, r; memcpy(&x,&a,8); memcpy(&y,&b,8); memcpy(&r,&c,8); \
    asm volatile(insn " %P0, %P1, %P2" : "+w"(r) : "w"(x), "w"(y)); u64 o; memcpy(&o,&r,8); return o; }
S3(s_add, "vadd.f32") S3(s_sub, "vsub.f32") S3(s_mul, "vmul.f32") S3(s_div, "vdiv.f32") S3(s_nmul, "vnmul.f32")
S3(s_maxnm, "vmaxnm.f32") S3(s_minnm, "vminnm.f32")
D3(d_add, "vadd.f64") D3(d_sub, "vsub.f64") D3(d_mul, "vmul.f64") D3(d_div, "vdiv.f64") D3(d_nmul, "vnmul.f64")
D3(d_maxnm, "vmaxnm.f64") D3(d_minnm, "vminnm.f64")
SA(s_mla, "vmla.f32") SA(s_mls, "vmls.f32") SA(s_nmla, "vnmla.f32") SA(s_nmls, "vnmls.f32")
SA(s_fma, "vfma.f32") SA(s_fms, "vfms.f32") SA(s_fnma, "vfnma.f32") SA(s_fnms, "vfnms.f32")
DA(d_mla, "vmla.f64") DA(d_mls, "vmls.f64") DA(d_nmla, "vnmla.f64") DA(d_nmls, "vnmls.f64")
DA(d_fma, "vfma.f64") DA(d_fms, "vfms.f64") DA(d_fnma, "vfnma.f64") DA(d_fnms, "vfnms.f64")
#define S2(nm, insn) static u32 nm(u32 a) { float x, r; memcpy(&x,&a,4); asm volatile(insn " %0, %1" : "=t"(r) : "t"(x)); u32 o; memcpy(&o,&r,4); return o; }
#define D2(nm, insn) static u64 nm(u64 a) { double x, r; memcpy(&x,&a,8); asm volatile(insn " %P0, %P1" : "=w"(r) : "w"(x)); u64 o; memcpy(&o,&r,8); return o; }
S2(s_sqrt, "vsqrt.f32") D2(d_sqrt, "vsqrt.f64") S2(s_rintz, "vrintz.f32") D2(d_rintz, "vrintz.f64")
S2(s_rintr, "vrintr.f32") D2(d_rintr, "vrintr.f64") S2(s_rintx, "vrintx.f32") D2(d_rintx, "vrintx.f64")
S2(s_rintn, "vrintn.f32") D2(d_rintn, "vrintn.f64") S2(s_rintm, "vrintm.f32") D2(d_rintm, "vrintm.f64")
S2(s_cvt_s32, "vcvt.s32.f32") S2(s_cvt_u32, "vcvt.u32.f32") S2(s_cvtr_s32, "vcvtr.s32.f32") S2(s_cvtr_u32, "vcvtr.u32.f32")
S2(s_cvt_fs, "vcvt.f32.s32") S2(s_cvt_fu, "vcvt.f32.u32")
static u32 d_cvt_s32(u64 a) { double x; float r; memcpy(&x,&a,8); asm volatile("vcvt.s32.f64 %0, %P1" : "=t"(r) : "w"(x)); u32 o; memcpy(&o,&r,4); return o; }
static u32 d_cvt_u32(u64 a) { double x; float r; memcpy(&x,&a,8); asm volatile("vcvt.u32.f64 %0, %P1" : "=t"(r) : "w"(x)); u32 o; memcpy(&o,&r,4); return o; }
static u32 d_cvtr_s32(u64 a) { double x; float r; memcpy(&x,&a,8); asm volatile("vcvtr.s32.f64 %0, %P1" : "=t"(r) : "w"(x)); u32 o; memcpy(&o,&r,4); return o; }
static u64 d_cvt_fs(u32 a) { float x; double r; memcpy(&x,&a,4); asm volatile("vcvt.f64.s32 %P0, %1" : "=w"(r) : "t"(x)); u64 o; memcpy(&o,&r,8); return o; }
static u64 d_cvt_fu(u32 a) { float x; double r; memcpy(&x,&a,4); asm volatile("vcvt.f64.u32 %P0, %1" : "=w"(r) : "t"(x)); u64 o; memcpy(&o,&r,8); return o; }
static u64 cvt_ds(u32 a) { float x; double r; memcpy(&x,&a,4); asm volatile("vcvt.f64.f32 %P0, %1" : "=w"(r) : "t"(x)); u64 o; memcpy(&o,&r,8); return o; }
static u32 cvt_sd(u64 a) { double x; float r; memcpy(&x,&a,8); asm volatile("vcvt.f32.f64 %0, %P1" : "=t"(r) : "w"(x)); u32 o; memcpy(&o,&r,4); return o; }
static u32 s_cmp(u32 a, u32 b, int e, int z) { float x, y; memcpy(&x,&a,4); memcpy(&y,&b,4); u32 f;
    if (z) { if (e) asm volatile("vcmpe.f32 %1, #0\n\tvmrs %0, fpscr" : "=r"(f) : "t"(x)); else asm volatile("vcmp.f32 %1, #0\n\tvmrs %0, fpscr" : "=r"(f) : "t"(x)); }
    else if (e) asm volatile("vcmpe.f32 %1, %2\n\tvmrs %0, fpscr" : "=r"(f) : "t"(x), "t"(y)); else asm volatile("vcmp.f32 %1, %2\n\tvmrs %0, fpscr" : "=r"(f) : "t"(x), "t"(y)); return f; }
static u32 d_cmp(u64 a, u64 b, int e) { double x, y; memcpy(&x,&a,8); memcpy(&y,&b,8); u32 f;
    if (e) asm volatile("vcmpe.f64 %P1, %P2\n\tvmrs %0, fpscr" : "=r"(f) : "w"(x), "w"(y)); else asm volatile("vcmp.f64 %P1, %P2\n\tvmrs %0, fpscr" : "=r"(f) : "w"(x), "w"(y)); return f; }
static int apsr_cmp(u64 a, u64 b) { double x, y; memcpy(&x,&a,8); memcpy(&y,&b,8); int r;
    asm volatile("vcmp.f64 %P1, %P2\n\tvmrs APSR_nzcv, fpscr\n\tmov %0, #0\n\tit gt\n\tmovgt %0, #1\n\tit mi\n\tmovmi %0, #2\n\tit vs\n\tmovvs %0, #3" : "=r"(r) : "w"(x), "w"(y) : "cc"); return r; }
/* S register pairs: write s1 of a d pair must keep s0 */
static u64 pair(u32 a, u32 b) { u64 r; asm volatile("vmov d16, %Q1, %R1\n\tvadd.f32 s1, s1, s1\n\tvmov %Q0, %R0, d16" : "=r"(r) : "r"((u64)a | (u64)b << 32) : "d16"); return r; }

static unsigned long long nl;
#define P(fmt, ...) do { printf(fmt " fpscr %08x\n", __VA_ARGS__, fpscr()); nl++; } while (0)
static void run(int n)
{
    for (int it = 0; it < n; it++) {
        u32 a = rf(), b = rf(), c = rf(); u64 A = rd(), B = rd(), C = rd();
        if (rnd() % 8 == 0) b = a ^ (u32)(rnd() & 1);
        if (rnd() % 8 == 0) B = A ^ (rnd() & 1);
#define T3(f) clr(); P(#f "s %08x %08x -> %08x", a, b, s_##f(a, b)); clr(); P(#f "d %016llx %016llx -> %016llx", A, B, d_##f(A, B));
        T3(add) T3(sub) T3(mul) T3(div) T3(nmul) T3(maxnm) T3(minnm)
#define TA(f) clr(); P(#f "s %08x %08x %08x -> %08x", a, b, c, s_##f(a, b, c)); clr(); P(#f "d %016llx %016llx %016llx -> %016llx", A, B, C, d_##f(A, B, C));
        TA(mla) TA(mls) TA(nmla) TA(nmls) TA(fma) TA(fms) TA(fnma) TA(fnms)
#define T2(f) clr(); P(#f "s %08x -> %08x", a, s_##f(a)); clr(); P(#f "d %016llx -> %016llx", A, d_##f(A));
        T2(sqrt) T2(rintz) T2(rintr) T2(rintx) T2(rintn) T2(rintm)
#define T1(f, x, fmt) clr(); P(#f " " fmt " -> %016llx", x, (u64)f(x));
        T1(s_cvt_s32, a, "%08x") T1(s_cvt_u32, a, "%08x") T1(s_cvtr_s32, a, "%08x") T1(s_cvtr_u32, a, "%08x")
        T1(s_cvt_fs, (u32)rnd(), "%08x") T1(s_cvt_fu, (u32)rnd(), "%08x")
        T1(d_cvt_s32, A, "%016llx") T1(d_cvt_u32, A, "%016llx") T1(d_cvtr_s32, A, "%016llx")
        T1(d_cvt_fs, (u32)rnd(), "%08x") T1(d_cvt_fu, (u32)rnd(), "%08x")
        T1(cvt_ds, a, "%08x") T1(cvt_sd, A, "%016llx")
        clr(); P("cmps %08x %08x -> %08x", a, b, s_cmp(a, b, 0, 0));
        clr(); P("cmpes %08x %08x -> %08x", a, b, s_cmp(a, b, 1, 0));
        clr(); P("cmpzs %08x -> %08x", a, s_cmp(a, b, 1, 1));
        clr(); P("cmpd %016llx %016llx -> %08x", A, B, d_cmp(A, B, 0));
        clr(); P("cmped %016llx %016llx -> %08x", A, B, d_cmp(A, B, 1));
        clr(); P("apsr %016llx %016llx -> %d", A, B, apsr_cmp(A, B));
        clr(); P("pair %08x %08x -> %016llx", a, b, pair(a, b));
        clr(); s_add(a, b); d_mul(A, B); s_sqrt(c); d_div(A, C); P("acc %08x -> %d", a, 0);
    }
}
static volatile int got;
static void handler(int sig) { volatile double x = 1e308; volatile double y = x * 10; (void)y; got++; }
static void *thr(void *arg) { run(200); return NULL; }
int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 2000;
    if (argc > 2) rs ^= strtoull(argv[2], NULL, 0) * 0x9e3779b97f4a7c15ull;
    if (getenv("UNBUF")) setvbuf(stdout, NULL, _IONBF, 0);
    run(n);
    signal(SIGUSR1, handler);
    clr(); s_div(0x3f800000, 0); raise(SIGUSR1); P("signal %d -> %d", got, 0);
    static const u32 modes[] = { 1u << 24, 1u << 25, 3u << 22, 1u << 22, 2u << 22 };
    for (int m = 0; m < 5; m++) {
        for (int i = 0; i < 300; i++) {
            u32 a = rf(), b = rf(); u64 A = rd(), B = rd();
            asm volatile("vmsr fpscr, %0" :: "r"(modes[m]));
            P("mode%d adds %08x %08x -> %08x", m, a, b, s_add(a, b));
            asm volatile("vmsr fpscr, %0" :: "r"(modes[m]));
            P("mode%d muld %016llx %016llx -> %016llx", m, A, B, d_mul(A, B));
            asm volatile("vmsr fpscr, %0" :: "r"(modes[m]));
            P("mode%d cvtr %016llx -> %08x", m, A, d_cvtr_s32(A));
        }
    }
    clr();
    pthread_t t; pthread_create(&t, NULL, thr, NULL); pthread_join(t, NULL);
    fprintf(stderr, "%llu lines\n", nl);
    return 0;
}
