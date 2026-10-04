#pragma once

#include <stdint.h>

#include "BootSectionClassify.h"  // BootSectionState vocabulary

// GET_BOOT_INFO (0x8B) reply codec and BOOT_UPDATE (0x9A) result vocabulary for
// the in-system twiboot update (#499). Pure logic, header-only, natively tested
// (test_boot_report): the unit encodes, both masters (S3 + ESP-01 follower)
// decode. Encode side lives here once so the format can never be defined twice.
//
// The reply carries the facts a master needs to decide whether an update is
// safe and to confirm one afterwards: lock + fuse bytes (readable only by
// application code — twiboot cannot report them, which is why this op exists,
// closing #502 item 8), the boot-section CRC32, the classified state, and the
// last update result.

// Result of the most recent BOOT_UPDATE the unit attempted (0 = never tried).
enum BootUpdateResult {
  BOOT_RESULT_NONE = 0,
  BOOT_RESULT_STAGE1_OK,       // never reported: stage 1 restarts the unit and
                               // the result does not survive; masters read
                               // state Page7Installed instead. Value reserved.
  BOOT_RESULT_STAGE2_OK,       // pages 0-6 rewritten + whole-section CRC verified
  BOOT_RESULT_REFUSED_STATE,   // wrong boot-section state for the requested stage
  BOOT_RESULT_REFUSED_LOCK,    // lock bits forbid boot-section self-program
  BOOT_RESULT_REFUSED_BUSY,    // drum moving / not homed
  BOOT_RESULT_VERIFY_FAILED,   // read-back after write did not match the target
  BOOT_RESULT_COUNT            // keep last — range-check bound
};

#define BOOT_INFO_REPLY_LEN     11
// Bit 7 of the result byte: the unit could not read its lock and fuse bytes,
// so the four bytes in the reply are placeholders, not values (#518). A master
// predating the flag rejects such a report on its result range check — it
// fails closed rather than presenting placeholders as fuses.
#define BOOT_INFO_FLAG_LOCKFUSE_UNREADABLE 0x80
// Distinct from the other reply masks (odometer ^0xA5, diag ^0xB7, ext ^0x93,
// lifetime ^0x6E) so a mis-routed reply of another shape fails the checksum.
#define BOOT_INFO_CHECKSUM_MASK 0x5A

struct BootUpdateReport {
  uint8_t lockByte = 0xFF;
  uint8_t fuseLow = 0xFF;
  uint8_t fuseHigh = 0xFF;
  uint8_t fuseExt = 0xFF;
  bool lockFuseReadable = true;  // false: the four bytes above mean nothing
  uint32_t bootCrc32 = 0;
  uint8_t state = BOOT_STATE_UNKNOWN;   // BootSectionState
  uint8_t lastResult = BOOT_RESULT_NONE;  // BootUpdateResult
};

// Byte layout: [0] lock, [1..3] fuse low/high/ext, [4..7] bootCrc32 LE,
// [8] state, [9] lastResult, [10] XOR(buf[0..9]) ^ mask.
inline void bootInfoEncode(const BootUpdateReport& r,
                           uint8_t buf[BOOT_INFO_REPLY_LEN]) {
  buf[0] = r.lockByte;
  buf[1] = r.fuseLow;
  buf[2] = r.fuseHigh;
  buf[3] = r.fuseExt;
  buf[4] = (uint8_t)(r.bootCrc32 & 0xFF);
  buf[5] = (uint8_t)((r.bootCrc32 >> 8) & 0xFF);
  buf[6] = (uint8_t)((r.bootCrc32 >> 16) & 0xFF);
  buf[7] = (uint8_t)((r.bootCrc32 >> 24) & 0xFF);
  buf[8] = r.state;
  buf[9] = (uint8_t)(r.lastResult |
                     (r.lockFuseReadable ? 0 : BOOT_INFO_FLAG_LOCKFUSE_UNREADABLE));
  uint8_t x = 0;
  for (uint8_t i = 0; i < BOOT_INFO_REPLY_LEN - 1; i++) x ^= buf[i];
  buf[BOOT_INFO_REPLY_LEN - 1] = (uint8_t)(x ^ BOOT_INFO_CHECKSUM_MASK);
}

// Validates checksum + the enum ranges, then decodes. Rejects the all-0xFF /
// old-firmware-garbage patterns because an impossible state or result there will
// not survive the range check even if a glitch happened to satisfy the XOR.
inline bool bootInfoDecode(const uint8_t buf[BOOT_INFO_REPLY_LEN],
                           BootUpdateReport& out) {
  uint8_t x = 0;
  for (uint8_t i = 0; i < BOOT_INFO_REPLY_LEN - 1; i++) x ^= buf[i];
  if (buf[BOOT_INFO_REPLY_LEN - 1] != (uint8_t)(x ^ BOOT_INFO_CHECKSUM_MASK)) {
    return false;
  }
  if (buf[8] > BOOT_STATE_LAST) return false;           // BootSectionState range
  uint8_t result = (uint8_t)(buf[9] & ~BOOT_INFO_FLAG_LOCKFUSE_UNREADABLE);
  if (result >= BOOT_RESULT_COUNT) return false;       // BootUpdateResult range
  out.lockByte = buf[0];
  out.fuseLow = buf[1];
  out.fuseHigh = buf[2];
  out.fuseExt = buf[3];
  out.bootCrc32 = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) |
                  ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
  out.state = buf[8];
  out.lastResult = result;
  out.lockFuseReadable = (buf[9] & BOOT_INFO_FLAG_LOCKFUSE_UNREADABLE) == 0;
  return true;
}

// Some of the fielded Nanos do not serve lock and fuse bytes to application
// code: boot_lock_fuse_bits_get() runs there as a plain lpm and returns the
// sketch's own flash bytes at the Z addresses it used — low fuse = Z 0,
// lock = Z 1, extended fuse = Z 2, high fuse = Z 3 (#518: 11 of 21 units,
// repeatable per unit). Those four flash bytes are the reset vector, so a
// genuine read equal to all four at once is not a configuration a running unit
// can have. `flash0to3` = the sketch's flash bytes 0..3.
inline bool bootLockFuseReadFellThrough(uint8_t lockByte, uint8_t fuseLow,
                                        uint8_t fuseHigh, uint8_t fuseExt,
                                        const uint8_t flash0to3[4]) {
  return fuseLow == flash0to3[0] && lockByte == flash0to3[1] &&
         fuseExt == flash0to3[2] && fuseHigh == flash0to3[3];
}

// The lock byte a stage is gated on. An unreadable lock is treated as open:
// refusing would block the update on every unit of that population for a
// reason nobody can check, and a write against a boot section that really is
// locked changes nothing and is caught by the read-back verification.
inline uint8_t bootEffectiveLockByte(const BootUpdateReport& r) {
  return r.lockFuseReadable ? r.lockByte : 0xFF;
}
