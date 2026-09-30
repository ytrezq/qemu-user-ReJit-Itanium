/*
 * Differential test of the scalar FP instructions: prints the result and
 * the FPSCR after each instruction for many operand combinations, and the
 * outcome of a few instruction sequences (sticky flags, lazy FPRF,
 * rounding modes, enabled exceptions, signals, threads).  The output of
 * the JIT build is compared with the reference QEMU (fpcmp.py).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fenv.h>
#include <signal.h>
#include <setjmp.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/wait.h>

typedef uint64_t u64;
typedef uint32_t u32;

static inline double d_of(u64 x) { double d; memcpy(&d, &x, 8); return d; }
static inline u64 u_of(double d) { u64 x; memcpy(&x, &d, 8); return x; }

static const u64 specials[] = {
    0x0000000000000000ull, 0x8000000000000000ull,   /* +-0 */
    0x0000000000000001ull, 0x8000000000000001ull,   /* +-min denormal */
    0x000fffffffffffffull, 0x800fffffffffffffull,   /* +-max denormal */
    0x0010000000000000ull, 0x8010000000000000ull,   /* +-min normal */
    0x3ff0000000000000ull, 0xbff0000000000000ull,   /* +-1 */
    0x3ff8000000000000ull, 0xbff8000000000000ull,   /* +-1.5 */
    0x4000000000000000ull, 0x3fe0000000000000ull,   /* 2, 0.5 */
    0xbfe0000000000000ull, 0x4004000000000000ull,   /* -0.5, 2.5 */
    0xc004000000000000ull, 0x3ff0000000000001ull,   /* -2.5, 1+ulp */
    0x7fefffffffffffffull, 0xffefffffffffffffull,   /* +-max */
    0x7ff0000000000000ull, 0xfff0000000000000ull,   /* +-inf */
    0x7ff8000000000000ull, 0xfff8000000000000ull,   /* +-qnan */
    0x7ffc000000000abcull, 0x7ff0000000000001ull,   /* qnan payload, snan */
    0xfff4000000000077ull,                          /* -snan payload */
    0x47efffffe0000000ull, 0x47effffff0000000ull,   /* FLT_MAX, +half ulp */
    0x47f0000000000000ull, 0x3810000000000000ull,   /* 2^128, FLT_MIN */
    0x36a0000000000000ull, 0x3690000000000000ull,   /* 2^-149, 2^-150 */
    0x380fffffe0000000ull, 0x3800000010000000ull,   /* max sp denorm, odd */
    0x41dfffffffc00000ull, 0x41e0000000000000ull,   /* 2^31-1, 2^31 */
    0xc1e0000000000000ull, 0xc1e0000000200000ull,   /* -2^31, -2^31-1 */
    0x41efffffffe00000ull, 0x41f0000000000000ull,   /* 2^32-1, 2^32 */
    0x43dfffffffffffffull, 0x43e0000000000000ull,   /* <2^63, 2^63 */
    0xc3e0000000000000ull, 0x43efffffffffffffull,   /* -2^63, <2^64 */
    0x43f0000000000000ull, 0x4330000000000001ull,   /* 2^64, 2^52+1 */
    0x3fdfffffffffffffull, 0x400921fb54442d18ull,   /* 0.5-ulp, pi */
    0xc00921fb54442d18ull, 0x3fb999999999999aull,   /* -pi, 0.1 */
    0x4024000000000000ull, 0x3ff0000010000000ull,   /* 10, 1+2^-28 */
    0xc1dfffffffe00000ull, 0x41dfffffffe00000ull,   /* -(2^31-0.5), 2^31-0.5 */
    0x7fe0000000000000ull, 0x0020000000000000ull,   /* 2^1023, 2^-1021 */
};
#define NSPEC (sizeof(specials) / sizeof(specials[0]))

static u64 vals[512];
static int nvals;

