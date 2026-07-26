#pragma once
// Pure logic for the CORE master<->unit reads and writes (#405) — the ones
// that predate the #231 masked-XOR discipline and are brought into it here.
// Natively tested by test_wire_contract. Wire/EEPROM glue lives in the .ino
// and UnitBus/FollowerBus sources (bench tier).
//
// SHARED header: copied verbatim into Unit, Master and FollowerEsp01. Fix bugs
// in ALL three trees (copy policy; tests/test_copied_headers.py gates it).
//
// The bus is not currently failing — .91 reported i2cErr 0 across 10,604
// transactions. This is hardening, not firefighting: the point is that a
// SILENT corruption on these five paths would not have been detected, which
// makes "0 errors" weaker evidence than it looks.
//
// Two idioms, both already in the protocol; nothing new is invented here.
//   READS   masked XOR + a *ReadbackValid() guard      (the #231 pattern)
//   WRITES  value + bitwise complement in one payload  (the #106 GET_LETTER
//           pattern), so the unit rejects a disagreeing pair instead of
//           persisting a corrupted value
//
// Day 0 (#407): the whole fleet is reflashed in one campaign, so these
// formats replace the old ones outright — no legacy fallback, no mixed-format
// negotiation. The one thing that survives forever is GET_VERSION's shape,
// because it carries the protocol version that gates everything else.

#include <stdint.h>

#include "SplitFlapProtocol.h"

// --- protocol-version gate --------------------------------------------------

// EQUALITY ONLY — never `<` or `>=`. The number increments so its history
// reads in order, but "newer" is not a thing the master can act on: it cannot
// speak a contract it does not have code for, so a higher version is exactly
// as un-drivable as a lower one. The git rev alongside it is a hash and is not
// orderable at all; both answer only "same or different", and different always
// means flash.
inline bool unitProtocolSupported(uint8_t reportedVersion) {
  return reportedVersion == SFP_PROTOCOL_VERSION;
}

// What may still be sent to a unit whose protocol version we do not recognise.
// Exactly the two opcodes SplitFlapProtocol.h documents as FIXED FOREVER: ask
// what it is, and push it into the bootloader so it can be made current.
// Everything else — including a bare letter index, which is a render — is
// blocked, because decoding or driving against an unknown contract is the
// silent-corruption class this file exists to close.
inline bool unitOpcodeAllowedWhenUnsupported(uint8_t opcode) {
  return opcode == SFP_CMD_GET_VERSION || opcode == SFP_CMD_ENTER_BOOTLOADER;
}

// --- GET_VERSION (0x81) — FIXED FOREVER -------------------------------------
// 10 bytes: 8 rev chars (null-padded), protocol version, checksum.
//
// This reply's format can never change again. It is the cross-generation
// identity read AND the carrier of the version gate, so every generation that
// will ever exist has to be able to parse it. A future contract change bumps
// SFP_PROTOCOL_VERSION and changes other opcodes — never this one.
#define VERSION_REV_LEN               8
#define VERSION_REPLY_LEN             10
#define VERSION_REPLY_CHECKSUM_MASK   0x4B

struct UnitVersionPacket {
  char    rev[VERSION_REV_LEN + 1] = {0};  // always NUL-terminated
  uint8_t protocolVersion = 0;
};

inline uint8_t versionChecksum(const uint8_t buf[VERSION_REPLY_LEN]) {
  uint8_t x = 0;
  for (uint8_t i = 0; i < VERSION_REPLY_LEN - 1; i++) x ^= buf[i];
  return (uint8_t)(x ^ VERSION_REPLY_CHECKSUM_MASK);
}

inline void versionEncodeReply(const char* rev, uint8_t protocolVersion,
                               uint8_t buf[VERSION_REPLY_LEN]) {
  for (uint8_t i = 0; i < VERSION_REV_LEN; i++) {
    buf[i] = (rev && rev[i]) ? (uint8_t)rev[i] : 0;
  }
  buf[VERSION_REV_LEN] = protocolVersion;
  buf[VERSION_REPLY_LEN - 1] = versionChecksum(buf);
}

