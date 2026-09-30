/* AArch64: 128x128 float matrix product with FMLA/FMUL by element (vfmaq_laneq_f32). */
#include <stdio.h>
#include <arm_neon.h>
#define N 128
float A[N][N], B[N][N], C[N][N];
static void kern(void)
{
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j += 4) {
            float32x4_t acc = vdupq_n_f32(0);
            for (int k = 0; k < N; k += 4) {
                float32x4_t a = vld1q_f32(&A[i][k]);
                acc = vfmaq_laneq_f32(acc, vld1q_f32(&B[k][j]), a, 0);
                acc = vfmaq_laneq_f32(acc, vld1q_f32(&B[k + 1][j]), a, 1);
                acc = vfmaq_laneq_f32(acc, vld1q_f32(&B[k + 2][j]), a, 2);
                acc = vfmaq_laneq_f32(acc, vld1q_f32(&B[k + 3][j]), a, 3);
            }
            vst1q_f32(&C[i][j], vmulq_laneq_f32(acc, vld1q_f32(&A[i][0]), 1));
        }
}
int main(void)
{
    for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) { A[i][j] = (i + j) * 0.001f; B[i][j] = (i - j) * 0.002f; }
    for (int r = 0; r < 200; r++) kern();
    printf("%f\n", C[5][7]);
    return 0;
}
