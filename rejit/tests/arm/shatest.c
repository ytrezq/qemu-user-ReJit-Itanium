/* Differential test of the A64 SHA-256 instructions: SHA256H, SHA256H2, SHA256SU0, SHA256SU1. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
static uint64_t rs = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static uint32x4_t rv(void) { uint32_t w[4]; for (int i = 0; i < 4; i++) w[i] = (uint32_t)rnd(); return vld1q_u32(w); }
static uint64_t h;
static void mix(uint32x4_t v) { uint32_t w[4]; vst1q_u32(w, v); for (int i = 0; i < 4; i++) h = (h ^ w[i]) * 0x100000001b3ull; }
int main(void)
{
    for (int i = 0; i < 300000; i++) {
        uint32x4_t a = rv(), b = rv(), c = rv();
        mix(vsha256hq_u32(a, b, c));
        mix(vsha256h2q_u32(a, b, c));
        mix(vsha256su0q_u32(a, b));
        mix(vsha256su1q_u32(a, b, c));
    }
    printf("hash %016llx\n", (unsigned long long)h);
    return 0;
}