inline bool versionReadbackValid(const uint8_t* buf, uint8_t len,
                                 UnitVersionPacket& out) {
  if (len < VERSION_REPLY_LEN) return false;
  if (buf[VERSION_REPLY_LEN - 1] != versionChecksum(buf)) return false;
  for (uint8_t i = 0; i < VERSION_REV_LEN; i++) out.rev[i] = (char)buf[i];
  out.rev[VERSION_REV_LEN] = '\0';  // a full 8-char rev must still be a string
  out.protocolVersion = buf[VERSION_REV_LEN];
  return true;
}

// --- GET_STATUS (0x83) ------------------------------------------------------
// 9 bytes: the historical 8-byte health payload (v1 #47) + checksum.
//
// The highest-frequency read on the bus and the one with the widest blast
// radius: it feeds fault flags, lifetime brownout/watchdog counts, uptime,
// last-homing step count, UnitEventLog transitions, stale detection and the
// HA sensors. It predated the checksum discipline entirely, so a corrupted
// flags byte could invent a fault or mask a real one and nothing would know.
//
// Kept as raw payload bytes rather than a struct: the unit assembles them in
// its TWI ISR and the master unpacks them into UnitStatus (UnitHealth.h),
// which does not exist in the Unit tree.
#define STATUS_PAYLOAD_LEN            8
#define STATUS_REPLY_LEN              9
#define STATUS_REPLY_CHECKSUM_MASK    0x7E

inline uint8_t statusChecksum(const uint8_t buf[STATUS_REPLY_LEN]) {
  uint8_t x = 0;
  for (uint8_t i = 0; i < STATUS_PAYLOAD_LEN; i++) x ^= buf[i];
  return (uint8_t)(x ^ STATUS_REPLY_CHECKSUM_MASK);
}

inline void statusEncodeReply(const uint8_t payload[STATUS_PAYLOAD_LEN],
                              uint8_t buf[STATUS_REPLY_LEN]) {
  for (uint8_t i = 0; i < STATUS_PAYLOAD_LEN; i++) buf[i] = payload[i];
  buf[STATUS_PAYLOAD_LEN] = statusChecksum(buf);
}

// `out` is left untouched on rejection — a bad read must not half-write the
// caller's fold.
inline bool statusReadbackValid(const uint8_t* buf, uint8_t len,
                                uint8_t out[STATUS_PAYLOAD_LEN]) {
  if (len < STATUS_REPLY_LEN) return false;
  if (buf[STATUS_PAYLOAD_LEN] != statusChecksum(buf)) return false;
  for (uint8_t i = 0; i < STATUS_PAYLOAD_LEN; i++) out[i] = buf[i];
  return true;
}

// --- GET_OFFSET (0x82) ------------------------------------------------------
// 3 bytes: int16 LE calibration offset + checksum.
#define OFFSET_REPLY_LEN              3
#define OFFSET_REPLY_CHECKSUM_MASK    0x1D

inline uint8_t offsetChecksum(const uint8_t buf[OFFSET_REPLY_LEN]) {
  return (uint8_t)((buf[0] ^ buf[1]) ^ OFFSET_REPLY_CHECKSUM_MASK);
}

inline void offsetEncodeReply(int16_t offset, uint8_t buf[OFFSET_REPLY_LEN]) {
  buf[0] = (uint8_t)((uint16_t)offset & 0xFF);
  buf[1] = (uint8_t)(((uint16_t)offset >> 8) & 0xFF);
  buf[2] = offsetChecksum(buf);
}

inline bool offsetReadbackValid(const uint8_t* buf, uint8_t len, int16_t& out) {
  if (len < OFFSET_REPLY_LEN) return false;
  if (buf[2] != offsetChecksum(buf)) return false;
  out = (int16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));
  return true;
}

// --- SET_OFFSET (0x93) ------------------------------------------------------
// 4 bytes: int16 LE value + its bitwise complement, LE. Was fire-and-forget:
// the master wrote and returned the transmission status, with no read-back
// even though GET_OFFSET exists. Range-clamping on both sides bounded the
// damage but detected nothing.
//
// An all-zero payload is correctly rejected — "set offset to 0" carries
// 0x0000 followed by 0xFFFF, so an idle or stuck bus cannot spell it.
#define SET_OFFSET_PAYLOAD_LEN        4