static u64 rng_state = 0x9e3779b97f4a7c15ull;
static u64 rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static void build_vals(int nrand)
{
    nvals = 0;
    for (size_t i = 0; i < NSPEC; i++) {
        vals[nvals++] = specials[i];
    }
    for (int i = 0; i < nrand; i++) {
        u64 r = rnd();
        int kind = i % 4;
        u64 e;
        if (kind == 0) {            /* moderate exponent */
            e = 1023 - 40 + (rnd() % 80);
        } else if (kind == 1) {     /* single-precision edge exponents */
            e = (rnd() & 1) ? 1023 + 120 + rnd() % 16 : 1023 - 150 + rnd() % 30;
        } else if (kind == 2) {     /* anything */
            e = rnd() % 2048;
        } else {                    /* near 1, few mantissa bits */
            e = 1023 + (rnd() % 8) - 4;
            r &= ~0xffffffull;
        }
        vals[nvals++] = (r & 0x800fffffffffffffull) | (e << 52);
    }
}

#define ZERO_FPSCR "mtfsf 0xff,%[z]\n\t"

/* d = op(a, b) */
#define OP2(name, insn)                                                    \
static void t_##name(u64 a, u64 b, u64 *r, u64 *f)                         \
{                                                                          \
    double res, fs;                                                        \
    asm volatile(ZERO_FPSCR insn " %[r],%[a],%[b]\n\tmffs %[f]"            \
                 : [r]"=&d"(res), [f]"=&d"(fs)                             \
                 : [a]"d"(d_of(a)), [b]"d"(d_of(b)), [z]"d"(0.0));         \
    *r = u_of(res); *f = u_of(fs);                                         \
}
/* d = op(a, c, b) for the A-form multiply-adds: frt,fra,frc,frb */
#define OP3(name, insn)                                                    \
static void t_##name(u64 a, u64 b, u64 c, u64 *r, u64 *f)                  \
{                                                                          \
    double res, fs;                                                        \
    asm volatile(ZERO_FPSCR insn " %[r],%[a],%[c],%[b]\n\tmffs %[f]"       \
                 : [r]"=&d"(res), [f]"=&d"(fs)                             \
                 : [a]"d"(d_of(a)), [b]"d"(d_of(b)), [c]"d"(d_of(c)),      \
                   [z]"d"(0.0));                                           \
    *r = u_of(res); *f = u_of(fs);                                         \
}
#define OP1(name, insn)                                                    \
static void t_##name(u64 b, u64 *r, u64 *f)                                \
{                                                                          \
    double res, fs;                                                        \
    asm volatile(ZERO_FPSCR insn " %[r],%[b]\n\tmffs %[f]"                 \
                 : [r]"=&d"(res), [f]"=&d"(fs)                             \
                 : [b]"d"(d_of(b)), [z]"d"(0.0));                          \
    *r = u_of(res); *f = u_of(fs);                                         \
}
/* record form: also returns CR1 */
#define OP2RC(name, insn)                                                  \
static void t_##name(u64 a, u64 b, u64 *r, u64 *f, u32 *cr)                \
{                                                                          \
    double res, fs;                                                        \
    unsigned long c;                                                       \
    asm volatile(ZERO_FPSCR insn " %[r],%[a],%[b]\n\tmfcr %[c]\n\t"        \
                 "mffs %[f]"                                               \
                 : [r]"=&d"(res), [f]"=&d"(fs), [c]"=&r"(c)                \
                 : [a]"d"(d_of(a)), [b]"d"(d_of(b)), [z]"d"(0.0)           \
                 : "cr1");                                                 \
    *r = u_of(res); *f = u_of(fs); *cr = (c >> 24) & 0xf;                  \
}

