#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
/* code rewritten by vse8.v / vse32.v while translated: the write-protect
   fault of the host store must invalidate the TBs of that page */
typedef long (*fn_t)(void);
int main(void)
{
    uint32_t *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    long sum = 0;
    for (int it = 0; it < 200; it++) {
        uint32_t insn[4] = {
            0x00000513 | ((it & 0x7ff) << 20),   /* li a0, it */
            0x00008067,                          /* ret */
            0x00000013, 0x00000013 };
        long vl;
        if (it & 1) {
            asm volatile("vsetivli %0, 16, e8, m1, ta, ma\n\t"
                         "vle8.v v1, (%1)\n\t"
                         "vse8.v v1, (%2)\n\t"
                         : "=&r"(vl) : "r"(insn), "r"(code) : "memory", "v1");
        } else {
            asm volatile("vsetivli %0, 3, e32, m1, tu, mu\n\t"
                         "vle32.v v1, (%1)\n\t"
                         "vse32.v v1, (%2)\n\t"
                         : "=&r"(vl) : "r"(insn), "r"(code) : "memory", "v1");
        }
        asm volatile("fence.i" ::: "memory");
        sum = sum * 3 + ((fn_t)code)();
    }
    printf("smc %ld\n", sum);
    return 0;
}