inline void setOffsetEncode(int16_t offset,
                            uint8_t buf[SET_OFFSET_PAYLOAD_LEN]) {
  uint16_t v = (uint16_t)offset;
  uint16_t c = (uint16_t)~v;
  buf[0] = (uint8_t)(v & 0xFF);
  buf[1] = (uint8_t)((v >> 8) & 0xFF);
  buf[2] = (uint8_t)(c & 0xFF);
  buf[3] = (uint8_t)((c >> 8) & 0xFF);
}

// The unit drains SET_OFFSET in its loop(), not its TWI ISR — an immediate
// GET_OFFSET read-back would race the EEPROM write and see the old value.
#define UNIT_OFFSET_WRITE_SETTLE_MS   30

// Master-side results for a write whose read-back did not confirm it. Chosen
// above Wire.endTransmission()'s 0..5 range so a caller grading the status
// cannot confuse them with a bus error.
#define UNIT_BUS_OFFSET_UNVERIFIED    20  // wrote OK, read-back unreadable
#define UNIT_BUS_OFFSET_MISMATCH      21  // wrote OK, unit reports another value

inline bool setOffsetDecode(const uint8_t* buf, uint8_t len, int16_t& out) {
  if (len < SET_OFFSET_PAYLOAD_LEN) return false;
  uint16_t v = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
  uint16_t c = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
  if (c != (uint16_t)~v) return false;
  out = (int16_t)v;
  return true;
}

// --- SET_I2C_ADDRESS (0x94) -------------------------------------------------
// 2 bytes: address + its bitwise complement.
//
// The worst failure mode in the whole contract. One unprotected byte,
// validated only as 1..126, after which the unit persists it and reboots. A
// corruption landing inside that range silently relocates the unit to an
// address nobody is looking at, and it cannot be verified after the fact
// because the unit is gone from the address you were talking to. twiboot
// still listens on the DIP-derived address, so recovery means a physical trip
// to re-DIP.
#define SET_ADDRESS_PAYLOAD_LEN       2

inline void setAddressEncode(uint8_t address,
                             uint8_t buf[SET_ADDRESS_PAYLOAD_LEN]) {
  buf[0] = address;
  buf[1] = (uint8_t)~address;
}

inline bool setAddressDecode(const uint8_t* buf, uint8_t len, uint8_t& out) {
  if (len < SET_ADDRESS_PAYLOAD_LEN) return false;
  if (buf[1] != (uint8_t)~buf[0]) return false;
  out = buf[0];
  return true;
}

// --- SET_GATES (0x99) -------------------------------------------------------
// 2 bytes: the UNIT_GATE_* byte + its bitwise complement (#409).
//
// The write half of #406's feature-gate byte, and the reason #407 can ship its
// two motion changes switched off: without it, enabling one would take a
// second fleet reflash — the exact cost the epic exists to avoid paying twice.
//
// Unlike SET_I2C_ADDRESS this one is fully verifiable: GET_LIFETIME already
// reports the byte under a checksum, so the master writes, waits out the
// unit's loop-context EEPROM drain, reads back and grades the op. Same shape
// as SET_OFFSET's read-back.
//
// Which bits are legal is the UNIT's call, not the master's — the unit
// rejects any bit its own firmware has no code for (UnitEeprom.h), so a
// newer master cannot talk an older unit into persisting a gate it will
// never act on.
#define SET_GATES_PAYLOAD_LEN         2

inline void setGatesEncode(uint8_t gates, uint8_t buf[SET_GATES_PAYLOAD_LEN]) {
  buf[0] = gates;
  buf[1] = (uint8_t)~gates;
}

inline bool setGatesDecode(const uint8_t* buf, uint8_t len, uint8_t& out) {
  if (len < SET_GATES_PAYLOAD_LEN) return false;
  if (buf[1] != (uint8_t)~buf[0]) return false;
  out = buf[0];
  return true;
}

// The unit persists in loop(), not the TWI ISR — an immediate read-back would
// race the EEPROM write. Same reasoning and the same order of magnitude as
// UNIT_OFFSET_WRITE_SETTLE_MS; the health block is one byte wider.
#define UNIT_GATES_WRITE_SETTLE_MS    30

#define UNIT_BUS_GATES_UNVERIFIED     22  // wrote OK, read-back unreadable
#define UNIT_BUS_GATES_MISMATCH       23  // wrote OK, unit reports other bits
