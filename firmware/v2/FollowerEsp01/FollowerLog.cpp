#include "FollowerLog.h"

// Single static ring. The ESP-01 is a cooperative single-core superloop:
// async web handlers run inside loop()'s yield, so the GET /log reader and the
// SerialPrint writers never truly preempt each other — no lock needed (the
// follower's "async handlers stage, loop() mutates" discipline).
static FollowerLogRing g_ring;

FollowerLogPrinter followerLogPrinter;

size_t FollowerLogPrinter::write(uint8_t b) {
  g_ring.append((const char*)&b, 1);
  return 1;
}

size_t FollowerLogPrinter::write(const uint8_t* buffer, size_t size) {
  g_ring.append((const char*)buffer, size);
  return size;
}

uint32_t followerLogReadSince(uint32_t after, String& out) {
  return g_ring.readSince(after, out);
}
