#pragma once
// BootDump.h — pure half of the unit boot-section dump (#511, stage 1 of the
// in-system twiboot update #499): the result slot, the CRC and the
// /unit/boot-dump-result JSON. The bus work is unitBusReadBootSection()
// (UnitBus.cpp), run by displayTask; the dumped bytes live in a store of
// their own (Tasks.h), not in the mutex-copied snapshot.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ATmega328P with BOOTSZ = 512 words (HFUSE 0xDC, UnitBootloader/Makefile).
#define BOOT_SECTION_START 0x7C00
#define BOOT_SECTION_LEN   1024

// Worst case: the fixed keys plus two hex characters per byte.
#define BOOT_DUMP_JSON_CAP (2 * BOOT_SECTION_LEN + 128)

enum class BootDumpOutcome : uint8_t {
  Pending = 0,       // slot default; a real result always overwrites it
  Ok,
  EnterFail,         // the unit did not ACK the enter-bootloader command
  BootloaderSilent,  // twiboot never answered at the unit's address
  ChipMismatch,      // chipinfo is not an ATmega328P with 128-byte pages
  ReadFail,          // a page read came back short
};

struct BootDumpSlot {
  uint32_t seq = 0;
  uint8_t addr = 0;
  BootDumpOutcome outcome = BootDumpOutcome::Pending;
  uint32_t crc32 = 0;  // of the BOOT_SECTION_LEN bytes; valid when Ok
};

inline const char* bootDumpOutcomeName(BootDumpOutcome o) {
  switch (o) {
    case BootDumpOutcome::Ok:               return "ok";
    case BootDumpOutcome::EnterFail:        return "enter-fail";
    case BootDumpOutcome::BootloaderSilent: return "bootloader-silent";
    case BootDumpOutcome::ChipMismatch:     return "chip-mismatch";
    case BootDumpOutcome::ReadFail:         return "read-fail";
    default:                                return "pending";
  }
}

// The zlib/PNG CRC-32 (reflected 0xEDB88320), bitwise: 1 KB once per dump
// does not earn a table.
inline uint32_t bootDumpCrc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1)));
    }
  }
  return ~crc;
}

// Renders the result for a queried seq — the same pending / found / expired
// ordering as the other op-result slots. `bytes` is the store's copy for
// THIS seq, or null when the store no longer holds it. Never writes past
// `cap`; returns the string length.
inline size_t buildBootDumpJson(char* buf, size_t cap, const BootDumpSlot& slot,
                                uint32_t seq, const uint8_t* bytes) {
  if (cap == 0) return 0;
  int n;
  if (slot.seq < seq ||
      (slot.seq == seq && slot.outcome == BootDumpOutcome::Pending)) {
    n = snprintf(buf, cap, "{\"state\":\"pending\"}");
  } else if (slot.seq > seq ||
             (slot.outcome == BootDumpOutcome::Ok && bytes == nullptr)) {
    n = snprintf(buf, cap, "{\"state\":\"expired\"}");
  } else if (slot.outcome != BootDumpOutcome::Ok) {
    n = snprintf(buf, cap, "{\"state\":\"failed\",\"addr\":%u,\"reason\":\"%s\"}",
                 (unsigned)slot.addr, bootDumpOutcomeName(slot.outcome));
  } else {
    n = snprintf(buf, cap,
                 "{\"state\":\"ok\",\"addr\":%u,\"start\":%u,\"len\":%u,"
                 "\"crc32\":\"%08lx\",\"hex\":\"",
                 (unsigned)slot.addr, (unsigned)BOOT_SECTION_START,
                 (unsigned)BOOT_SECTION_LEN, (unsigned long)slot.crc32);
    if (n > 0 && (size_t)n + 2 * BOOT_SECTION_LEN + 3 <= cap) {
      static const char digits[] = "0123456789abcdef";
      char* p = buf + n;
      for (int i = 0; i < BOOT_SECTION_LEN; i++) {
        *p++ = digits[bytes[i] >> 4];
        *p++ = digits[bytes[i] & 0x0F];
      }
      *p++ = '"';
      *p++ = '}';
      *p = '\0';
      return (size_t)(p - buf);
    }
    n = snprintf(buf, cap, "{}");  // caller's buffer cannot hold the dump
  }
  if (n < 0) { buf[0] = '\0'; return 0; }
  if ((size_t)n >= cap) n = (int)cap - 1;  // snprintf truncated
  return (size_t)n;
}
