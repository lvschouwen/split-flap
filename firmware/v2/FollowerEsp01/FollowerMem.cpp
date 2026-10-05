#include "FollowerMem.h"

#include <Arduino.h>
#include <stdlib.h>
#include <umm_malloc/umm_heap_select.h>

void* followerBufAlloc(size_t bytes) {
#ifdef UMM_HEAP_IRAM
  {
    HeapSelectIram secondHeap;
    void* buf = malloc(bytes);
    if (buf != nullptr) return buf;
  }
#endif
  if (ESP.getMaxFreeBlockSize() < bytes + FOLLOWER_BUF_HEAP_MARGIN) return nullptr;
  return malloc(bytes);
}

// free() finds the heap a block lives in by its address.
void followerBufFree(void* buf) { free(buf); }

uint32_t followerSecondHeapFree() {
#ifdef UMM_HEAP_IRAM
  HeapSelectIram secondHeap;
  return ESP.getFreeHeap();
#else
  return 0;
#endif
}
