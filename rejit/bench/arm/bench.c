/* Micro-benchmarks: integer, scalar FP, vector FP, strings (argument: any of "ifvs"). */
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
int main(int argc,char**argv){ const char*k=argc>1?argv[1]:"ifvs"; double ti=0,tf=0,tv=0,ts=0; uint64_t h=0; double s=0; float v=0; size_t z=0; for(int i=0;i<(1<<16);i++){A[i]=i*0.001f;B[i]=1.0f/(i+1);C[i]=0;}
  double t; if(strchr(k,'i')){t=now(); h=int_kernel(200000000); ti=now()-t;}
  if(strchr(k,'f')){t=now(); s=fp_kernel(50000000); tf=now()-t;}
  if(strchr(k,'v')){t=now(); v=vec_kernel(2000); tv=now()-t;}
  if(strchr(k,'s')){t=now(); z=str_kernel(20000); ts=now()-t;}
  printf("entier %.2f s | flottant %.2f s | vecteur %.2f s | chaînes %.2f s   (%llx %g %g %zu)\n",ti,tf,tv,ts,(unsigned long long)h,s,v,z); }
