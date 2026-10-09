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
//   6    hallEdgesLastRev u8      entering-edges in last completed rev (1=OK;
//                                 0 = no completed rev measured yet — a
//                                 sentinel, not an anomaly, #418)
//   7..8 dutyWindow       u16 LE  moves in a rolling ~60s window
//   9    statusBits       u8      bit0 last-move stall/jam; bits1-3 TWI
//                                 self-heals since boot that freed the bus (#489,
//                                 saturating at 7); bits4-7 reserved
//   10   checksum         u8      XOR of 0..9 ^ EXT_DIAG_REPLY_CHECKSUM_MASK
//
// Link-health extension (#502): 10 more bytes AFTER the checksum above, so a
// master that reads EXT_DIAG_REPLY_LEN bytes sees the unchanged base packet
// and one that reads EXT_DIAG_LINK_REPLY_LEN gets both, each with its own
// checksum.
//   11..14 uptimeSeconds  u32 LE  seconds since boot (GET_STATUS's u16
//                                 saturates at 18 h 12 min); lags by the
//                                 length of a blocking move
//   15..16 rxFrames       u16 LE  master writes this unit received since boot,
//                                 wrapping — a delta of 0 across a window in
//                                 which the master wrote means the unit was deaf
//   17..18 txReplies      u16 LE  master reads this unit answered, wrapping;
//                                 counts the read carrying this packet. Both
//                                 counters are sampled as the reply is sent
//   19     deafHeals      u8      TWI register self-check re-inits since boot
//                                 (UnitTwiHeal.h), saturating
//   20     checksum       u8      XOR of 11..19 ^ EXT_DIAG_LINK_CHECKSUM_MASK
// A unit without the extension stops driving after byte 10, so the master
// clocks in bus padding (0xFF) and the masked checksum rejects it.
//
// Backward compat (#231/#106 pattern): a pre-ext-diag unit answers the unknown
// opcode with its 1-byte status reply + bus padding; the masked checksum
// rejects all-0xFF, all-0x00 and repeated-status garbage, so the master
// degrades to diagExt=false and emits no ext fields — never a phantom reading.

#include <stdint.h>

#define EXT_DIAG_REPLY_LEN            11
#define EXT_DIAG_REPLY_CHECKSUM_MASK  0x93
#define EXT_DIAG_STATUS_STALL         (1 << 0)
#define EXT_DIAG_STATUS_TWI_HEAL_SHIFT 1
#define EXT_DIAG_STATUS_TWI_HEAL_MASK  (0x07 << EXT_DIAG_STATUS_TWI_HEAL_SHIFT)
// The last move was held back for a low supply before it started (#505).
#define EXT_DIAG_STATUS_SUPPLY_WAIT   (1 << 4)

// #489: replaces bits1-3 of statusBits with the (saturated) self-heal count.
inline uint8_t extDiagWithTwiHeal(uint8_t statusBits, uint8_t count) {
  uint8_t c = count > 7 ? 7 : count;
  return (uint8_t)((statusBits & ~EXT_DIAG_STATUS_TWI_HEAL_MASK) |
                   (c << EXT_DIAG_STATUS_TWI_HEAL_SHIFT));
}

inline uint8_t extDiagTwiHealCount(uint8_t statusBits) {
  return (uint8_t)((statusBits & EXT_DIAG_STATUS_TWI_HEAL_MASK) >>
                   EXT_DIAG_STATUS_TWI_HEAL_SHIFT);
}

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

#define EXT_DIAG_LINK_EXT_LEN         10
#define EXT_DIAG_LINK_REPLY_LEN       (EXT_DIAG_REPLY_LEN + EXT_DIAG_LINK_EXT_LEN)
#define EXT_DIAG_LINK_CHECKSUM_MASK   0xC6

struct UnitLinkStats {
  uint32_t uptimeSeconds = 0;
  uint16_t rxFrames = 0;
  uint16_t txReplies = 0;
  uint8_t  deafHeals = 0;
};

// `ext` points at the extension's first byte (offset EXT_DIAG_REPLY_LEN of the
// full reply).
inline uint8_t extDiagLinkChecksum(const uint8_t ext[EXT_DIAG_LINK_EXT_LEN]) {
  uint8_t x = 0;
  for (uint8_t i = 0; i < EXT_DIAG_LINK_EXT_LEN - 1; i++) x ^= ext[i];
  return (uint8_t)(x ^ EXT_DIAG_LINK_CHECKSUM_MASK);
}

inline void extDiagLinkEncode(const UnitLinkStats& l,
                              uint8_t ext[EXT_DIAG_LINK_EXT_LEN]) {
  ext[0] = (uint8_t)(l.uptimeSeconds & 0xFF);
  ext[1] = (uint8_t)((l.uptimeSeconds >> 8) & 0xFF);
  ext[2] = (uint8_t)((l.uptimeSeconds >> 16) & 0xFF);
  ext[3] = (uint8_t)((l.uptimeSeconds >> 24) & 0xFF);
  ext[4] = (uint8_t)(l.rxFrames & 0xFF);
  ext[5] = (uint8_t)((l.rxFrames >> 8) & 0xFF);
  ext[6] = (uint8_t)(l.txReplies & 0xFF);
  ext[7] = (uint8_t)((l.txReplies >> 8) & 0xFF);
  ext[8] = l.deafHeals;
  ext[9] = extDiagLinkChecksum(ext);
}

// `out` is left untouched on rejection.
inline bool extDiagLinkReadbackValid(const uint8_t ext[EXT_DIAG_LINK_EXT_LEN],
                                     UnitLinkStats& out) {
  if (ext[EXT_DIAG_LINK_EXT_LEN - 1] != extDiagLinkChecksum(ext)) return false;
  out.uptimeSeconds = (uint32_t)ext[0] | ((uint32_t)ext[1] << 8) |
                      ((uint32_t)ext[2] << 16) | ((uint32_t)ext[3] << 24);
  out.rxFrames = (uint16_t)ext[4] | ((uint16_t)ext[5] << 8);
  out.txReplies = (uint16_t)ext[6] | ((uint16_t)ext[7] << 8);
  out.deafHeals = ext[8];
  return true;
}
