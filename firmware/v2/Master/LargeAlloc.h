#pragma once
// LargeAlloc.h — the v2 memory policy's allocation helper (#187).
//
// Policy (spec 2026-07-09): task stacks, queues, ISR/DMA/Wire buffers live
// in internal SRAM (RTOS/hardware requirement, statically allocated). Large
// elastic buffers — anything KB-sized that tolerates PSRAM latency, like
// the web log ring — go through largeAlloc(): PSRAM when the module has it,
// internal heap otherwise. One binary is correct on every devkit variant;
// enabling PSRAM (BOARD_HAS_PSRAM + memory_type in platformio.ini) simply
// relocates these buffers off the internal heap.

#include <stdlib.h>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>

inline void* largeAlloc(size_t size) {
  void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p != nullptr) return p;
  return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

#else  // native test env

inline void* largeAlloc(size_t size) { return malloc(size); }

#endif
