#pragma once
// FactoryChunkPlan — pure routing of one rescue-upload chunk (#195).
// FactorySlot holds flash sector 0 back in RAM until factoryWriteEnd()'s MD5
// verdict, so an interleaved, torn, or abandoned upload can never leave a
// bootable-looking header in the factory slot (there is no sibling slot to
// fall back to). This computes, for a chunk arriving at `writeOffset`, how
// many leading bytes belong in the RAM header buffer and what remainder goes
// straight to flash. Natively tested (test_factory_chunk_plan).

#include <stddef.h>

struct FactoryChunkPlan {
  size_t headerBytes;  // copy data[0..headerBytes) into headerBuf[writeOffset..]
  size_t flashOffset;  // partition offset for the remainder (if flashBytes > 0)
  size_t flashBytes;   // bytes of data[headerBytes..] written to flash
  size_t eraseTo;      // erase watermark the flash write needs (0 = no write)
};

inline FactoryChunkPlan planFactoryChunk(size_t writeOffset, size_t len,
                                         size_t sector) {
  FactoryChunkPlan p = {0, 0, 0, 0};
  if (writeOffset < sector) {
    size_t room = sector - writeOffset;
    p.headerBytes = (len < room) ? len : room;
  }
  p.flashBytes = len - p.headerBytes;
  if (p.flashBytes > 0) {
    p.flashOffset = writeOffset + p.headerBytes;
    size_t end = p.flashOffset + p.flashBytes;
    p.eraseTo = ((end + sector - 1) / sector) * sector;
  }
  return p;
}
