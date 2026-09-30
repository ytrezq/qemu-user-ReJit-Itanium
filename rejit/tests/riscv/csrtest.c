#include <stdio.h>
#include <stdint.h>
#include <fenv.h>
int main(void)
{
    unsigned long acc = 0;
    for (int i = 0; i < 1000; i++) {
        unsigned long rm = i % 5, r1, r2, fl, vl, vt, vlenb, fcsr;
        asm volatile("fsrm %5\n\t"
                     "frrm %0\n\t"
                     "fsflags %6\n\t"
                     "frflags %1\n\t"
                     "frcsr %2\n\t"
                     "vsetvli %3, %7, e16, m2, ta, ma\n\t"
                     "csrr %3, vl\n\t"
                     "csrr %4, vtype\n\t"
                     : "=&r"(r1), "=&r"(fl), "=&r"(fcsr), "=&r"(vl), "=&r"(vt)
                     : "r"(rm), "r"((unsigned long)(i & 31)), "r"((unsigned long)i)
                     : "memory");
        asm volatile("csrr %0, vlenb\n\tfrrm %1" : "=r"(vlenb), "=r"(r2));
        acc = acc * 31 + r1 * 1000003 + fl * 7919 + fcsr * 104729 + vl * 13 + vt * 17 + vlenb + r2 * 5;
    }
    fesetround(FE_TONEAREST);
    printf("csr %lx %d\n", acc, fegetround());
    return 0;
}
