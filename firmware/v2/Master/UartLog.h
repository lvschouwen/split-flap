#pragma once
// UartLog.h — the devkit's second USB port (UART0 through its bridge, 115200).
// The console is the S3's native USB; a board flashed and watched through the
// UART port would show its bootloader's text and then fall silent. So the log
// goes out here too, and ImprovService.cpp listens here (#601).
//
// A log line never waits for this port: what does not fit in the transmit
// buffer is dropped. Target-only.

#include <Arduino.h>

#include <soc/soc_caps.h>

// A write is safe from waiting only with this much room past its length: the
// buffer keeps a header with every write.
#define UART_LOG_SLACK 32

inline void uartLogBegin() {
  Serial0.setTxBufferSize(4096);
  Serial0.setRxBufferSize(1024);
  Serial0.begin(115200);
}

// Would `len` bytes go into the transmit buffer without waiting? A full
// buffer does not report 0: availableForWrite() then gives the room in the
// hardware FIFO, so anything up to that size counts as full.
inline bool uartLogHasRoom(size_t len) {
  size_t room = (size_t)Serial0.availableForWrite();
  return room > SOC_UART_FIFO_LEN && room >= len + UART_LOG_SLACK;
}

// Before uartLogBegin() there is no room, so early lines are dropped too.
inline void uartLogWrite(const char* data, size_t len) {
  if (data == nullptr || len == 0) return;
  if (!uartLogHasRoom(len)) return;
  Serial0.write(reinterpret_cast<const uint8_t*>(data), len);
}
