/* AArch64: loops that GCC -O3 vectorizes into FSQRT, FCVTZS, FRINTM, SCVTF (4S/2D). */
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#define N 4096
float a[N], b[N]; int32_t c[N]; double d[N], e[N]; int64_t f[N];
int main(void)
{
    for (int i = 0; i < N; i++) { a[i] = i * 0.37f + 1; d[i] = i * 1.37 + 1; }
    int64_t s = 0;
    for (int r = 0; r < 20000; r++) {
        for (int i = 0; i < N; i++) b[i] = sqrtf(a[i]);
        for (int i = 0; i < N; i++) c[i] = (int32_t)b[i];
        for (int i = 0; i < N; i++) e[i] = __builtin_floor(d[i] * 0.5);
        for (int i = 0; i < N; i++) f[i] = (int64_t)e[i] + (int64_t)c[i & 1023];
        for (int i = 0; i < N; i++) a[i] = (float)c[i] + 1.0f;
        s += f[r & (N - 1)];
    }
    printf("%lld\n", (long long)s);
    return 0;
}
