/*
 * Differential test of VSX floating-point instructions (scalar and vector):
 * prints the whole target VSR and FPSCR after each instruction.  Compare
 * the output of two QEMU builds with fpcmp.py.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef __vector unsigned long long v2u;

static inline double d_of(u64 x) { double d; memcpy(&d, &x, 8); return d; }
static inline u64 u_of(double d) { u64 x; memcpy(&x, &d, 8); return x; }

/* VSR as two doublewords in ISA order (dw0 = high on BE) */
static inline v2u mk(u64 dw0, u64 dw1)
{
    v2u v;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    v[1] = dw0; v[0] = dw1;
#else
    v[0] = dw0; v[1] = dw1;
#endif
    return v;
}
static inline u64 dw(v2u v, int i)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return v[1 - i];
#else
    return v[i];
#endif
}

static const u64 dspec[] = {
    0x0000000000000000ull, 0x8000000000000000ull, 0x0000000000000001ull,
    0x800fffffffffffffull, 0x0010000000000000ull, 0x8010000000000000ull,
    0x3ff0000000000000ull, 0xbff0000000000000ull, 0x3ff8000000000000ull,
    0xc004000000000000ull, 0x3ff0000000000001ull, 0x7fefffffffffffffull,
    0xffefffffffffffffull, 0x7ff0000000000000ull, 0xfff0000000000000ull,
    0x7ff8000000000000ull, 0xfff8000000000000ull, 0x7ffc000000000abcull,
    0x7ff0000000000001ull, 0xfff4000000000077ull, 0x47efffffe0000000ull,
    0x47effffff0000000ull, 0x3810000000000000ull, 0x36a0000000000000ull,
    0x3690000000000000ull, 0x380fffffe0000000ull, 0x41dfffffffc00000ull,
    0x41e0000000000000ull, 0xc1e0000000000000ull, 0xc1e0000000200000ull,
    0x41efffffffe00000ull, 0x41f0000000000000ull, 0x43dfffffffffffffull,
    0x43e0000000000000ull, 0xc3e0000000000000ull, 0x43f0000000000000ull,
    0x4330000000000001ull, 0x3fdfffffffffffffull, 0x400921fb54442d18ull,
    0x3fb999999999999aull, 0x41dfffffffe00000ull, 0xc1dfffffffe00000ull,
    0x3fe0000000000000ull, 0xbfe0000000000000ull, 0x4004000000000000ull,
};
#define NDSPEC (sizeof(dspec) / sizeof(dspec[0]))

static const u32 fspec[] = {
    0x00000000, 0x80000000, 0x00000001, 0x807fffff, 0x00800000, 0x80800000,
    0x3f800000, 0xbf800000, 0x3fc00000, 0xc0200000, 0x3f800001, 0x7f7fffff,
    0xff7fffff, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7fe00abc,
    0x7f800001, 0xffa00077, 0x4effffff, 0x4f000000, 0xcf000000, 0xcf000001,
    0x4f7fffff, 0x4f800000, 0x5effffff, 0x5f000000, 0xdf000000, 0x5f800000,
    0x3effffff, 0x40490fdb, 0x3dcccccd, 0x3f000000, 0xbf000000, 0x40200000,
    0x00400000, 0x80000010, 0x3f7fffff, 0x33800000,
};
#define NFSPEC (sizeof(fspec) / sizeof(fspec[0]))

static u64 dv[256];
static u32 fv[256];
static int ndv, nfv;
static u64 rs = 0x243f6a8885a308d3ull;
static u64 rnd(void)
{
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return rs;
}

static void build(int nrand)
{
    for (size_t i = 0; i < NDSPEC; i++) {
        dv[ndv++] = dspec[i];
    }
    for (size_t i = 0; i < NFSPEC; i++) {
        fv[nfv++] = fspec[i];
    }
    for (int i = 0; i < nrand; i++) {
        u64 r = rnd();
        u64 e = (i & 1) ? 1023 - 60 + rnd() % 120 : rnd() % 2048;
        dv[ndv++] = (r & 0x800fffffffffffffull) | (e << 52);
        u32 q = (u32)rnd();
        u32 fe = (i & 1) ? 127 - 30 + rnd() % 60 : rnd() % 256;
        fv[nfv++] = (q & 0x807fffff) | (fe << 23);
    }
}

static u64 fpscr_now(void)
{
    double f;
    asm volatile("mffs %0" : "=d"(f));
    return u_of(f) & 0xffffffff;
}