OP2(fadd, "fadd") OP2(fadds, "fadds") OP2(fsub, "fsub") OP2(fsubs, "fsubs")
OP2(fdiv, "fdiv") OP2(fdivs, "fdivs") OP2(fmul, "fmul") OP2(fmuls, "fmuls")
OP3(fmadd, "fmadd") OP3(fmadds, "fmadds") OP3(fmsub, "fmsub")
OP3(fmsubs, "fmsubs") OP3(fnmadd, "fnmadd") OP3(fnmadds, "fnmadds")
OP3(fnmsub, "fnmsub") OP3(fnmsubs, "fnmsubs") OP3(fsel, "fsel")
OP1(fsqrt, "fsqrt") OP1(fsqrts, "fsqrts") OP1(frsp, "frsp")
OP1(friz, "friz") OP1(frip, "frip") OP1(frim, "frim") OP1(frin, "frin")
OP1(fre, "fre") OP1(fres, "fres") OP1(frsqrte, "frsqrte")
OP1(frsqrtes, "frsqrtes")
OP1(fctiw, "fctiw") OP1(fctiwz, "fctiwz") OP1(fctiwu, "fctiwu")
OP1(fctiwuz, "fctiwuz") OP1(fctid, "fctid") OP1(fctidz, "fctidz")
OP1(fctidu, "fctidu") OP1(fctiduz, "fctiduz")
OP1(fcfid, "fcfid") OP1(fcfids, "fcfids") OP1(fcfidu, "fcfidu")
OP1(fcfidus, "fcfidus")
OP2RC(fadd_rc, "fadd.") OP2RC(fdiv_rc, "fdiv.") OP2RC(fmuls_rc, "fmuls.")

static void t_fcmpu(u64 a, u64 b, u64 *r, u64 *f)
{
    double fs;
    unsigned long c;
    asm volatile(ZERO_FPSCR "fcmpu 6,%[a],%[b]\n\tmfcr %[c]\n\tmffs %[f]"
                 : [f]"=&d"(fs), [c]"=&r"(c)
                 : [a]"d"(d_of(a)), [b]"d"(d_of(b)), [z]"d"(0.0) : "cr6");
    *r = (c >> 4) & 0xf; *f = u_of(fs);
}

static void t_fcmpo(u64 a, u64 b, u64 *r, u64 *f)
{
    double fs;
    unsigned long c;
    asm volatile(ZERO_FPSCR "fcmpo 6,%[a],%[b]\n\tmfcr %[c]\n\tmffs %[f]"
                 : [f]"=&d"(fs), [c]"=&r"(c)
                 : [a]"d"(d_of(a)), [b]"d"(d_of(b)), [z]"d"(0.0) : "cr6");
    *r = (c >> 4) & 0xf; *f = u_of(fs);
}

static void t_lfs(u32 w, u64 *r)
{
    double res;
    asm volatile("lfs %[r],0(%[p])" : [r]"=d"(res) : [p]"b"(&w) : "memory");
    *r = u_of(res);
}

static void t_stfs(u64 a, u32 *w)
{
    asm volatile("stfs %[a],0(%[p])" : : [a]"d"(d_of(a)), [p]"b"(w)
                 : "memory");
}

typedef void (*fn1)(u64, u64 *, u64 *);
typedef void (*fn2)(u64, u64, u64 *, u64 *);
typedef void (*fn3)(u64, u64, u64, u64 *, u64 *);

static void run1(const char *n, fn1 f, const u64 *v, int nv)
{
    for (int i = 0; i < nv; i++) {
        u64 r, s;
        f(v[i], &r, &s);
        printf("%s %016llx -> %016llx %08llx\n", n, (unsigned long long)v[i],
               (unsigned long long)r, (unsigned long long)(s & 0xffffffff));
    }
}

static void run2(const char *n, fn2 f, const u64 *v, int nv)
{
    for (int i = 0; i < nv; i++) {
        for (int j = 0; j < nv; j++) {
            u64 r, s;
            f(v[i], v[j], &r, &s);
            printf("%s %016llx %016llx -> %016llx %08llx\n", n,
                   (unsigned long long)v[i], (unsigned long long)v[j],
                   (unsigned long long)r,
                   (unsigned long long)(s & 0xffffffff));
        }
    }
}

static void run3(const char *n, fn3 f, const u64 *v, int nv)
{
    for (int i = 0; i < nv; i++) {
        for (int j = 0; j < nv; j++) {
            for (int k = 0; k < nv; k++) {
                u64 r, s;
                f(v[i], v[j], v[k], &r, &s);
                printf("%s %016llx %016llx %016llx -> %016llx %08llx\n", n,
                       (unsigned long long)v[i], (unsigned long long)v[j],
                       (unsigned long long)v[k], (unsigned long long)r,
                       (unsigned long long)(s & 0xffffffff));
            }
        }
    }
}

static u64 get_fpscr(void)
{
    double f;
    asm volatile("mffs %0" : "=d"(f));
    return u_of(f) & 0xffffffff;
}

