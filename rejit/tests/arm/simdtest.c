/* Differential test of A32 parallel add/sub ([SU]ADD8/16, [SU]SUB8/16) with their GE flags, SEL, APSR writes. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
typedef uint32_t u32;
static uint64_t rs = 88172645463325252ull;
static u32 rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (u32)(rs >> 11); }
static u32 val(void)
{
    static const u32 sp[] = { 0, 0xffffffff, 0x80808080, 0x7f7f7f7f, 0x80008000, 0x7fff7fff, 0x01010101, 0xfefefefe };
    return rnd() % 4 == 0 ? sp[rnd() % 8] : rnd();
}
#define OP(name) \
static u32 t_##name(u32 a, u32 b, u32 *ge) { u32 r, f; \
  asm volatile(#name " %0, %2, %3\n\tmrs %1, apsr" : "=&r"(r), "=r"(f) : "r"(a), "r"(b)); \
  *ge = (f >> 16) & 0xf; return r; }
OP(uadd8) OP(usub8) OP(sadd8) OP(ssub8) OP(uadd16) OP(usub16) OP(sadd16) OP(ssub16)
static u32 t_sel(u32 a, u32 b, u32 g)
{
    u32 r;
    asm volatile("msr APSR_g, %1\n\tsel %0, %2, %3" : "=&r"(r) : "r"(g << 16), "r"(a), "r"(b));
    return r;
}
int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 200000;
    uint64_t h = 0;
    for (int i = 0; i < n; i++) {
        u32 a = val(), b = val(), g, r;
        u32 (*f[8])(u32, u32, u32 *) = { t_uadd8, t_usub8, t_sadd8, t_ssub8, t_uadd16, t_usub16, t_sadd16, t_ssub16 };
        for (int k = 0; k < 8; k++) {
            r = f[k](a, b, &g);
            h = (h ^ r ^ ((uint64_t)g << 32)) * 0x100000001b3ull;
            if (i < 3) printf("%d %08x %08x -> %08x ge=%x\n", k, a, b, r, g);
        }
        r = t_sel(a, b, rnd() & 15);
        h = (h ^ r) * 0x100000001b3ull;
    }
    printf("hash %016llx\n", (unsigned long long)h);
    return 0;
}
