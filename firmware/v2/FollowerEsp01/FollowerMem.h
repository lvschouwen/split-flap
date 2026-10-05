#pragma once
// FollowerMem.h — where this board's large buffers come from (#564).
//
// The image is built with a second heap (platformio.ini,
// PIO_FRAMEWORK_ARDUINO_MMU_CACHE16_IRAM48_SECHEAP_SHARED): the code memory
// the image leaves unused. A byte read or written there costs about 3 us (the
// core emulates it in an exception; whole words are as fast as anywhere), and
// the SDK and lwIP never allocate from it. So it holds buffers that are
// filled once and copied out — the unit facts document, the boot dump block,
// the log ring — and nothing a hot path walks byte by byte.

#include <stddef.h>
#include <stdint.h>

// Free memory that must remain in the first heap beside a buffer taken from
// it: the web library's send buffer and response objects.
#define FOLLOWER_BUF_HEAP_MARGIN 1536

// A buffer from the second heap; from the first heap when the second cannot
// serve and the first keeps its margin; nullptr otherwise. Given back with
// followerBufFree(). No yield inside, so no other allocation lands in the
// second heap by accident.
void* followerBufAlloc(size_t bytes);
void followerBufFree(void* buf);

// Free bytes in the second heap; 0 on an image built without one.
uint32_t followerSecondHeapFree();