static void set_fpscr(u64 v)
{
    asm volatile("mtfsf 0xff,%0" : : "d"(d_of(v)));
}

/* ---- sequences: several instructions between FPSCR reads ---- */
static volatile double vd[8] = { 1.0, 3.0, 0.0, 1e308, 1e-308, 0.1, -2.0, 7.0 };

static void seq_tests(void)
{
    double x, y, z;

    /* sticky flags, FPRF from the last result */
    set_fpscr(0);
    x = vd[0] / vd[1];          /* inexact */
    y = vd[0] * vd[6];          /* exact, -2 */
    printf("seq1 %016llx %016llx fpscr %08llx\n", (unsigned long long)u_of(x),
           (unsigned long long)u_of(y), (unsigned long long)get_fpscr());

    set_fpscr(0);
    x = vd[0] / vd[2];          /* ZX, +inf */
    y = vd[3] * vd[3];          /* OX XX +inf */
    z = vd[4] * vd[4];          /* UX XX 0/denormal */
    printf("seq2 %016llx %016llx %016llx fpscr %08llx\n",
           (unsigned long long)u_of(x), (unsigned long long)u_of(y),
           (unsigned long long)u_of(z), (unsigned long long)get_fpscr());

    /* compare after a result: FPCC from the compare, C from the result */
    set_fpscr(0);
    {
        double a = vd[4] * vd[5], b;  /* denormal-ish: C=1 */
        int lt;
        u64 cr;
        asm volatile("fmul %0,%2,%3\n\tfcmpu 7,%3,%4\n\tmfcr %1"
                     : "=&d"(b), "=r"(cr) : "d"(a), "d"(vd[4]), "d"(vd[5])
                     : "cr7");
        lt = cr & 0xf;
        printf("seq3 %016llx %x fpscr %08llx\n", (unsigned long long)u_of(b),
               lt, (unsigned long long)get_fpscr());
    }
    /* compare first, then a result */
    set_fpscr(0);
    {
        double b;
        u64 cr;
        asm volatile("fcmpu 7,%2,%3\n\tfmul %0,%2,%3\n\tmfcr %1"
                     : "=&d"(b), "=r"(cr) : "d"(vd[4]), "d"(vd[5])
                     : "cr7");
        printf("seq4 %016llx %x fpscr %08llx\n", (unsigned long long)u_of(b),
               (unsigned)(cr & 0xf), (unsigned long long)get_fpscr());
    }
    /* mtfsb0 of a flag raised by generated code, then more code */
    set_fpscr(0);
    {
        double a, b;
        u64 f1, f2;
        double t1, t2;
        asm volatile("fdiv %0,%4,%5\n\tmffs %2\n\tmtfsb0 6\n\tmtfsb0 0\n\t"
                     "fadd %1,%4,%4\n\tmffs %3"
                     : "=&d"(a), "=&d"(b), "=&d"(t1), "=&d"(t2)
                     : "d"(vd[0]), "d"(vd[1]));
        f1 = u_of(t1) & 0xffffffff;
        f2 = u_of(t2) & 0xffffffff;
        printf("seq5 %016llx %016llx fpscr %08llx %08llx\n",
               (unsigned long long)u_of(a), (unsigned long long)u_of(b),
               (unsigned long long)f1, (unsigned long long)f2);
    }
    /* rounding mode switch inside one block: RN=1 (toward zero), RN=3 */
    set_fpscr(0);
    {
        double a, b, c, d;
        u64 fs;
        double t;
        asm volatile("mtfsfi 7,1\n\t"
                     "fdiv %0,%5,%6\n\t"        /* 1/3 toward zero */
                     "mtfsfi 7,3\n\t"
                     "fsub %1,%5,%5\n\t"        /* -0 toward -inf */
                     "fdiv %2,%7,%6\n\t"        /* -2/3 toward -inf */
                     "mtfsfi 7,0\n\t"
                     "fdiv %3,%5,%6\n\t"        /* nearest */
                     "mffs %4"
                     : "=&d"(a), "=&d"(b), "=&d"(c), "=&d"(d), "=&d"(t)
                     : "d"(vd[0]), "d"(vd[1]), "d"(vd[6]));
        fs = u_of(t) & 0xffffffff;
        printf("seq6 %016llx %016llx %016llx %016llx fpscr %08llx\n",
               (unsigned long long)u_of(a), (unsigned long long)u_of(b),
               (unsigned long long)u_of(c), (unsigned long long)u_of(d),
               (unsigned long long)fs);
    }
    /* fesetround across calls */
    set_fpscr(0);
    {
        int modes[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD };
        for (int m = 0; m < 4; m++) {
            volatile double s = 0;
            fesetround(modes[m]);
            for (int i = 1; i < 200; i++) {
                s += vd[5] / i;
                s = fma(s, vd[5], -vd[4] * i);
            }
            fesetround(FE_TONEAREST);
            printf("seq7 mode %d %016llx fpscr %08llx\n", m,
                   (unsigned long long)u_of(s),
                   (unsigned long long)get_fpscr());
        }
    }
    /* feclearexcept / fetestexcept */
    {
        volatile double r;
        feclearexcept(FE_ALL_EXCEPT);
        r = vd[0] / vd[1];
        int e1 = fetestexcept(FE_ALL_EXCEPT);
        feclearexcept(FE_INEXACT);
        r = vd[0] + vd[0];
        int e2 = fetestexcept(FE_ALL_EXCEPT);
        r = vd[2] / vd[2];
        int e3 = fetestexcept(FE_ALL_EXCEPT);
        r = sqrt(-vd[0]);
        int e4 = fetestexcept(FE_INVALID);
        feclearexcept(FE_ALL_EXCEPT);
        r = (double)(int)vd[3];
        int e5 = fetestexcept(FE_ALL_EXCEPT);
        (void)r;
        printf("seq8 %x %x %x %x %x\n", e1, e2, e3, e4, e5);
    }
}

