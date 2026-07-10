#pragma once
// SlotRecord.h — pure format/parse for the per-OTA-slot NVS records that fix
// #200. On netif-up Master stamps the running slot's record
// ("1|<seq>|<sha256 hex>|<git rev>", NVS keys slotRec0/slotRec1 in the
// shared "splitflap" namespace); Rescue ranks /rescue/exit by seq after
// checking sha against the slot's actual image, because the app-descriptor
// build stamp is frozen at framework-assembly time under pioarduino hybrid
// builds and cannot order images. Rescue parses these from a separately
// compiled image, so the format is wire-contract-like: versioned and
// strictly validated. RescueSlotRecord.h in firmware/v2/Rescue is the
// parse-only copy — keep behavior identical. Natively tested
// (test_slot_record).

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define SLOT_RECORD_SHA_HEX_LEN 64
#define SLOT_RECORD_REV_MAX_LEN 32

// "1|" + 10-digit seq + "|" + sha + "|" + rev + NUL — derived from the field
// constants so resizing a field can't silently outgrow callers' buffers.
#define SLOT_RECORD_BUF_LEN \
  (2 + 10 + 1 + SLOT_RECORD_SHA_HEX_LEN + 1 + SLOT_RECORD_REV_MAX_LEN + 1)

struct SlotRecord {
  bool ok = false;
  uint32_t seq = 0;
  char sha[SLOT_RECORD_SHA_HEX_LEN + 1] = {0};
  char rev[SLOT_RECORD_REV_MAX_LEN + 1] = {0};
};

// rev longer than SLOT_RECORD_REV_MAX_LEN is clamped (display-only field);
// characters outside printable ASCII or in {", \, |} are dropped so the
// stored record always round-trips through parse and JSON output.
static inline bool formatSlotRecord(char* buf, size_t bufLen, uint32_t seq,
                                    const uint8_t sha[32], const char* rev) {
  if (buf == nullptr || bufLen < SLOT_RECORD_BUF_LEN || sha == nullptr ||
      rev == nullptr) {
    return false;
  }

  int n = snprintf(buf, bufLen, "1|%lu|", (unsigned long)seq);

  static const char* HEX_DIGITS = "0123456789abcdef";  // HEX is an Arduino macro
  for (int i = 0; i < 32; i++) {
    buf[n++] = HEX_DIGITS[sha[i] >> 4];
    buf[n++] = HEX_DIGITS[sha[i] & 0x0F];
  }
  buf[n++] = '|';

  int revLen = 0;
  for (const char* p = rev; *p != '\0' && revLen < SLOT_RECORD_REV_MAX_LEN;
       p++) {
    char c = *p;
    if (c < 0x20 || c > 0x7E || c == '"' || c == '\\' || c == '|') continue;
    buf[n++] = c;
    revLen++;
  }
  if (revLen == 0) buf[n++] = '?';  // parse requires a non-empty rev
  buf[n] = '\0';
  return true;
}

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

  // sha: exactly 64 lowercase hex chars (what formatSlotRecord emits).
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

// The next confirm sequence: one past the highest recorded, 1 on a fresh
// device. Both records count even when their sha no longer matches the slot
// — monotonicity must survive reflashes.
static inline uint32_t nextSlotRecordSeq(const SlotRecord& a,
                                         const SlotRecord& b) {
  uint32_t maxSeq = 0;
  if (a.ok && a.seq > maxSeq) maxSeq = a.seq;
  if (b.ok && b.seq > maxSeq) maxSeq = b.seq;
  return maxSeq + 1;
}
