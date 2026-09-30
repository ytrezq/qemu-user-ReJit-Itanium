/* RVV vsetvl/vsetvli/vsetivli differential test: vl/vtype results and loops using them. */
#include <stdio.h>
#include <stdint.h>
#include <riscv_vector.h>
static uint64_t h = 1469598103934665603ull;
static void mix(uint64_t v) { h = (h ^ v) * 1099511628211ull; }
#define VSETVLI(avl, vt) ({ unsigned long _vl; asm volatile("vsetvli %0, %1, " vt : "=r"(_vl) : "r"(avl)); _vl; })
#define VSETIVLI(avl, vt) ({ unsigned long _vl; asm volatile("vsetivli %0, " #avl ", " vt : "=r"(_vl)); _vl; })
static unsigned long getvl(void) { unsigned long v; asm volatile("csrr %0, vl" : "=r"(v)); return v; }
static unsigned long getvtype(void) { unsigned long v; asm volatile("csrr %0, vtype" : "=r"(v)); return v; }
int main(void)
{
    static int32_t a[1000], b[1000], c[1000];
    static uint8_t x[3000], y[3000];
    for (int i = 0; i < 1000; i++) { a[i] = i * 7 - 300; b[i] = 1000 - i * 3; }
    for (int i = 0; i < 3000; i++) x[i] = (uint8_t)(i * 13 + 1);
    for (unsigned long avl = 0; avl < 300; avl += 7) {
        mix(VSETVLI(avl, "e8, m1, ta, ma")); mix(getvtype());
        mix(VSETVLI(avl, "e16, m2, tu, mu")); mix(getvtype());
        mix(VSETVLI(avl, "e32, m4, ta, mu")); mix(getvtype());
        mix(VSETVLI(avl, "e64, m8, tu, ma")); mix(getvtype());
        mix(VSETVLI(avl, "e8, mf2, ta, ma")); mix(getvtype());
        mix(VSETVLI(avl, "e8, mf8, ta, ma")); mix(getvtype());
        mix(VSETVLI(avl, "e32, mf2, ta, ma")); mix(getvtype());   /* e32 mf2: ok with ELEN 64 */
        mix(VSETVLI(avl, "e64, mf2, ta, ma")); mix(getvtype());   /* illegal: vill */
        mix(VSETIVLI(5, "e16, m1, ta, ma")); mix(getvtype());
        mix(VSETIVLI(31, "e8, mf4, ta, ma")); mix(getvtype());
        {   /* rs1 = x0, rd != x0: vl = VLMAX */
            unsigned long vl; asm volatile("vsetvli %0, x0, e32, m2, ta, ma" : "=r"(vl)); mix(vl); mix(getvtype());
            /* rd = rs1 = x0: keep vl */
            VSETVLI(avl, "e32, m1, ta, ma");
            asm volatile("vsetvli x0, x0, e16, mf2, ta, ma"); mix(getvl()); mix(getvtype());
            asm volatile("vsetvli x0, x0, e8, mf8, ta, ma"); mix(getvl()); mix(getvtype());
        }
        {   /* vsetvl with register vtype */
            unsigned long vl, vt = 0x51; asm volatile("vsetvl %0, %1, %2" : "=r"(vl) : "r"(avl), "r"(vt)); mix(vl); mix(getvtype());
        }
    }
    /* strip-mined loops at various SEW/LMUL: vl < VLMAX in the last iteration */
    for (int n = 1; n < 1000; n += 37) {
        for (size_t i = 0, vl; i < (size_t)n; i += vl) {
            vl = __riscv_vsetvl_e32m1(n - i);
            vint32m1_t va = __riscv_vle32_v_i32m1(a + i, vl), vb = __riscv_vle32_v_i32m1(b + i, vl);
            __riscv_vse32_v_i32m1(c + i, __riscv_vadd_vv_i32m1(va, vb, vl), vl);
        }
        for (int i = 0; i < n; i++) mix(c[i]);
        for (size_t i = 0, vl; i < (size_t)n * 3; i += vl) {
            vl = __riscv_vsetvl_e8m4(n * 3 - i);
            vuint8m4_t vx = __riscv_vle8_v_u8m4(x + i, vl);
            vbool2_t m = __riscv_vmsgtu_vx_u8m4_b2(vx, 100, vl);
            __riscv_vse8_v_u8m4_m(m, y + i, __riscv_vadd_vx_u8m4(vx, 5, vl), vl);
        }
        for (int i = 0; i < n * 3; i++) mix(y[i]);
        for (size_t i = 0, vl; i < (size_t)n; i += vl) {
            vl = __riscv_vsetvl_e16mf2(n - i);
            vint16mf2_t v = __riscv_vncvt_x_x_w_i16mf2(__riscv_vle32_v_i32m1(a + i, vl), vl);
            vint32m1_t w = __riscv_vsext_vf2_i32m1(__riscv_vadd_vx_i16mf2(v, 3, vl), vl);
            __riscv_vse32_v_i32m1(c + i, w, vl);
        }
        for (int i = 0; i < n; i++) mix(c[i]);
    }
    printf("hash %016llx\n", (unsigned long long)h);
    return 0;
}