/* ---- enabled exceptions: SIGFPE ---- */
static sigjmp_buf jb;
static volatile int fpe_code;

static void on_fpe(int sig, siginfo_t *si, void *uc)
{
    fpe_code = si->si_code;
    siglongjmp(jb, 1);
}

static void trap_tests(void)
{
    struct sigaction sa = { 0 };
    int ex[4] = { FE_DIVBYZERO, FE_INVALID, FE_OVERFLOW, FE_INEXACT };
    sa.sa_sigaction = on_fpe;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGFPE, &sa, NULL);
    for (int i = 0; i < 4; i++) {
        volatile double r = 0;
        feclearexcept(FE_ALL_EXCEPT);
        fpe_code = -1;
        if (sigsetjmp(jb, 1) == 0) {
            feenableexcept(ex[i]);
            switch (i) {
            case 0: r = vd[0] / vd[2]; break;
            case 1: r = vd[2] / vd[2]; break;
            case 2: r = vd[3] * vd[3]; break;
            case 3: r = vd[0] / vd[1]; break;
            }
            printf("trap%d none r=%016llx\n", i, (unsigned long long)u_of(r));
        } else {
            printf("trap%d SIGFPE code %d\n", i, fpe_code);
        }
        fedisableexcept(FE_ALL_EXCEPT);
        printf("trap%d after fpscr %08llx\n", i,
               (unsigned long long)get_fpscr() & ~0x60000ull);
    }
    /* pending flags from generated code, then enabling that exception */
    set_fpscr(0);
    fpe_code = -1;
    if (sigsetjmp(jb, 1) == 0) {
        volatile double r = vd[0] / vd[2];
        (void)r;
        feenableexcept(FE_DIVBYZERO);
        printf("trap4 none\n");
    } else {
        printf("trap4 SIGFPE code %d\n", fpe_code);
    }
    fedisableexcept(FE_ALL_EXCEPT);
    set_fpscr(0);
}

/* ---- signals: FPSCR is saved and restored around a handler ---- */
#include <ucontext.h>
static volatile u64 segv_fpscr;
static void on_segv(int sig, siginfo_t *si, void *uc)
{
    ucontext_t *u = uc;
    segv_fpscr = u_of(u->uc_mcontext.fp_regs[32]);
    siglongjmp(jb, 1);
}

