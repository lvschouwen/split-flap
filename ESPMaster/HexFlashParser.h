// Intel-HEX -> page-buffer assembler for twiboot-style flashing.
//
// Header-only, no Wire dependency. Production code in
// ServiceFirmwareFunctions.ino wraps an HexFlashState and passes a
// real Wire-backed flush callback; host-side tests wrap the same state
// with an in-memory capture callback.
//
// Usage (production):
//
//   static HexFlashState g_flash;
//
//   static bool producePage(const uint8_t* page, uint16_t addr, void*) {
//     // stream `page` (128 bytes) to twiboot at flash address `addr`
//     ...
//   }
//
//   hexInitState(g_flash, TWIBOOT_APP_MAX);
//   hexFeedChunk(g_flash, chunk, chunkLen, producePage, nullptr);
//   ...
//   hexFlushFinal(g_flash, producePage, nullptr);

#pragma once

#include <Arduino.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#define HEX_PAGE_SIZE           128
#define HEX_APP_MAX_DEFAULT     0x7C00
#define HEX_MAX_BYTES_DEFAULT   (64UL * 1024UL)
#define HEX_LINE_BUF_SIZE       64

// Callback invoked once per completed 128-byte page. Returning false
// propagates as a parse failure; the parser stops and sets errorMsg.
typedef bool (*HexPageFlushFn)(const uint8_t* page, uint16_t addr, void* userCtx);

struct HexFlashState {
  uint8_t  pageBuf[HEX_PAGE_SIZE];
  uint16_t currentPageAddr;
  bool     pageHasData;

  char     lineBuf[HEX_LINE_BUF_SIZE];
  int      lineLen;

  uint32_t totalHexBytesSeen;
  uint32_t totalBytesWritten;
  uint32_t maxHexBytes;
  uint16_t appMax;

  String   errorMsg;
};

inline void hexInitState(HexFlashState& s,
                         uint16_t appMax = HEX_APP_MAX_DEFAULT,
                         uint32_t maxHexBytes = HEX_MAX_BYTES_DEFAULT) {
  memset(s.pageBuf, 0xFF, HEX_PAGE_SIZE);
  s.currentPageAddr   = 0;
  s.pageHasData       = false;
  s.lineLen           = 0;
  s.totalHexBytesSeen = 0;
  s.totalBytesWritten = 0;
  s.maxHexBytes       = maxHexBytes;
  s.appMax            = appMax;
  s.errorMsg          = "";
}

inline int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

inline int hexByte(char hi, char lo) {
  int h = hexNibble(hi), l = hexNibble(lo);
  if (h < 0 || l < 0) return -1;
  return (h << 4) | l;
}

// Flush the currently-buffered page (if any) via the callback. Resets
// the page buffer afterwards. Returns false on flush error (errorMsg set).
inline bool hexFlushPage(HexFlashState& s, HexPageFlushFn flushFn, void* ctx) {
  if (!s.pageHasData) return true;
  if (!flushFn(s.pageBuf, s.currentPageAddr, ctx)) {
    if (s.errorMsg.length() == 0) {
      s.errorMsg = "Page flush callback failed";
    }
    return false;
  }
  s.totalBytesWritten += HEX_PAGE_SIZE;
  memset(s.pageBuf, 0xFF, HEX_PAGE_SIZE);
  s.pageHasData = false;
  return true;
}

// Parse one complete Intel-HEX record (without the leading whitespace).
// Returns true on parse OK (including EOF record), false on error.
inline bool hexParseLine(HexFlashState& s, const char* line, int len,
                         HexPageFlushFn flushFn, void* ctx) {
  if (len < 11 || line[0] != ':') {
    s.errorMsg = "Invalid HEX line";
    return false;
  }

  int byteCount  = hexByte(line[1], line[2]);
  int addrHi     = hexByte(line[3], line[4]);
  int addrLo     = hexByte(line[5], line[6]);
  int recordType = hexByte(line[7], line[8]);
  if (byteCount < 0 || addrHi < 0 || addrLo < 0 || recordType < 0) {
    s.errorMsg = "Malformed HEX header";
    return false;
  }

  uint16_t baseAddr = ((uint16_t)addrHi << 8) | addrLo;

  if (recordType == 0x01) return true;      // EOF
  if (recordType != 0x00) return true;      // 02/04 extended-address, ignored for 32KB targets

  if (len < 9 + byteCount * 2 + 2) {
    s.errorMsg = "HEX data record truncated";
    return false;
  }

  for (int i = 0; i < byteCount; i++) {
    int b = hexByte(line[9 + i * 2], line[9 + i * 2 + 1]);
    if (b < 0) {
      s.errorMsg = "Bad HEX data byte";
      return false;
    }

    uint16_t addr = baseAddr + i;
    if (addr >= s.appMax) {
      s.errorMsg = String("Address 0x") + String(addr, HEX) + " past app section";
      return false;
    }

    uint16_t pageBase   = addr & ~((uint16_t)HEX_PAGE_SIZE - 1);
    uint8_t  pageOffset = addr & (HEX_PAGE_SIZE - 1);

    if (s.pageHasData && pageBase != s.currentPageAddr) {
      if (!hexFlushPage(s, flushFn, ctx)) return false;
    }

    s.currentPageAddr   = pageBase;
    s.pageBuf[pageOffset] = (uint8_t)b;
    s.pageHasData       = true;
  }

  return true;
}

// Feed a chunk of bytes from the uploaded HEX file. Accumulates into
// line buffer, dispatches to hexParseLine on each newline.
inline bool hexFeedChunk(HexFlashState& s, const uint8_t* data, size_t len,
                         HexPageFlushFn flushFn, void* ctx) {
  s.totalHexBytesSeen += len;
  if (s.totalHexBytesSeen > s.maxHexBytes) {
    s.errorMsg = "HEX upload exceeded size limit";
    return false;
  }

  for (size_t i = 0; i < len; i++) {
    char c = (char)data[i];
    if (c == '\n' || c == '\r') {
      if (s.lineLen > 0) {
        bool ok = hexParseLine(s, s.lineBuf, s.lineLen, flushFn, ctx);
        s.lineLen = 0;
        if (!ok) return false;
      }
    } else {
      if (s.lineLen < HEX_LINE_BUF_SIZE - 1) {
        s.lineBuf[s.lineLen++] = c;
      } else {
        s.errorMsg = "HEX line exceeds internal buffer";
        return false;
      }
    }
  }
  return true;
}

// Flush any trailing partial page. Call once after feeding the whole
// HEX file (or its EOF record) to make sure the final page is written.
inline bool hexFlushFinal(HexFlashState& s, HexPageFlushFn flushFn, void* ctx) {
  return hexFlushPage(s, flushFn, ctx);
}
