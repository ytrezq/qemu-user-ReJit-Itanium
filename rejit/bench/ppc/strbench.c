#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static char a[1<<16], b[1<<16];
int main(void){ size_t t=0; memset(a,'x',sizeof a-1); memcpy(b,a,sizeof a);
  for(int r=0;r<20000;r++){ a[(r*7919)%60000]='y'; t+=strlen(a); t+=(size_t)(strchr(a,'y')-a); t+=strcmp(a,b)!=0; t+=memcmp(a,b,sizeof a)!=0;
    memmove(a+3,a,4096); memcpy(b+r%64,a,8192); memset(b+(r%100),'x',16384); t+=strrchr(a,'y')!=0; t+=(size_t)memchr(a,'y',sizeof a)!=0; a[(r*7919)%60000]='x'; }
  printf("%zu\n",t); }
