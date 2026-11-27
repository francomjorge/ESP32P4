#pragma once
#include <stdint.h>
// Preserve the clock phase; late service coalesces missed slots rather
// than producing a burst of identical snapshots. Clock is monotonic.
static inline uint64_t prismaFrameSlots(uint64_t now,uint64_t &last,uint64_t period){
  if(!period||now<last||now-last<period)return 0;
  const uint64_t slots=(now-last)/period;
  last+=slots*period;
  return slots;
}
