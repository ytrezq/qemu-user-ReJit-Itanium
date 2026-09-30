/* PAuth differential test: run with a fixed QEMU seed, JIT on and off. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#include <setjmp.h>
#include <string.h>
#include <sys/prctl.h>
#ifndef PR_PAC_RESET_KEYS
#define PR_PAC_RESET_KEYS 54
#endif

typedef uint64_t u64;
static u64 rs = 0x9e3779b97f4a7c15ull;
static u64 rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

static sigjmp_buf jb;
static volatile int nsig;
static void onill(int sig, siginfo_t *si, void *uc)
{
    nsig++;
    siglongjmp(jb, 1);
}

#define PAC2(name, insn)                                             \
static u64 name(u64 p, u64 m)                                       \
{ asm volatile(insn " %0, %1" : "+r"(p) : "r"(m)); return p; }
PAC2(pacia, "pacia") PAC2(pacib, "pacib") PAC2(pacda, "pacda") PAC2(pacdb, "pacdb")
PAC2(autia, "autia") PAC2(autib, "autib") PAC2(autda, "autda") PAC2(autdb, "autdb")
#define PAC1(name, insn)                                             \
static u64 name(u64 p) { asm volatile(insn " %0" : "+r"(p)); return p; }
PAC1(paciza, "paciza") PAC1(autiza, "autiza") PAC1(pacdzb, "pacdzb") PAC1(autdzb, "autdzb")
PAC1(xpaci, "xpaci") PAC1(xpacd, "xpacd")

static u64 hint_sp(u64 lr, u64 sp, int aut)
{
    register u64 x30 asm("x30") = lr;
    register u64 x16 asm("x16") = sp;
    register u64 x17 asm("x17") = lr ^ 0x1234;
    if (aut) {
        asm volatile("autia1716" : "+r"(x17) : "r"(x16));
    } else {
        asm volatile("pacia1716" : "+r"(x17) : "r"(x16));
    }
    return x17 ^ (x30 & 0);
}

static u64 ptr(void)
{
    u64 r = rnd();
    switch (rnd() % 6) {
    case 0: return r & 0x0000ffffffffffffull;                 /* canonical user */
    case 1: return r & 0x00ffffffffffffffull;                 /* dirty [48,56) */
    case 2: return r;                                         /* anything */
    case 3: return (r & 0x0000ffffffffffffull) | (rnd() << 56); /* tagged */
    case 4: return r | 0x0080000000000000ull;                 /* bit 55 */
    default: return r & 0x000fffffffffffffull;                /* 52-bit */
    }
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 20000;
    struct sigaction sa = { .sa_sigaction = onill, .sa_flags = SA_SIGINFO | SA_NODEFER };
    sigaction(SIGILL, &sa, NULL);
    u64 h = 0;

    for (int i = 0; i < n; i++) {
        u64 p = ptr(), m = rnd() % 3 == 0 ? 0 : rnd(), r[24];
        int k = 0;
        r[k++] = pacia(p, m); r[k++] = pacib(p, m); r[k++] = pacda(p, m); r[k++] = pacdb(p, m);
        r[k++] = paciza(p); r[k++] = pacdzb(p); r[k++] = xpaci(p); r[k++] = xpacd(p);
        r[k++] = hint_sp(p, m, 0);
        /* good authentications */
        r[k++] = autia(pacia(p, m), m); r[k++] = autib(pacib(p, m), m);
        r[k++] = autda(pacda(p, m), m); r[k++] = autdb(pacdb(p, m), m);
        r[k++] = autiza(paciza(p)); r[k++] = autdzb(pacdzb(p));
        /* bad ones: FPAC */
        volatile u64 bad = 0;
        nsig = 0;
        if (!sigsetjmp(jb, 1)) { bad = autia(pacia(p, m) ^ (1ull << (48 + rnd() % 7)), m); }
        r[k++] = bad ^ nsig;
        nsig = 0; bad = 0;
        if (!sigsetjmp(jb, 1)) { bad = autdb(p, m + 1); }
        r[k++] = bad ^ (nsig << 1);
        nsig = 0; bad = 0;
        if (!sigsetjmp(jb, 1)) { bad = autia(p, m); }
        r[k++] = bad ^ (nsig << 2);
        for (int j = 0; j < k; j++) {
            if (i < 40) {
                printf("%d.%d %016llx %016llx -> %016llx\n", i, j, (unsigned long long)p,
                       (unsigned long long)m, (unsigned long long)r[j]);
            }
            h = (h ^ r[j]) * 0x100000001b3ull;
        }
    }
    for (int i = 0; i < 1000; i++) {
        u64 p = rnd() & 0x0000ffffffffffffull, m = rnd();
        u64 x = pacia(p, m), y = pacib(p, m);
        if (i % 100 == 0) {
            prctl(PR_PAC_RESET_KEYS, 0, 0, 0, 0);
        }
        volatile u64 r = 0;
        nsig = 0;
        if (!sigsetjmp(jb, 1)) { r = autia(x, m); }
        h = (h ^ r ^ nsig) * 0x100000001b3ull;
        nsig = 0;
        if (!sigsetjmp(jb, 1)) { r = autib(y, m); }
        h = (h ^ r ^ (nsig << 1)) * 0x100000001b3ull;
        h = (h ^ pacia(p, m)) * 0x100000001b3ull;
    }
    printf("hash %016llx\n", (unsigned long long)h);
    return 0;
}
