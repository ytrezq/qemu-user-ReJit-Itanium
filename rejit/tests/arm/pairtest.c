/* Differential test of the pairwise integer ops: A64 ADDP, [SU]MAXP, [SU]MINP; A32 VPADD, VPMAX, VPMIN. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
static uint64_t rs = 0x243f6a8885a308d3ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static uint64_t h;
static void mix(const void *p, int n) { const uint8_t *b = p; for (int i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull; }
#define T64(fn, ty, ld) { ty r = fn(ld((const void *)a), ld((const void *)b)); mix(&r, 8); }
#define T128(fn, ty, ld) { ty r = fn(ld((const void *)a), ld((const void *)b)); mix(&r, 16); }
int main(void)
{
    uint8_t a[16], b[16];
    for (int it = 0; it < 200000; it++) {
        for (int i = 0; i < 16; i += 8) { uint64_t x = rnd(), y = rnd(); memcpy(a + i, &x, 8); memcpy(b + i, &y, 8); }
        T64(vpadd_u8, uint8x8_t, vld1_u8) T64(vpadd_u16, uint16x4_t, vld1_u16) T64(vpadd_u32, uint32x2_t, vld1_u32)
        T64(vpmax_u8, uint8x8_t, vld1_u8) T64(vpmax_s8, int8x8_t, vld1_s8) T64(vpmin_u8, uint8x8_t, vld1_u8) T64(vpmin_s8, int8x8_t, vld1_s8)
        T64(vpmax_u16, uint16x4_t, vld1_u16) T64(vpmax_s16, int16x4_t, vld1_s16) T64(vpmin_u16, uint16x4_t, vld1_u16) T64(vpmin_s16, int16x4_t, vld1_s16)
        T64(vpmax_u32, uint32x2_t, vld1_u32) T64(vpmax_s32, int32x2_t, vld1_s32) T64(vpmin_u32, uint32x2_t, vld1_u32) T64(vpmin_s32, int32x2_t, vld1_s32)
#ifdef __aarch64__
        T128(vpaddq_u8, uint8x16_t, vld1q_u8) T128(vpaddq_u16, uint16x8_t, vld1q_u16) T128(vpaddq_u32, uint32x4_t, vld1q_u32) T128(vpaddq_u64, uint64x2_t, vld1q_u64)
        T128(vpmaxq_u8, uint8x16_t, vld1q_u8) T128(vpmaxq_s8, int8x16_t, vld1q_s8) T128(vpminq_u8, uint8x16_t, vld1q_u8) T128(vpminq_s8, int8x16_t, vld1q_s8)
        T128(vpmaxq_u16, uint16x8_t, vld1q_u16) T128(vpmaxq_s16, int16x8_t, vld1q_s16) T128(vpminq_u16, uint16x8_t, vld1q_u16) T128(vpminq_s16, int16x8_t, vld1q_s16)
        T128(vpmaxq_u32, uint32x4_t, vld1q_u32) T128(vpmaxq_s32, int32x4_t, vld1q_s32) T128(vpminq_u32, uint32x4_t, vld1q_u32) T128(vpminq_s32, int32x4_t, vld1q_s32)
#endif
    }
    printf("hash %016llx\n", (unsigned long long)h);
    return 0;
}
