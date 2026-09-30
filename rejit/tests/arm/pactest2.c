/*
 * PAuth cache test: the same pointers and modifiers again and again, so
 * that lookups hit, with good and bad codes, empty-entry look-alikes
 * (0/0, 0/16), combined forms (BRAA, RETAA, LDRAA).  Differential: run
 * with a fixed QEMU seed (QEMU_RAND_SEED) and compare with the reference.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/prctl.h>
#ifndef PR_PAC_RESET_KEYS
#define PR_PAC_RESET_KEYS 54
#endif

typedef uint64_t u64;
static sigjmp_buf jb;
static volatile int nsig;
static void onill(int sig, siginfo_t *si, void *uc) { nsig++; siglongjmp(jb, 1); }

#define PAC2(name, insn) \
static u64 name(u64 p, u64 m) { asm volatile(insn " %0, %1" : "+r"(p) : "r"(m)); return p; }
PAC2(pacia, "pacia") PAC2(pacib, "pacib") PAC2(pacda, "pacda") PAC2(pacdb, "pacdb")
PAC2(autia, "autia") PAC2(autib, "autib") PAC2(autda, "autda") PAC2(autdb, "autdb")

/* BRAA to a label signed with the modifier: returns 1 if the branch was taken */
static u64 braa_test(u64 m, u64 flip)
{
    u64 r;
    asm volatile("adr x9, 1f\n\t"
                 "pacia x9, %1\n\t"
                 "eor x9, x9, %2\n\t"
                 "mov %0, #0\n\t"
                 "braa x9, %1\n\t"
                 "mov %0, #2\n\t"
                 "b 2f\n"
                 "1: mov %0, #1\n"
                 "2:"
                 : "=&r"(r) : "r"(m), "r"(flip) : "x9");
    return r;
}

/* LDRAA of a DA-signed pointer to v */
static u64 ldraa_test(u64 *v, u64 flip)
{
    u64 p = (u64)v, r;
    asm volatile("pacdza %1\n\t"
                 "eor %1, %1, %2\n\t"
                 "ldraa %0, [%1]"
                 : "=&r"(r), "+r"(p) : "r"(flip) : "memory");
    return r;
}

static __attribute__((noinline, target("branch-protection=pac-ret"))) u64 leaf2(u64 x)
{
    return x * 7 + 1;
}

static __attribute__((noinline, target("branch-protection=pac-ret"))) u64 callee(u64 x)
{
    return leaf2(x) ^ leaf2(x + 1);
}

#define TRY(expr, shift) do { \
        volatile u64 _v = 0; nsig = 0; \
        if (!sigsetjmp(jb, 1)) { _v = (expr); } \
        h = (h ^ _v ^ ((u64)nsig << (shift))) * 0x100000001b3ull; \
        if (pr) printf("  %-40s %016llx sig %d\n", #expr, (unsigned long long)_v, nsig); \
    } while (0)

int main(int argc, char **argv)
{
    struct sigaction sa = { .sa_sigaction = onill, .sa_flags = SA_SIGINFO | SA_NODEFER };
    static u64 var = 0x1122334455667788ull;
    u64 ps[8], ms[5] = { 0, 16, 0x7ffc0000fff0ull, 0x7ffc0000fff0ull + 16, 0xdeadbeefcafef00dull };
    u64 h = 0;

    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    ps[0] = 0; ps[1] = 16; ps[2] = 4; ps[3] = (u64)&main; ps[4] = (u64)&var;
    ps[5] = 0x0000fffffffffffcull; ps[6] = (u64)&main + 0x800; ps[7] = 0x12345678;
    for (int round = 0; round < 60; round++) {
        int pr = round < 2;
        if (round == 30) {
            prctl(PR_PAC_RESET_KEYS, 0, 0, 0, 0);
        }
        for (int i = 0; i < 8; i++) {
            for (int j = 0; j < 5; j++) {
                u64 p = ps[i], m = ms[j];
                u64 sa_ = pacia(p, m), sb = pacib(p, m), sd = pacda(p, m);
                if (pr) printf("p %016llx m %016llx\n", (unsigned long long)p, (unsigned long long)m);
                TRY(pacia(p, m), 0);
                TRY(pacdb(p, m), 0);
                TRY(autia(sa_, m), 1);
                TRY(autib(sb, m), 1);
                TRY(autda(sd, m), 1);
                TRY(autia(sa_ ^ (1ull << 50), m), 2);
                TRY(autia(sa_ | (1ull << 55), m), 3);
                TRY(autia(sa_, m + 16), 4);
                TRY(autia(sb, m), 5);
                TRY(autda(sa_, m), 6);
                TRY(autia(p, m), 7);
                TRY(pacia(p | (1ull << 52), m), 8);
                TRY(pacia(p | (0x5aull << 56), m), 9);
                TRY(autia(pacia(p | (0x5aull << 56), m), m), 10);
                TRY(pacia(p | (1ull << 55), m), 11);
                TRY(autia(pacia(p | (1ull << 55), m), m), 12);
            }
        }
        /* empty-entry look-alikes */
        TRY(autia(0, 0), 13);
        TRY(autia(0, 16), 14);
        TRY(autdb(0, 0), 15);
        TRY(autdb(0, 16), 16);
        TRY(braa_test(round, 0), 17);
        TRY(braa_test(round, 1ull << 49), 18);
        TRY(ldraa_test(&var, 0), 19);
        TRY(ldraa_test(&var, 1ull << 51), 20);
        TRY(callee(round), 21);
    }
    printf("hash %016llx\n", (unsigned long long)h);
    return 0;
}
