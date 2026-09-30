#include <stdio.h>
#include <signal.h>
#include <setjmp.h>
#include <string.h>
#include <sys/mman.h>
typedef __vector unsigned long long v2u;
static sigjmp_buf jb; static volatile void *fa; static volatile int sig;
static void h(int s, siginfo_t *si, void *u) { sig = s; fa = si->si_addr; siglongjmp(jb, 1); }
int main(void) {
    struct sigaction sa = {0}; sa.sa_sigaction = h; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, 0);
    long pg = 4096;
    unsigned char *m = mmap(0, 3*pg, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    munmap(m + 2*pg, pg); mprotect(m + pg, pg, PROT_READ);
    for (int nb = 1; nb <= 16; nb += 5) for (int back = 1; back <= 16; back += 7) {
        unsigned char *p = m + 2*pg - back; unsigned long rb = (unsigned long)nb << 56; v2u r = {1,2};
        sig = 0; fa = 0;
        if (!sigsetjmp(jb, 1)) asm volatile("lxvl %x0,%1,%2" : "=wa"(r) : "b"(p), "r"(rb) : "memory");
        printf("lxvl nb=%d back=%d sig=%d addr=%+ld r=%016llx%016llx\n", nb, back, sig, fa ? (long)((unsigned char*)fa - (m + 2*pg)) : 999, r[1], r[0]);
        sig = 0; fa = 0;
        p = m + pg + 100; r = (v2u){0x1111, 0x2222};
        if (!sigsetjmp(jb, 1)) asm volatile("stxvl %x0,%1,%2" : : "wa"(r), "b"(p), "r"(rb) : "memory");
        printf("stxvl nb=%d sig=%d addr=%+ld\n", nb, sig, fa ? (long)((unsigned char*)fa - (m + pg)) : 999);
    }
    return 0;
}