static volatile u64 in_handler;
static void on_usr1(int sig)
{
    volatile double r = vd[0] / vd[2];   /* ZX inside the handler */
    (void)r;
    in_handler = get_fpscr();
}

static void signal_tests(void)
{
    volatile double r;
    signal(SIGUSR1, on_usr1);
    set_fpscr(0);
    r = vd[0] / vd[1];                   /* XX before */
    raise(SIGUSR1);
    printf("sig1 handler %08llx after %08llx\n",
           (unsigned long long)in_handler & ~0x60000ull,
           (unsigned long long)get_fpscr() & ~0x60000ull);
    (void)r;
    /* SIGSEGV from a guest access in the middle of FP code */
    set_fpscr(0);
    {
        struct sigaction sa = { 0 };
        sa.sa_sigaction = on_segv;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL);
        if (sigsetjmp(jb, 1) == 0) {
            double a;
            asm volatile("fdiv %0,%1,%2\n\tlfd %0,0(%3)"
                         : "=&d"(a) : "d"(vd[0]), "d"(vd[1]), "b"(16)
                         : "memory");
            printf("sig2 no fault\n");
        } else {
            printf("sig2 SIGSEGV fpscr at fault %08llx now %08llx\n",
                   (unsigned long long)segv_fpscr & 0xfff9ffffull,
                   (unsigned long long)get_fpscr() & ~0x60000ull);
        }
    }
}

/* ---- threads: flags are per thread ---- */
static void *thr(void *p)
{
    volatile double r;
    set_fpscr(0);
    r = vd[0] / vd[2];
    (void)r;
    *(u64 *)p = get_fpscr() & ~0x60000ull;
    return NULL;
}

static void thread_tests(void)
{
    pthread_t t;
    u64 child;
    volatile double r;
    set_fpscr(0);
    r = vd[0] / vd[1];
    pthread_create(&t, NULL, thr, &child);
    pthread_join(t, NULL);
    printf("thr parent %08llx child %08llx\n",
           (unsigned long long)get_fpscr() & ~0x60000ull,
           (unsigned long long)child);
    (void)r;
    set_fpscr(0);
    r = vd[0] / vd[2];
    pid_t pid = fork();
    if (pid == 0) {
        printf("fork child %08llx\n",
               (unsigned long long)get_fpscr() & ~0x60000ull);
        fflush(stdout);
        _exit(0);
    }
    waitpid(pid, NULL, 0);
    printf("fork parent %08llx\n",
           (unsigned long long)get_fpscr() & ~0x60000ull);
}