#define Z "mtfsf 0xff,%[z]\n\t"

static void out(const char *n, int nin, const v2u *in, v2u r, u64 f)
{
    printf("%s", n);
    for (int i = 0; i < nin; i++) {
        printf(" %016llx:%016llx", (unsigned long long)dw(in[i], 0),
               (unsigned long long)dw(in[i], 1));
    }
    printf(" -> %016llx:%016llx %08llx\n", (unsigned long long)dw(r, 0),
           (unsigned long long)dw(r, 1), (unsigned long long)f);
}

/* xt = op(xa, xb) */
#define X3(name, insn)                                                     \
static void t_##name(v2u a, v2u b)                                         \
{                                                                          \
    v2u r; double fs; v2u in[2] = { a, b };                                \
    asm volatile(Z insn " %x[r],%x[a],%x[b]\n\tmffs %[f]"                  \
                 : [r]"=&wa"(r), [f]"=&d"(fs)                              \
                 : [a]"wa"(a), [b]"wa"(b), [z]"d"(0.0));                   \
    out(#name, 2, in, r, u_of(fs) & 0xffffffff);                           \
}
/* xt = op(xa, xb, xt) */
#define X3T(name, insn)                                                    \
static void t_##name(v2u a, v2u b, v2u t)                                  \
{                                                                          \
    v2u r = t; double fs; v2u in[3] = { a, b, t };                         \
    asm volatile(Z insn " %x[r],%x[a],%x[b]\n\tmffs %[f]"                  \
                 : [r]"+&wa"(r), [f]"=&d"(fs)                              \
                 : [a]"wa"(a), [b]"wa"(b), [z]"d"(0.0));                   \
    out(#name, 3, in, r, u_of(fs) & 0xffffffff);                           \
}
/* xt = op(xb) */
#define X2(name, insn)                                                     \
static void t_##name(v2u b)                                                \
{                                                                          \
    v2u r; double fs; v2u in[1] = { b };                                   \
    asm volatile(Z insn " %x[r],%x[b]\n\tmffs %[f]"                        \
                 : [r]"=&wa"(r), [f]"=&d"(fs)                              \
                 : [b]"wa"(b), [z]"d"(0.0));                               \
    out(#name, 1, in, r, u_of(fs) & 0xffffffff);                           \
}
/* record form: CR6 appended as a third "input" */
#define X3R(name, insn)                                                    \
static void t_##name(v2u a, v2u b)                                         \
{                                                                          \
    v2u r; double fs; unsigned long c;                                     \
    asm volatile(Z insn " %x[r],%x[a],%x[b]\n\tmfcr %[c]\n\tmffs %[f]"     \
                 : [r]"=&wa"(r), [f]"=&d"(fs), [c]"=&r"(c)                 \
                 : [a]"wa"(a), [b]"wa"(b), [z]"d"(0.0) : "cr6");           \
    v2u in[3] = { a, b, mk((c >> 4) & 0xf, 0) };                           \
    out(#name, 3, in, r, u_of(fs) & 0xffffffff);                           \
}
/* CR field compares */
#define XCR(name, insn)                                                    \
static void t_##name(v2u a, v2u b)                                         \
{                                                                          \
    double fs; unsigned long c;                                            \
    asm volatile(Z insn " 6,%x[a],%x[b]\n\tmfcr %[c]\n\tmffs %[f]"         \
                 : [f]"=&d"(fs), [c]"=&r"(c)                               \
                 : [a]"wa"(a), [b]"wa"(b), [z]"d"(0.0) : "cr6");           \
    v2u in[2] = { a, b };                                                  \
    out(#name, 2, in, mk((c >> 4) & 0xf, 0), u_of(fs) & 0xffffffff);       \
}

X3(xsadddp, "xsadddp") X3(xssubdp, "xssubdp") X3(xsmuldp, "xsmuldp")
X3(xsdivdp, "xsdivdp") X3(xsaddsp, "xsaddsp") X3(xssubsp, "xssubsp")
X3(xsmulsp, "xsmulsp") X3(xsdivsp, "xsdivsp")
X3(xscmpeqdp, "xscmpeqdp") X3(xscmpgedp, "xscmpgedp")
X3(xscmpgtdp, "xscmpgtdp") X3(xsmaxcdp, "xsmaxcdp") X3(xsmincdp, "xsmincdp") X3(xsmaxdp, "xsmaxdp") X3(xsmindp, "xsmindp")
X3(xvmaxdp, "xvmaxdp") X3(xvmindp, "xvmindp") X3(xvmaxsp, "xvmaxsp") X3(xvminsp, "xvminsp")
X3T(xsmaddadp, "xsmaddadp") X3T(xsmaddmdp, "xsmaddmdp")
X3T(xsmsubadp, "xsmsubadp") X3T(xsmsubmdp, "xsmsubmdp")
X3T(xsnmaddadp, "xsnmaddadp") X3T(xsnmaddmdp, "xsnmaddmdp")
X3T(xsnmsubadp, "xsnmsubadp") X3T(xsnmsubmdp, "xsnmsubmdp")
X3T(xsmaddasp, "xsmaddasp") X3T(xsmaddmsp, "xsmaddmsp")
X3T(xsnmsubasp, "xsnmsubasp") X3T(xsmsubmsp, "xsmsubmsp")
X2(xssqrtdp, "xssqrtdp") X2(xssqrtsp, "xssqrtsp")
X2(xscvdpsxds, "xscvdpsxds") X2(xscvdpuxds, "xscvdpuxds")
X2(xscvdpsxws, "xscvdpsxws") X2(xscvdpuxws, "xscvdpuxws")
X2(xscvsxddp, "xscvsxddp") X2(xscvuxddp, "xscvuxddp")
X2(xscvsxdsp, "xscvsxdsp") X2(xscvuxdsp, "xscvuxdsp")
X2(xsrdpic, "xsrdpic") X2(xsrdpim, "xsrdpim") X2(xsrdpip, "xsrdpip")
X2(xsrdpiz, "xsrdpiz") X2(xsrdpi, "xsrdpi") X2(xsrsp, "xsrsp")
XCR(xscmpudp, "xscmpudp") XCR(xscmpodp, "xscmpodp")

X3(xvadddp, "xvadddp") X3(xvsubdp, "xvsubdp") X3(xvmuldp, "xvmuldp")
X3(xvdivdp, "xvdivdp") X3(xvaddsp, "xvaddsp") X3(xvsubsp, "xvsubsp")
X3(xvmulsp, "xvmulsp") X3(xvdivsp, "xvdivsp")
X3T(xvmaddadp, "xvmaddadp") X3T(xvmaddmdp, "xvmaddmdp")
X3T(xvmsubadp, "xvmsubadp") X3T(xvnmaddadp, "xvnmaddadp")
X3T(xvnmsubmdp, "xvnmsubmdp")
X3T(xvmaddasp, "xvmaddasp") X3T(xvmaddmsp, "xvmaddmsp")
X3T(xvmsubasp, "xvmsubasp") X3T(xvnmaddasp, "xvnmaddasp")
X3T(xvnmsubmsp, "xvnmsubmsp")
X2(xvsqrtdp, "xvsqrtdp") X2(xvsqrtsp, "xvsqrtsp")
X2(xvcvsxddp, "xvcvsxddp") X2(xvcvuxddp, "xvcvuxddp")
X2(xvcvdpsxds, "xvcvdpsxds") X2(xvcvdpuxds, "xvcvdpuxds")
X2(xvcvsxwsp, "xvcvsxwsp") X2(xvcvuxwsp, "xvcvuxwsp")
X2(xvcvspsxws, "xvcvspsxws") X2(xvcvspuxws, "xvcvspuxws")
X2(xvrdpic, "xvrdpic") X2(xvrdpim, "xvrdpim") X2(xvrdpip, "xvrdpip")
X2(xvrdpiz, "xvrdpiz") X2(xvrspic, "xvrspic") X2(xvrspim, "xvrspim")
X2(xvrspip, "xvrspip") X2(xvrspiz, "xvrspiz")
/* not known to the assembler: fixed registers vs32-vs34 */
#define X3E(name, word, rec)                                               \
static void t_##name(v2u a, v2u b)                                         \
{                                                                          \
    v2u r; double fs; unsigned long c;                                     \
    asm volatile(Z "xxlor 32,%x[a],%x[a]\n\txxlor 33,%x[b],%x[b]\n\t"      \
                 ".long " #word "\n\txxlor %x[r],34,34\n\t"                \
                 "mfcr %[c]\n\tmffs %[f]"                                   \
                 : [r]"=&wa"(r), [f]"=&d"(fs), [c]"=&r"(c)                 \
                 : [a]"wa"(a), [b]"wa"(b), [z]"d"(0.0)                     \
                 : "cr6", "v0", "v1", "v2");                               \
    v2u in[3] = { a, b, mk(rec ? (c >> 4) & 0xf : 0, 0) };                 \
    out(#name, 3, in, r, u_of(fs) & 0xffffffff);                           \
}
X3E(xvcmpnedp, 4030729183, 0) X3E(xvcmpnesp, 4030728927, 0) X3E(xvcmpnesp_, 4030729951, 1)
X3(xvcmpeqdp, "xvcmpeqdp") X3(xvcmpgtdp, "xvcmpgtdp")
X3(xvcmpgedp, "xvcmpgedp")
X3(xvcmpeqsp, "xvcmpeqsp") X3(xvcmpgtsp, "xvcmpgtsp")
X3(xvcmpgesp, "xvcmpgesp")
X3R(xvcmpeqdp_, "xvcmpeqdp.") X3R(xvcmpgtdp_, "xvcmpgtdp.")
X3R(xvcmpgesp_, "xvcmpgesp.")

typedef void (*f1)(v2u);
typedef void (*f2)(v2u, v2u);
typedef void (*f3)(v2u, v2u, v2u);

static v2u dvec(int i, int j)
{
    return mk(dv[i % ndv], dv[j % ndv]);
}

static v2u fvec(int i, int j)
{
    u32 w0 = fv[i % nfv], w1 = fv[j % nfv];
    u32 w2 = fv[(i + j) % nfv], w3 = fv[(i * 7 + j * 3 + 1) % nfv];
    return mk(((u64)w0 << 32) | w1, ((u64)w2 << 32) | w3);
}

/* scalar operands: doubleword 0 matters, doubleword 1 is junk */
static v2u svec(int i, int j)
{
    return mk(dv[i % ndv], 0x5555555555555555ull ^ dv[j % ndv]);
}

int main(int argc, char **argv)
{
    int nrand = argc > 1 ? atoi(argv[1]) : 30;
    build(nrand);
    setvbuf(stdout, NULL, _IOFBF, 1 << 20);

    struct { f2 f; int kind; } bin[] = {
        { t_xsadddp, 0 }, { t_xssubdp, 0 }, { t_xsmuldp, 0 },
        { t_xsdivdp, 0 }, { t_xsaddsp, 0 }, { t_xssubsp, 0 },
        { t_xsmulsp, 0 }, { t_xsdivsp, 0 }, { t_xscmpeqdp, 0 },
        { t_xscmpgedp, 0 }, { t_xscmpgtdp, 0 }, { t_xsmaxcdp, 0 },
        { t_xsmincdp, 0 }, { t_xscmpudp, 0 }, { t_xscmpodp, 0 },
        { t_xsmaxdp, 0 }, { t_xsmindp, 0 }, { t_xvmaxdp, 1 }, { t_xvmindp, 1 },
        { t_xvmaxsp, 2 }, { t_xvminsp, 2 },
        { t_xvadddp, 1 }, { t_xvsubdp, 1 }, { t_xvmuldp, 1 },
        { t_xvdivdp, 1 }, { t_xvcmpeqdp, 1 }, { t_xvcmpgtdp, 1 },
        { t_xvcmpgedp, 1 }, { t_xvcmpnedp, 1 }, { t_xvcmpeqdp_, 1 },
        { t_xvcmpgtdp_, 1 },
        { t_xvaddsp, 2 }, { t_xvsubsp, 2 }, { t_xvmulsp, 2 },
        { t_xvdivsp, 2 }, { t_xvcmpeqsp, 2 }, { t_xvcmpgtsp, 2 },
        { t_xvcmpgesp, 2 }, { t_xvcmpnesp, 2 }, { t_xvcmpgesp_, 2 },
        { t_xvcmpnesp_, 2 },
    };
    for (size_t k = 0; k < sizeof(bin) / sizeof(bin[0]); k++) {
        int n = bin[k].kind == 2 ? nfv : ndv;
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < n; j++) {
                switch (bin[k].kind) {
                case 0: bin[k].f(svec(i, j), svec(j, i)); break;
                case 1: bin[k].f(dvec(i, j), dvec(j + 1, i)); break;
                case 2: bin[k].f(fvec(i, j), fvec(j + 2, i)); break;
                }
            }
        }
    }
    struct { f3 f; int kind; } ter[] = {
        { t_xsmaddadp, 0 }, { t_xsmaddmdp, 0 }, { t_xsmsubadp, 0 },
        { t_xsmsubmdp, 0 }, { t_xsnmaddadp, 0 }, { t_xsnmaddmdp, 0 },
        { t_xsnmsubadp, 0 }, { t_xsnmsubmdp, 0 }, { t_xsmaddasp, 0 },
        { t_xsmaddmsp, 0 }, { t_xsnmsubasp, 0 }, { t_xsmsubmsp, 0 },
        { t_xvmaddadp, 1 }, { t_xvmaddmdp, 1 }, { t_xvmsubadp, 1 },
        { t_xvnmaddadp, 1 }, { t_xvnmsubmdp, 1 },
        { t_xvmaddasp, 2 }, { t_xvmaddmsp, 2 }, { t_xvmsubasp, 2 },
        { t_xvnmaddasp, 2 }, { t_xvnmsubmsp, 2 },
    };
    for (size_t k = 0; k < sizeof(ter) / sizeof(ter[0]); k++) {
        int n = (ter[k].kind == 2 ? nfv : ndv);
        for (int i = 0; i < n; i += 2) {
            for (int j = 0; j < n; j += 2) {
                for (int l = 0; l < n; l += 3) {
                    switch (ter[k].kind) {
                    case 0:
                        ter[k].f(svec(i, j), svec(j, l), svec(l, i));
                        break;
                    case 1:
                        ter[k].f(dvec(i, j), dvec(j, l), dvec(l, i));
                        break;
                    case 2:
                        ter[k].f(fvec(i, j), fvec(j, l), fvec(l, i));
                        break;
                    }
                }
            }
        }
    }
    struct { f1 f; int kind; } un[] = {
        { t_xssqrtdp, 0 }, { t_xssqrtsp, 0 }, { t_xscvdpsxds, 0 },
        { t_xscvdpuxds, 0 }, { t_xscvdpsxws, 0 }, { t_xscvdpuxws, 0 },
        { t_xscvsxddp, 0 }, { t_xscvuxddp, 0 }, { t_xscvsxdsp, 0 },
        { t_xscvuxdsp, 0 }, { t_xsrdpic, 0 }, { t_xsrdpim, 0 },
        { t_xsrdpip, 0 }, { t_xsrdpiz, 0 }, { t_xsrdpi, 0 }, { t_xsrsp, 0 },
        { t_xvsqrtdp, 1 }, { t_xvcvsxddp, 1 }, { t_xvcvuxddp, 1 },
        { t_xvcvdpsxds, 1 }, { t_xvcvdpuxds, 1 }, { t_xvrdpic, 1 },
        { t_xvrdpim, 1 }, { t_xvrdpip, 1 }, { t_xvrdpiz, 1 },
        { t_xvsqrtsp, 2 }, { t_xvcvsxwsp, 2 }, { t_xvcvuxwsp, 2 },
        { t_xvcvspsxws, 2 }, { t_xvcvspuxws, 2 }, { t_xvrspic, 2 },
        { t_xvrspim, 2 }, { t_xvrspip, 2 }, { t_xvrspiz, 2 },
    };
    for (size_t k = 0; k < sizeof(un) / sizeof(un[0]); k++) {
        int n = (un[k].kind == 2 ? nfv : ndv);
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < 3; j++) {
                switch (un[k].kind) {
                case 0: un[k].f(svec(i, i + j)); break;
                case 1: un[k].f(dvec(i, i + j + 1)); break;
                case 2: un[k].f(fvec(i, i + j + 1)); break;
                }
            }
        }
    }
    /* sequence: vector op flags + scalar FPRF + compare */
    {
        v2u a = dvec(3, 11), b = dvec(12, 17), r, t;
        double fs;
        unsigned long c;
        asm volatile(Z "xvmuldp %x[r],%x[a],%x[b]\n\t"
                     "xsadddp %x[t],%x[a],%x[b]\n\t"
                     "xscmpudp 7,%x[a],%x[b]\n\t"
                     "xvaddsp %x[r],%x[r],%x[b]\n\tmfcr %[c]\n\tmffs %[f]"
                     : [r]"=&wa"(r), [t]"=&wa"(t), [f]"=&d"(fs), [c]"=&r"(c)
                     : [a]"wa"(a), [b]"wa"(b), [z]"d"(0.0) : "cr7");
        printf("vseq %016llx %016llx %lx fpscr %08llx\n",
               (unsigned long long)dw(r, 0), (unsigned long long)dw(t, 0),
               c & 0xf, (unsigned long long)u_of(fs) & 0xffffffff);
    }
    (void)fpscr_now;
    return 0;
}
