#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
__attribute__((noinline)) uint64_t int_kernel(uint64_t n){ uint64_t h=1469598103934665603ULL,x=n;
  for(uint64_t i=0;i<n;i++){ x^=x<<13; x^=x>>7; x^=x<<17; h=(h^(x&0xff))*1099511628211ULL; if(h&1) h+=i; } return h; }
__attribute__((noinline)) double fp_kernel(int n){ double s=0,a=1.0000001,b=0.9999999,c=0.5;
  for(int i=0;i<n;i++){ c=c*a+b*1e-9; s+=c/(1.0+i); a=a*b+1e-12; } return s; }
static float A[1<<16],B[1<<16],C[1<<16];
__attribute__((noinline)) float vec_kernel(int reps){ for(int r=0;r<reps;r++) for(int i=0;i<(1<<16);i++) C[i]=A[i]*B[i]+C[i]*0.5f; return C[123]; }
__attribute__((noinline)) size_t str_kernel(int reps){ static char buf[1<<16]; memset(buf,'a',sizeof buf-1); size_t t=0;
  for(int r=0;r<reps;r++){ buf[(r*7919)%(sizeof buf-1)]='b'; t+=strlen(buf)+(size_t)(strchr(buf,'b')-buf); memmove(buf+1,buf,4096);} return t; }
int main(void){ for(int i=0;i<(1<<16);i++){A[i]=i*0.001f;B[i]=1.0f/(i+1);C[i]=0;}
  double t=now(); uint64_t h=int_kernel(200000000); double ti=now()-t;
  t=now(); double s=fp_kernel(50000000); double tf=now()-t;
  t=now(); float v=vec_kernel(2000); double tv=now()-t;
  t=now(); size_t z=str_kernel(20000); double ts=now()-t;
  printf("entier %.2f s | flottant %.2f s | vecteur %.2f s | chaînes %.2f s   (%llx %g %g %zu)\n",ti,tf,tv,ts,(unsigned long long)h,s,v,z); }
