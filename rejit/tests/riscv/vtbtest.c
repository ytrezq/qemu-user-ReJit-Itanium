/* in-TB vsetivli followed by an indirect jump: the jalr probe must use the
   vector state of the vsetivli, not that of the TB start */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

uint8_t outbuf[64] __attribute__((aligned(64)));
uint64_t hsum;

/* consume: uses the current vtype/vl: vid, vadd.vx a0, whole store, vl, vtype */
asm(".text\n.globl consume\nconsume:\n"
    "  vid.v v8\n"
    "  vadd.vx v8, v8, a0\n"
    "  lla t0, outbuf\n"
    "  vs1r.v v8, (t0)\n"
    "  csrr a0, vl\n"
    "  csrr t1, vtype\n"
    "  slli t1, t1, 16\n"
    "  or a0, a0, t1\n"
    "  ret\n");
/* setters: vsetivli (in the same TB as the tail call), then jr to consume */
#define SETTER(name, vt) \
    asm(".text\n.globl " #name "\n" #name ":\n  addi a0, a0, 1\n  vsetivli zero, " vt "\n" \
        "  lla t2, consume\n  jr t2\n");
SETTER(set_a, "4, e32, m1, tu, mu")
SETTER(set_b, "16, e8, m1, tu, mu")
SETTER(set_c, "5, e16, m2, tu, mu")
SETTER(set_d, "1, e64, m1, tu, mu")
SETTER(set_e, "31, e8, m2, tu, mu")
SETTER(set_f, "2, e32, mf2, tu, mu")
SETTER(set_g, "8, e16, m1, ta, ma")
SETTER(set_h, "3, e8, mf4, tu, mu")
extern long set_a(long), set_b(long), set_c(long), set_d(long);
extern long set_e(long), set_f(long), set_g(long), set_h(long);

int main(void)
{
    long (*fs[8])(long) = { set_a, set_b, set_c, set_d, set_e, set_f, set_g, set_h };
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 200000; i++) {
        long r;
        memset(outbuf, i & 0xff, sizeof(outbuf));
        r = fs[(i * 7 + (i >> 3)) % 8](i);
        h = (h ^ (uint64_t)r) * 0x100000001b3ull;
        for (int k = 0; k < 16; k++) {
            h = (h ^ outbuf[k]) * 0x100000001b3ull;
        }
    }
    printf("vtb %016llx\n", (unsigned long long)h);
    return 0;
}
