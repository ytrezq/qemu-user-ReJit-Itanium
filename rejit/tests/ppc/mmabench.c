#include <stdio.h>
#include <time.h>
#include <altivec.h>
typedef vector unsigned char vec_t;
static float A[1024 * 4], B[1024 * 4];
int main(void) {
    for (int i = 0; i < 4096; i++) { A[i] = 1.0f / (i + 1); B[i] = (i % 7) * 0.25f; }
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    __vector_quad acc; __builtin_mma_xxsetaccz(&acc);
    for (int r = 0; r < 20000; r++)
        for (int k = 0; k < 1024; k += 2) {
            vec_t a0 = (vec_t)vec_xl(0, &A[4 * k]), b0 = (vec_t)vec_xl(0, &B[4 * k]);
            vec_t a1 = (vec_t)vec_xl(0, &A[4 * k + 4]), b1 = (vec_t)vec_xl(0, &B[4 * k + 4]);
            __builtin_mma_xvf32gerpp(&acc, a0, b0);
            __builtin_mma_xvf32gerpp(&acc, a1, b1);
        }
    vector float rows[4]; __builtin_mma_disassemble_acc(rows, &acc);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("MMA sgemm-like: %.2f s, c[0]=%g c[15]=%g (%.1f GFLOP/s)\n", (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9, rows[0][0], rows[3][3],
           20000.0 * 1024 * 32 / ((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9) * 1e-9);
    return 0;
}
