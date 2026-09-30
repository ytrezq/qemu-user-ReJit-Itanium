/* FEAT_MOPS differential test: CPY*, CPYF*, SET*, overlaps, page crossings, faults */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <ucontext.h>
#include <sys/mman.h>

typedef uint64_t u64;
static u64 h = 1469598103934665603ull;
static void mix(const void *p, size_t n)
{
    const unsigned char *c = p;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ c[i]) * 1099511628211ull;
    }
}

static void cpy(void *d, const void *s, size_t n)
{
    asm volatile("cpyp [%0]!, [%1]!, %2!\n\tcpym [%0]!, [%1]!, %2!\n\tcpye [%0]!, [%1]!, %2!"
                 : "+r"(d), "+r"(s), "+r"(n) : : "memory", "cc");
}
static void cpyf(void *d, const void *s, size_t n)
{
    asm volatile("cpyfp [%0]!, [%1]!, %2!\n\tcpyfm [%0]!, [%1]!, %2!\n\tcpyfe [%0]!, [%1]!, %2!"
                 : "+r"(d), "+r"(s), "+r"(n) : : "memory", "cc");
}
static void set(void *d, int v, size_t n)
{
    asm volatile("setp [%0]!, %1!, %2\n\tsetm [%0]!, %1!, %2\n\tsete [%0]!, %1!, %2"
                 : "+r"(d), "+r"(n) : "r"((u64)v) : "memory", "cc");
}

static sigjmp_buf jb;
static volatile u64 fault_addr, fault_regs[3];
static void onsegv(int sig, siginfo_t *si, void *uc_)
{
    ucontext_t *uc = uc_;
    fault_addr = (u64)si->si_addr;
    fault_regs[0] = uc->uc_mcontext.regs[0];
    fault_regs[1] = uc->uc_mcontext.regs[1];
    fault_regs[2] = uc->uc_mcontext.regs[2];
    siglongjmp(jb, 1);
}

/* fixed registers for the fault test: x0 dst, x1 src, x2 size */
static void cpy_fixed(void *d, const void *s, size_t n)
{
    register void *x0 asm("x0") = d;
    register const void *x1 asm("x1") = s;
    register size_t x2 asm("x2") = n;
    asm volatile("cpyfp [x0]!, [x1]!, x2!\n\tcpyfm [x0]!, [x1]!, x2!\n\tcpyfe [x0]!, [x1]!, x2!"
                 : "+r"(x0), "+r"(x1), "+r"(x2) : : "memory", "cc");
}

int main(void)
{
    enum { PG = 4096, N = 6 * PG };
    unsigned char *a = mmap(NULL, N, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char *b = mmap(NULL, N, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    struct sigaction sa = { .sa_sigaction = onsegv, .sa_flags = SA_SIGINFO | SA_NODEFER };
    static const size_t sizes[] = { 0, 1, 2, 3, 7, 8, 15, 16, 17, 31, 32, 63, 64, 100, 255, 256,
                                    1000, 4095, 4096, 4097, 5000, 8191, 8192, 12345, 3 * PG + 17 };
    static const size_t offs[] = { 0, 1, 7, 64, 4000, 4095, 4096 + 13 };
    u64 seed = 12345;

    sigaction(SIGSEGV, &sa, NULL);
    for (size_t i = 0; i < N; i++) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        a[i] = seed >> 56;
    }
    for (int si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        for (int oi = 0; oi < sizeof(offs) / sizeof(offs[0]); oi++) {
            for (int oj = 0; oj < sizeof(offs) / sizeof(offs[0]); oj++) {
                size_t n = sizes[si], o1 = offs[oi], o2 = offs[oj];
                if (o1 + n > N - PG || o2 + n > N - PG) {
                    continue;
                }
                memset(b, 0x5a, N);
                cpyf(b + o1, a + o2, n);
                mix(b, N);
                cpy(b + o2, a + o1, n);
                mix(b, N);
                /* overlapping memmove within b, both directions */
                cpy(b + o1, b + o2, n);
                mix(b, N);
                set(b + o2, (int)(n & 0xff), n);
                mix(b, N);
            }
        }
    }
    printf("copies %016llx\n", (unsigned long long)h);

    /* faults: copy from a to a region whose second page is PROT_NONE */
    unsigned char *c = mmap(NULL, 3 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mprotect(c + PG, PG, PROT_NONE);
    for (int k = 0; k < 6; k++) {
        size_t start = PG - 100 + k * 37, n = 300 + k * 1000;
        fault_addr = 0;
        memset(c, 0, PG);
        if (!sigsetjmp(jb, 1)) {
            cpy_fixed(c + start, a + 3, n);
        }
        printf("fault %d: addr %+lld regs %+lld %+lld %lld\n", k,
               (long long)(fault_addr - (u64)c), (long long)(fault_regs[0] - (u64)c),
               (long long)(fault_regs[1] - (u64)a), (long long)fault_regs[2]);
        mix(c, PG);
        if (!sigsetjmp(jb, 1)) {
            cpy_fixed(c + 50, c + PG + 10 + k, 20 + k * 500);
        }
        printf("rfault %d: addr %+lld regs %+lld %+lld %lld\n", k,
               (long long)(fault_addr - (u64)c), (long long)(fault_regs[0] - (u64)c),
               (long long)(fault_regs[1] - (u64)c), (long long)fault_regs[2]);
    }
    printf("hash %016llx\n", (unsigned long long)h);
    return 0;
}
