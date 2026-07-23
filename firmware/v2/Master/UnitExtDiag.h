#pragma once
// Pure logic for the SFP_CMD_GET_EXT_DIAG reply (#365) — the unit's
// new-measurement diagnostics that GET_STATUS/GET_VITALS don't already carry.
// Natively tested by test_ext_diag. AVR glue that FEEDS it lives in the .ino
// files (bench tier).
//
// SHARED header: copied verbatim into Unit, Master and FollowerEsp01. Fix bugs
// in ALL three trees (copy policy; tests/test_copied_headers.py gates it).
//
// Wire format (SFP_CMD_GET_EXT_DIAG reply, 11 bytes):
//   off  field            type    notes
//   0..1 stepExcessLast   u16 LE  last home: actual - geometry-expected steps
//   2..3 stepExcessMax    u16 LE  worst-seen excess since boot (drag alarm)
//   4..5 vccSagLastMove   u16 LE  min Vcc during last move (mV)
//   6    hallEdgesLastRev u8      entering-edges in last completed rev (1=OK)
//   7..8 dutyWindow       u16 LE  moves in a rolling ~60s window
//   9    statusBits       u8      bit0 last-move stall/jam; bits1-7 reserved
//   10   checksum         u8      XOR of 0..9 ^ EXT_DIAG_REPLY_CHECKSUM_MASK
//
// Backward compat (#231/#106 pattern): a pre-ext-diag unit answers the unknown
// opcode with its 1-byte status reply + bus padding; the masked checksum
// rejects all-0xFF, all-0x00 and repeated-status garbage, so the master
// degrades to diagExt=false and emits no ext fields — never a phantom reading.

#include <stdint.h>

#define EXT_DIAG_REPLY_LEN            11
#define EXT_DIAG_REPLY_CHECKSUM_MASK  0x93
#define EXT_DIAG_STATUS_STALL         (1 << 0)

struct UnitExtDiag {
  uint16_t stepExcessLast = 0;
  uint16_t stepExcessMax = 0;
  uint16_t vccSagLastMove = 0;
  uint8_t  hallEdgesLastRev = 0;
  uint16_t dutyWindow = 0;
  uint8_t  statusBits = 0;
};

inline uint8_t extDiagChecksum(const uint8_t buf[EXT_DIAG_REPLY_LEN]) {
  uint8_t x = 0;
  for (uint8_t i = 0; i < EXT_DIAG_REPLY_LEN - 1; i++) x ^= buf[i];
  return (uint8_t)(x ^ EXT_DIAG_REPLY_CHECKSUM_MASK);
}

inline void extDiagEncodeReply(const UnitExtDiag& d, uint8_t buf[EXT_DIAG_REPLY_LEN]) {
  buf[0] = (uint8_t)(d.stepExcessLast & 0xFF);
  buf[1] = (uint8_t)((d.stepExcessLast >> 8) & 0xFF);
  buf[2] = (uint8_t)(d.stepExcessMax & 0xFF);
  buf[3] = (uint8_t)((d.stepExcessMax >> 8) & 0xFF);
  buf[4] = (uint8_t)(d.vccSagLastMove & 0xFF);
  buf[5] = (uint8_t)((d.vccSagLastMove >> 8) & 0xFF);
  buf[6] = d.hallEdgesLastRev;
  buf[7] = (uint8_t)(d.dutyWindow & 0xFF);
  buf[8] = (uint8_t)((d.dutyWindow >> 8) & 0xFF);
  buf[9] = d.statusBits;
  buf[10] = extDiagChecksum(buf);
}

inline bool extDiagReadbackValid(const uint8_t buf[EXT_DIAG_REPLY_LEN], UnitExtDiag& out) {
  if (buf[EXT_DIAG_REPLY_LEN - 1] != extDiagChecksum(buf)) return false;
  out.stepExcessLast = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
  out.stepExcessMax  = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
  out.vccSagLastMove = (uint16_t)buf[4] | ((uint16_t)buf[5] << 8);
  out.hallEdgesLastRev = buf[6];
  out.dutyWindow = (uint16_t)buf[7] | ((uint16_t)buf[8] << 8);
  out.statusBits = buf[9];
  return true;
}
