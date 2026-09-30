/* fflags accumulation across instructions, helpers, syscalls, signals,
   threads, fcsr writes and frm changes */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <fenv.h>
#include <math.h>

static unsigned rdfl(void) { unsigned f; asm volatile("frflags %0" : "=r"(f)); return f; }
static void clrfl(void) { asm volatile("fsflags zero"); }
static double dadd(double a, double b) { double r; asm volatile("fadd.d %0, %1, %2" : "=f"(r) : "f"(a), "f"(b)); return r; }
static double ddiv(double a, double b) { double r; asm volatile("fdiv.d %0, %1, %2" : "=f"(r) : "f"(a), "f"(b)); return r; }
static double ddiv_rdn(double a, double b) { double r; asm volatile("fdiv.d %0, %1, %2, rdn" : "=f"(r) : "f"(a), "f"(b)); return r; }
static double dmul(double a, double b) { double r; asm volatile("fmul.d %0, %1, %2" : "=f"(r) : "f"(a), "f"(b)); return r; }
static double dsqrt(double a) { double r; asm volatile("fsqrt.d %0, %1" : "=f"(r) : "f"(a)); return r; }

static volatile unsigned in_handler, handler_saw;
static void handler(int sig)
{
    handler_saw = rdfl();
    /* raise divide by zero in the handler: must not survive the return */
    volatile double z = ddiv(1.0, 0.0);
    (void)z;
    in_handler = rdfl();
}

static void *thr(void *arg)
{
    unsigned *out = arg;
    out[0] = rdfl();               /* inherited from the parent at clone */
    clrfl();
    volatile double x = ddiv(1.0, 3.0);   /* NX */
    (void)x;
    out[1] = rdfl();
    return NULL;
}

int main(void)
{
    volatile double one = 1.0, three = 3.0, zero = 0.0, big = 1e308, tiny = 1e-308, m1 = -1.0;
    volatile double r;
    unsigned f;

    clrfl();
    r = dadd(one, three);                  /* exact */
    printf("exact add: %x\n", rdfl());
    r = ddiv(one, three);                  /* NX */
    r = dmul(big, big);                    /* OF NX */
    printf("accum: %x\n", rdfl());
    r = dsqrt(m1);                         /* NV */
    r = dmul(tiny, tiny);                  /* UF NX */
    r = ddiv(one, zero);                   /* DZ */
    printf("accum2: %x\n", rdfl());
    clrfl();
    printf("cleared: %x\n", rdfl());

    /* JIT op, then a helper op (rdn), then fflags */
    r = ddiv(one, three);
    clrfl();
    r = ddiv_rdn(one, zero);               /* DZ via the helper */
    r = dmul(big, big);                    /* OF NX via host */
    printf("mixed: %x\n", rdfl());

    /* across a syscall */
    clrfl();
    r = ddiv(one, three);
    if (write(1, "", 0) < 0) {
        return 1;
    }
    getpid();
    printf("after syscall: %x\n", rdfl());

    /* fscsr / fsflags with a value */
    asm volatile("fsflags %0" : : "r"(0x10));      /* NV only */
    r = dadd(one, three);
    printf("fsflags nv: %x\n", rdfl());
    asm volatile("fscsr %0" : : "r"(0));
    printf("fscsr 0: %x\n", rdfl());

    /* signal handler */
    clrfl();
    r = ddiv(one, three);                  /* NX */
    signal(SIGUSR1, handler);
    raise(SIGUSR1);
    f = rdfl();
    printf("signal: before %x in handler %x after %x\n", handler_saw, in_handler, f);

    /* threads */
    {
        pthread_t t;
        unsigned res[2];
        clrfl();
        r = dmul(big, big);                /* OF NX in the parent */
        pthread_create(&t, NULL, thr, res);
        pthread_join(t, NULL);
        printf("thread: inherited %x own %x parent %x\n", res[0], res[1], rdfl());
    }

    /* frm changes: dynamic rounding */
    {
        volatile double x = 1.0, y = 3.0;
        double q[4];
        for (int m = 0; m < 4; m++) {
            static const int modes[4] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
            fesetround(modes[m]);
            q[m] = ddiv(x, y);
        }
        fesetround(FE_TONEAREST);
        printf("rounding: %a %a %a %a\n", q[0], q[1], q[2], q[3]);
        /* the same TB with different frm, in a loop */
        double acc = 0;
        for (int i = 0; i < 1000; i++) {
            fesetround(i & 1 ? FE_UPWARD : FE_TONEAREST);
            acc += ddiv(x, y + i);
        }
        fesetround(FE_TONEAREST);
        printf("loop: %a\n", acc);
    }

    /* libm */
    clrfl();
    printf("libm: %.17g %.17g %.17g %.17g %x\n", sin(0.5), exp(1.1), log(3.3), pow(2.2, 3.3), rdfl());
    feclearexcept(FE_ALL_EXCEPT);
    r = sqrt(-1.0 * one);
    printf("fetestexcept: %x\n", fetestexcept(FE_ALL_EXCEPT));
    return 0;
}
