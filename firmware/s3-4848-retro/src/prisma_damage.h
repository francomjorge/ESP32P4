#pragma once
#include <stdint.h>
// 64-byte plane blocks -> 32-native-pixel candidates. A plane byte is
// four native pixels apart; each candidate is eight bytes in each plane.
static inline uint16_t prismaDamageCandidates(const uint32_t *dirty,unsigned a){
  unsigned remaining=80,offset=0,mask=0;
  a&=65535;
  while(remaining){
    const unsigned block=a>>6;
    unsigned n=64-(a&63);if(n>remaining)n=remaining;
    if(dirty[block>>5]&(1u<<(block&31))){
      const unsigned lo=offset>>3,hi=(offset+n-1)>>3;
      mask|=((1u<<(hi-lo+1))-1)<<lo;
    }
    offset+=n;remaining-=n;a=(a+n)&65535;
  }
  return uint16_t(mask);
}
// Invert the exact existing nearest-neighbor mapping; no geometry change.
static inline void prismaNativeEdges(const uint16_t *xmap,unsigned width,uint16_t *edges){
  unsigned x=0;
  for(unsigned p=0;p<=320;++p){
    while(x<width&&(xmap[x]>>1)<p)++x;
    edges[p]=x;
  }
}
// Exact visible color difference inside a candidate, preserving palette aliases.
static inline uint32_t prismaPixelDamage(const uint8_t (&now)[4][80],const uint8_t (&prior)[4][80],const uint16_t *palette,unsigned segment){
  uint32_t bits=0;
  for(unsigned a=0;a<8;++a)for(unsigned p=0;p<4;++p){
    const unsigned offset=segment*8+a;
    const uint8_t old=prior[p][offset],v=now[p][offset];
    if(old!=v&&palette[old]!=palette[v])bits|=1u<<(a*4+p);
  }
  return bits;
}
static inline unsigned prismaDamageRunEnd(uint32_t bits,unsigned first){
  const uint32_t remaining=bits>>first;
  return first+((~remaining)?unsigned(__builtin_ctz(~remaining)):32u);
}
