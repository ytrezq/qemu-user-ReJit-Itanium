/*
 * Differential test of Altivec/VSX permute-type instructions that the JIT
 * compiles inline: vshasigmaw/d, vsldoi, vperm(r), xxperm(r), lxvl(l),
 * stxvl(l), vbpermq.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

typedef uint64_t u64;
typedef __vector unsigned long long v2u;

static u64 rs = 0x9e3779b97f4a7c15ull;
static u64 rnd(void)
{
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return rs;
}
static v2u rv(void)
{
    v2u v = { rnd(), rnd() };
    return v;
}
static void pv(v2u v)
{
    printf(" %016llx:%016llx", (unsigned long long)v[1],
           (unsigned long long)v[0]);
}

#define SHA(name, st, six)                                              \
static v2u name##_##st##_##six(v2u a)                                   \
{                                                                       \
    v2u r;                                                              \
    asm(#name " %0,%1," #st "," #six : "=v"(r) : "v"(a));               \
    return r;                                                           \
}
#define SHA_ALL(name, st) \
    SHA(name, st, 0) SHA(name, st, 1) SHA(name, st, 2) SHA(name, st, 5) \
    SHA(name, st, 10) SHA(name, st, 15)
SHA_ALL(vshasigmaw, 0) SHA_ALL(vshasigmaw, 1)
SHA_ALL(vshasigmad, 0) SHA_ALL(vshasigmad, 1)

#define SLDOI(n)                                                        \
static v2u sldoi_##n(v2u a, v2u b)                                      \
{                                                                       \
    v2u r;                                                              \
    asm("vsldoi %0,%1,%2," #n : "=v"(r) : "v"(a), "v"(b));              \
    return r;                                                           \
}
SLDOI(0) SLDOI(1) SLDOI(2) SLDOI(3) SLDOI(4) SLDOI(5) SLDOI(6) SLDOI(7)
SLDOI(8) SLDOI(9) SLDOI(10) SLDOI(11) SLDOI(12) SLDOI(13) SLDOI(14) SLDOI(15)

static v2u (*const sldoi[16])(v2u, v2u) = {
    sldoi_0, sldoi_1, sldoi_2, sldoi_3, sldoi_4, sldoi_5, sldoi_6, sldoi_7,
    sldoi_8, sldoi_9, sldoi_10, sldoi_11, sldoi_12, sldoi_13, sldoi_14,
    sldoi_15,
};

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 2000;
    setvbuf(stdout, NULL, _IOFBF, 1 << 20);
    for (int i = 0; i < n; i++) {
        v2u a = rv(), b = rv(), c = rv(), t = rv(), r;
#define P1(name, fn) do { r = fn(a); printf(name); pv(a); printf(" ->"); \
                          pv(r); printf("\n"); } while (0)
        P1("shaw00", vshasigmaw_0_0); P1("shaw01", vshasigmaw_0_1);
        P1("shaw02", vshasigmaw_0_2); P1("shaw05", vshasigmaw_0_5);
        P1("shaw0a", vshasigmaw_0_10); P1("shaw0f", vshasigmaw_0_15);
        P1("shaw10", vshasigmaw_1_0); P1("shaw11", vshasigmaw_1_1);
        P1("shaw12", vshasigmaw_1_2); P1("shaw15", vshasigmaw_1_5);
        P1("shaw1a", vshasigmaw_1_10); P1("shaw1f", vshasigmaw_1_15);
        P1("shad00", vshasigmad_0_0); P1("shad01", vshasigmad_0_1);
        P1("shad02", vshasigmad_0_2); P1("shad05", vshasigmad_0_5);
        P1("shad0a", vshasigmad_0_10); P1("shad0f", vshasigmad_0_15);
        P1("shad10", vshasigmad_1_0); P1("shad11", vshasigmad_1_1);
        P1("shad12", vshasigmad_1_2); P1("shad15", vshasigmad_1_5);
        P1("shad1a", vshasigmad_1_10); P1("shad1f", vshasigmad_1_15);
        for (int k = 0; k < 16; k++) {
            r = sldoi[k](a, b);
            printf("vsldoi%d", k); pv(a); pv(b); printf(" ->"); pv(r);
            printf("\n");
        }
        asm("vperm %0,%1,%2,%3" : "=v"(r) : "v"(a), "v"(b), "v"(c));
        printf("vperm"); pv(a); pv(b); pv(c); printf(" ->"); pv(r);
        printf("\n");
        asm("vpermr %0,%1,%2,%3" : "=v"(r) : "v"(a), "v"(b), "v"(c));
        printf("vpermr"); pv(a); pv(b); pv(c); printf(" ->"); pv(r);
        printf("\n");
        r = t;
        asm("xxperm %x0,%x1,%x2" : "+wa"(r) : "wa"(a), "wa"(c));
        printf("xxperm"); pv(a); pv(t); pv(c); printf(" ->"); pv(r);
        printf("\n");
        r = t;
        asm("xxpermr %x0,%x1,%x2" : "+wa"(r) : "wa"(a), "wa"(c));
        printf("xxpermr"); pv(a); pv(t); pv(c); printf(" ->"); pv(r);
        printf("\n");
        asm("vbpermq %0,%1,%2" : "=v"(r) : "v"(a), "v"(c));
        printf("vbpermq"); pv(a); pv(c); printf(" ->"); pv(r); printf("\n");
    }

    /* lxvl, lxvll, stxvl, stxvll: lengths 0..20, near unmapped pages */
    {
        long pg = 4096;
        unsigned char *m = mmap(NULL, 3 * pg, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        munmap(m + 2 * pg, pg);
        for (int i = 0; i < 2 * pg; i++) {
            m[i] = (unsigned char)(i * 7 + 3);
        }
        for (int off = 0; off < 40; off++) {
            unsigned char *p = m + 2 * pg - 24 + off;
            for (unsigned long nb = 0; nb <= 20; nb++) {
                unsigned long rb = nb << 56 | 0x55;
                v2u r1, r2, s = rv();
                if (p + (nb > 16 ? 16 : nb) > m + 2 * pg) {
                    continue;
                }
                asm volatile("lxvl %x0,%1,%2" : "=wa"(r1) : "b"(p), "r"(rb)
                             : "memory");
                asm volatile("lxvll %x0,%1,%2" : "=wa"(r2) : "b"(p), "r"(rb)
                             : "memory");
                printf("lxvl %d %lu ->", off, nb); pv(r1); pv(r2);
                printf("\n");
                unsigned char buf[48];
                memset(buf, 0xee, sizeof(buf));
                asm volatile("stxvl %x0,%1,%2" : : "wa"(s), "b"(buf + 8),
                             "r"(rb) : "memory");
                printf("stxvl %lu", nb); pv(s); printf(" ->");
                for (int k = 0; k < 32; k++) {
                    printf("%02x", buf[k]);
                }
                memset(buf, 0xee, sizeof(buf));
                asm volatile("stxvll %x0,%1,%2" : : "wa"(s), "b"(buf + 8),
                             "r"(rb) : "memory");
                printf(" ");
                for (int k = 0; k < 32; k++) {
                    printf("%02x", buf[k]);
                }
                printf("\n");
            }
        }
    }
    return 0;
}
