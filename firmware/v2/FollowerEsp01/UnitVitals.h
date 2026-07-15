#pragma once
// Pure logic for the CMD_GET_VITALS reply (#306) — the unit's supply-voltage,
// commanded-position and free-RAM diagnostics. Natively tested by test_vitals;
// the AVR ADC/free-RAM glue that FEEDS it lives in the .ino files (bench tier).
//
// SHARED header: copied verbatim into firmware/v2/Master and
// firmware/v2/FollowerEsp01 so both master generations decode the same packet.
// Fix bugs in ALL three trees (copy policy).
//
// Wire format (SFP_CMD_GET_VITALS reply, 8 bytes):
//   offset field       type    notes
//   0..1   vccNow_mV   u16 LE  AVR 1.1V-bandgap read, current rail
//   2..3   vccMin_mV   u16 LE  since-boot minimum (sampled mid-move)
//   4      cmdPos      u8      last commanded flap index (0..FLAP_AMOUNT-1)
//   5..6   freeRamMin  u16 LE  since-boot minimum free SRAM (bytes)
//   7      checksum    u8      XOR of bytes 0..6 ^ VITALS_REPLY_CHECKSUM_MASK
//
// Backward compatibility (the #231/#106 pattern): a unit on pre-vitals
// firmware answers the unknown opcode with its 1-byte status reply and bus
// padding (0xFF from an un-ACKed read). The masked checksum rejects all-0xFF,
// all-0x00 and repeated-status garbage, so the reader degrades to
// diagV2=false and emits no Vcc fields — never a phantom reading.

#include <stdint.h>

#define VITALS_REPLY_LEN            8
#define VITALS_REPLY_CHECKSUM_MASK  0x3C

// Vcc from the AVR internal 1.1V bandgap measured against AVcc:
//   ADMUX = (1<<REFS0) | 0b1110  routes the 1.1V ref into the ADC.
//   Vcc_mV = 1100 * 1023 / ADC.
// ADC is never 0 in practice (the bandgap is ~1.1V, not 0V) — guard it so a
// glitched 0 can't divide-by-zero, and clamp the degenerate low end to u16.
inline uint16_t unitVccFromAdc(uint16_t adc) {
  if (adc == 0) return 0;
  uint32_t mv = 1100UL * 1023UL / (uint32_t)adc;
  return (mv > 0xFFFFUL) ? 0xFFFF : (uint16_t)mv;
}

struct UnitVitals {
  uint16_t vccNow_mV = 0;
  uint16_t vccMin_mV = 0;
  uint8_t  cmdPos = 0;
  uint16_t freeRamMin = 0;
};

inline uint8_t vitalsChecksum(const uint8_t buf[VITALS_REPLY_LEN]) {
  uint8_t x = 0;
  for (uint8_t i = 0; i < VITALS_REPLY_LEN - 1; i++) x ^= buf[i];
  return (uint8_t)(x ^ VITALS_REPLY_CHECKSUM_MASK);
}

inline void vitalsEncodeReply(const UnitVitals& v,
                              uint8_t buf[VITALS_REPLY_LEN]) {
  buf[0] = (uint8_t)(v.vccNow_mV & 0xFF);
  buf[1] = (uint8_t)((v.vccNow_mV >> 8) & 0xFF);
  buf[2] = (uint8_t)(v.vccMin_mV & 0xFF);
  buf[3] = (uint8_t)((v.vccMin_mV >> 8) & 0xFF);
  buf[4] = v.cmdPos;
  buf[5] = (uint8_t)(v.freeRamMin & 0xFF);
  buf[6] = (uint8_t)((v.freeRamMin >> 8) & 0xFF);
  buf[7] = vitalsChecksum(buf);
}

// Validates + decodes the 8-byte vitals reply. false → checksum mismatch
// (unknown-opcode garbage / pre-vitals firmware) → caller sets diagV2=false.
inline bool vitalsReadbackValid(const uint8_t buf[VITALS_REPLY_LEN],
                                UnitVitals& out) {
  if (buf[VITALS_REPLY_LEN - 1] != vitalsChecksum(buf)) return false;
  out.vccNow_mV = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
  out.vccMin_mV = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
  out.cmdPos = buf[4];
  out.freeRamMin = (uint16_t)buf[5] | ((uint16_t)buf[6] << 8);
  return true;
}
