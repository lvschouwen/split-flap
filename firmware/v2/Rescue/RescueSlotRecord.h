#pragma once
// RescueSlotRecord.h — parse-only copy of Master's SlotRecord.h (#200), the
// per-OTA-slot NVS records ("1|<seq>|<sha256 hex>|<git rev>", keys
// slotRec0/slotRec1 in the shared "splitflap" namespace) that Master stamps
// on netif-up. Rescue ranks /rescue/exit by seq after checking sha against
// the slot's actual image, because the app-descriptor build stamp is frozen
// at framework-assembly time under pioarduino hybrid builds and cannot
// order images. Copy, not a shared include — Master refactors must never
// change a bench-proven rescue image; the format is wire-contract-like, so
// keep parse behavior identical on both sides. Natively tested
// (test_rescue_slot_record).

#include <stdint.h>

#define SLOT_RECORD_SHA_HEX_LEN 64
#define SLOT_RECORD_REV_MAX_LEN 32

struct SlotRecord {
  bool ok = false;
  uint32_t seq = 0;
  char sha[SLOT_RECORD_SHA_HEX_LEN + 1] = {0};
  char rev[SLOT_RECORD_REV_MAX_LEN + 1] = {0};
};

static inline SlotRecord parseSlotRecord(const char* raw) {
  SlotRecord r;
  if (raw == nullptr || raw[0] != '1' || raw[1] != '|') return r;
  const char* p = raw + 2;

  // seq: 1..10 decimal digits, must fit uint32.
  uint64_t seq = 0;
  int digits = 0;
  while (*p >= '0' && *p <= '9') {
    seq = seq * 10 + (*p - '0');
    if (++digits > 10 || seq > 0xFFFFFFFFull) return r;
    p++;
  }
  if (digits == 0 || *p != '|') return r;
  p++;

  // sha: exactly 64 lowercase hex chars (what Master's formatSlotRecord emits).
  for (int i = 0; i < SLOT_RECORD_SHA_HEX_LEN; i++) {
    char c = p[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return r;
    r.sha[i] = c;
  }
  p += SLOT_RECORD_SHA_HEX_LEN;
  if (*p != '|') return r;
  p++;

  // rev: 1..32 printable-ASCII chars, JSON-safe, no separator.
  int revLen = 0;
  while (*p != '\0') {
    char c = *p;
    if (c < 0x20 || c > 0x7E || c == '"' || c == '\\' || c == '|') return r;
    if (revLen >= SLOT_RECORD_REV_MAX_LEN) return r;
    r.rev[revLen++] = c;
    p++;
  }
  if (revLen == 0) return r;

  r.seq = (uint32_t)seq;
  r.ok = true;
  return r;
}

// Does the record describe this exact image? sha is the 32-byte digest from
// esp_partition_get_sha256 on the slot the record claims to cover.
static inline bool slotRecordShaMatches(const SlotRecord& r,
                                        const uint8_t sha[32]) {
  if (!r.ok || sha == nullptr) return false;
  static const char* HEX_DIGITS = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    if (r.sha[2 * i] != HEX_DIGITS[sha[i] >> 4] ||
        r.sha[2 * i + 1] != HEX_DIGITS[sha[i] & 0x0F]) {
      return false;
    }
  }
  return true;
}