int main(int argc, char **argv)
{
    int nrand = argc > 1 ? atoi(argv[1]) : 40;
    static u64 small[40];
    static u64 ints[80];
    int ni = 0, ns = 0;

    setvbuf(stdout, NULL, _IOFBF, 1 << 20);
    build_vals(nrand);

    for (int i = 0; i < nvals && ns < 40; i += 3) {
        small[ns++] = vals[i];
    }
    {
        static const u64 iv[] = {
            0, 1, 2, 3, (u64)-1, (u64)-2, 0x7fffffff, 0x80000000,
            0xffffffff, 0x100000001ull, (1ull << 24) + 1, (1ull << 53) + 1,
            (1ull << 53) - 1, 0x7fffffffffffffffull, 0x8000000000000000ull,
            0xffffffffffffffffull, 0x8000000000000001ull, 0x0020000020000001ull,
            0xfffffffff0000001ull, 123456789012345ull,
        };
        for (size_t i = 0; i < sizeof(iv) / sizeof(iv[0]); i++) {
            ints[ni++] = iv[i];
        }
        while (ni < 80) {
            u64 r = rnd();
            ints[ni++] = r >> (rnd() % 64);
        }
    }

    run2("fadd", t_fadd, vals, nvals);
    run2("fadds", t_fadds, vals, nvals);
    run2("fsub", t_fsub, vals, nvals);
    run2("fsubs", t_fsubs, vals, nvals);
    run2("fmul", t_fmul, vals, nvals);
    run2("fmuls", t_fmuls, vals, nvals);
    run2("fdiv", t_fdiv, vals, nvals);
    run2("fdivs", t_fdivs, vals, nvals);
    run2("fcmpu", t_fcmpu, vals, nvals);
    run2("fcmpo", t_fcmpo, vals, nvals);
    run3("fmadd", t_fmadd, small, ns);
    run3("fmadds", t_fmadds, small, ns);
    run3("fmsub", t_fmsub, small, ns);
    run3("fmsubs", t_fmsubs, small, ns);
    run3("fnmadd", t_fnmadd, small, ns);
    run3("fnmadds", t_fnmadds, small, ns);
    run3("fnmsub", t_fnmsub, small, ns);
    run3("fnmsubs", t_fnmsubs, small, ns);
    run3("fsel", t_fsel, small, ns);
    run1("fsqrt", t_fsqrt, vals, nvals);
    run1("fsqrts", t_fsqrts, vals, nvals);
    run1("frsp", t_frsp, vals, nvals);
    run1("friz", t_friz, vals, nvals);
    run1("frip", t_frip, vals, nvals);
    run1("frim", t_frim, vals, nvals);
    run1("frin", t_frin, vals, nvals);
    run1("fre", t_fre, vals, nvals);
    run1("fres", t_fres, vals, nvals);
    run1("frsqrte", t_frsqrte, vals, nvals);
    run1("frsqrtes", t_frsqrtes, vals, nvals);
    run1("fctiw", t_fctiw, vals, nvals);
    run1("fctiwz", t_fctiwz, vals, nvals);
    run1("fctiwu", t_fctiwu, vals, nvals);
    run1("fctiwuz", t_fctiwuz, vals, nvals);
    run1("fctid", t_fctid, vals, nvals);
    run1("fctidz", t_fctidz, vals, nvals);
    run1("fctidu", t_fctidu, vals, nvals);
    run1("fctiduz", t_fctiduz, vals, nvals);
    run1("fcfid", t_fcfid, ints, ni);
    run1("fcfids", t_fcfids, ints, ni);
    run1("fcfidu", t_fcfidu, ints, ni);
    run1("fcfidus", t_fcfidus, ints, ni);
    run1("fcfid", t_fcfid, vals, nvals);
    run1("fcfids", t_fcfids, vals, nvals);
    run1("fcfidu", t_fcfidu, vals, nvals);
    run1("fcfidus", t_fcfidus, vals, nvals);
    for (int i = 0; i < nvals; i++) {
        for (int j = 0; j < nvals; j += 7) {
            u64 r, s;
            u32 cr;
            t_fadd_rc(vals[i], vals[j], &r, &s, &cr);
            printf("fadd. %016llx %016llx -> %016llx %08llx cr1 %x\n",
                   (unsigned long long)vals[i], (unsigned long long)vals[j],
                   (unsigned long long)r, (unsigned long long)(s & 0xffffffff),
                   cr);
            t_fdiv_rc(vals[i], vals[j], &r, &s, &cr);
            printf("fdiv. %016llx %016llx -> %016llx %08llx cr1 %x\n",
                   (unsigned long long)vals[i], (unsigned long long)vals[j],
                   (unsigned long long)r, (unsigned long long)(s & 0xffffffff),
                   cr);
            t_fmuls_rc(vals[i], vals[j], &r, &s, &cr);
            printf("fmuls. %016llx %016llx -> %016llx %08llx cr1 %x\n",
                   (unsigned long long)vals[i], (unsigned long long)vals[j],
                   (unsigned long long)r, (unsigned long long)(s & 0xffffffff),
                   cr);
        }
    }
    for (int i = 0; i < nvals; i++) {
        u64 r;
        u32 w;
        t_lfs((u32)(vals[i] >> 32), &r);
        printf("lfs %08x -> %016llx\n", (u32)(vals[i] >> 32),
               (unsigned long long)r);
        t_lfs((u32)vals[i], &r);
        printf("lfs %08x -> %016llx\n", (u32)vals[i], (unsigned long long)r);
        t_stfs(vals[i], &w);
        printf("stfs %016llx -> %08x\n", (unsigned long long)vals[i], w);
    }
    seq_tests();
    trap_tests();
    signal_tests();
    thread_tests();
    return 0;
}
