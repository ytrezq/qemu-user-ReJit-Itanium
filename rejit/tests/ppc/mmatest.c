/* Differential test of the MMA floating-point GER instructions. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <altivec.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef vector unsigned char vec_t;

static u64 rs = 0x2545f4914f6cdd1dull;
static u64 rnd(void)
{
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return rs;
}

static const u32 fspec[] = {
    0x00000000, 0x80000000, 0x00000001, 0x807fffff, 0x00800000, 0x3f800000,
    0xbf800000, 0x7f7fffff, 0x7f800000, 0xff800000, 0x7fc00000, 0x7fa00001,
    0x3fc00000, 0x40490fdb, 0x1f800000, 0x5f800000,
};
static const u64 dspec[] = {
    0, 0x8000000000000000ull, 1, 0x0010000000000000ull, 0x3ff0000000000000ull,
    0xbff0000000000000ull, 0x7fefffffffffffffull, 0x7ff0000000000000ull,
    0x7ff8000000000000ull, 0x7ff4000000000001ull, 0x400921fb54442d18ull,
    0x1ff0000000000000ull, 0x5ff0000000000000ull,
};

static u32 rf(void)
{
    if (rnd() % 4 == 0) {
        return fspec[rnd() % (sizeof(fspec) / 4)];
    }
    u32 e = 127 - 20 + rnd() % 40;
    return ((u32)rnd() & 0x807fffff) | (e << 23);
}

static u64 rd(void)
{
    if (rnd() % 4 == 0) {
        return dspec[rnd() % (sizeof(dspec) / 8)];
    }
    u64 e = 1023 - 40 + rnd() % 80;
    return (rnd() & 0x800fffffffffffffull) | (e << 52);
}

static vec_t vf(void)
{
    u32 w[4] = { rf(), rf(), rf(), rf() };
    vec_t v;
    memcpy(&v, w, 16);
    return v;
}

static vec_t vd(void)
{
    u64 d[2] = { rd(), rd() };
    vec_t v;
    memcpy(&v, d, 16);
    return v;
}

static u64 fpscr(void)
{
    double f;
    asm volatile("mffs %0" : "=d"(f));
    u64 u;
    memcpy(&u, &f, 8);
    return u & 0xffffffff;
}

static void clr(void)
{
    asm volatile("mtfsf 0xff,%0" : : "d"(0.0));
}

static void dump(const char *n, __vector_quad *acc)
{
    vec_t rows[4];
    __builtin_mma_disassemble_acc(rows, acc);
    printf("%s", n);
    for (int i = 0; i < 4; i++) {
        u64 d[2];
        memcpy(d, &rows[i], 16);
        printf(" %016llx%016llx", (unsigned long long)d[1],
               (unsigned long long)d[0]);
    }
    printf(" fpscr %08llx\n", (unsigned long long)fpscr());
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 3000;
    for (int k = 0; k < n; k++) {
        __vector_quad acc;
        vec_t a = vf(), b = vf(), c0 = vf(), c1 = vf(), c2 = vf(), c3 = vf();
        vec_t p0 = vd(), p1 = vd(), q = vd();
        __vector_pair ap;

        clr();
        __builtin_mma_xvf32ger(&acc, a, b); dump("f32ger", &acc);
        clr();
        __builtin_mma_build_acc(&acc, c0, c1, c2, c3);
        __builtin_mma_xvf32gerpp(&acc, a, b); dump("f32gerpp", &acc);
        clr();
        __builtin_mma_build_acc(&acc, c0, c1, c2, c3);
        __builtin_mma_xvf32gerpn(&acc, a, b); dump("f32gerpn", &acc);
        clr();
        __builtin_mma_build_acc(&acc, c0, c1, c2, c3);
        __builtin_mma_xvf32gernp(&acc, a, b); dump("f32gernp", &acc);
        clr();
        __builtin_mma_build_acc(&acc, c0, c1, c2, c3);
        __builtin_mma_xvf32gernn(&acc, a, b); dump("f32gernn", &acc);
        clr();
        __builtin_mma_build_acc(&acc, c0, c1, c2, c3);
        __builtin_mma_pmxvf32gerpp(&acc, a, b, 0xb, 0x6); dump("pmf32gerpp", &acc);
        clr();
        __builtin_mma_build_acc(&acc, c0, c1, c2, c3);
        __builtin_mma_pmxvf32gerpp(&acc, a, b, 0xf, 0xf); dump("pmf32gerppF", &acc);

        __builtin_vsx_build_pair(&ap, p0, p1);
        clr();
        __builtin_mma_xvf64ger(&acc, ap, q); dump("f64ger", &acc);
        clr();
        __builtin_mma_build_acc(&acc, c0, c1, c2, c3);
        __builtin_mma_xvf64gerpp(&acc, ap, q); dump("f64gerpp", &acc);
        clr();
        __builtin_mma_build_acc(&acc, c0, c1, c2, c3);
        __builtin_mma_xvf64gerpn(&acc, ap, q); dump("f64gerpn", &acc);
        clr();
        __builtin_mma_build_acc(&acc, c0, c1, c2, c3);
        __builtin_mma_xvf64gernp(&acc, ap, q); dump("f64gernp", &acc);
        clr();
        __builtin_mma_build_acc(&acc, c0, c1, c2, c3);
        __builtin_mma_xvf64gernn(&acc, ap, q); dump("f64gernn", &acc);
        clr();
        __builtin_mma_build_acc(&acc, c0, c1, c2, c3);
        __builtin_mma_pmxvf64gerpp(&acc, ap, q, 0x5, 0x2); dump("pmf64gerpp", &acc);
    }
    return 0;
}
