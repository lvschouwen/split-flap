#include "FollowerLog.h"

#include <Arduino.h>
#include <new>

#include "FollowerMem.h"

// One ring, in a buffer of FollowerMem.h, claimed by the first line logged
// (the heaps exist before any constructor runs). The ESP-01 is a cooperative
// single-core superloop: a web handler that logs runs inside loop()'s yield,
// so the link's reader and the SerialPrint writers never truly preempt each
// other — no lock needed.
static FollowerLogRing* g_ring = nullptr;

static FollowerLogRing& ring() {
  if (g_ring == nullptr) {
    void* mem = followerBufAlloc(sizeof(FollowerLogRing));
    // No memory for the only log this board has: nothing can report it, and
    // nothing after it would work either.
    if (mem == nullptr) abort();
    g_ring = new (mem) FollowerLogRing;
  }
  return *g_ring;
}

FollowerLogPrinter followerLogPrinter;

size_t FollowerLogPrinter::write(uint8_t b) {
  ring().appendStamped((const char*)&b, 1, millis() / 1000UL);
  return 1;
}

size_t FollowerLogPrinter::write(const uint8_t* buffer, size_t size) {
  ring().appendStamped((const char*)buffer, size, millis() / 1000UL);
  return size;
}

const FollowerLogRing& followerLogRing() { return ring(); }
