/* AArch64: 100 M calls of a function with PACIASP/AUTIASP (build with -mbranch-protection=standard). */
__attribute__((noinline)) long g(long x);
__attribute__((noinline)) long f(long x) { return g(x) + 1; }
__attribute__((noinline)) long g(long x) { return x * 3; }
int main(void) { long s = 0; for (long i = 0; i < 100000000; i++) s += f(i); return s & 1; }
