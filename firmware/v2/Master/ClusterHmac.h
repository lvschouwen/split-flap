#pragma once
// ClusterHmac.h — pure cluster-wire authentication (#313 follow-on, epic
// #270). Adds a forgery-proof layer ON TOP of #313's source-IP binding: the
// leader mints a random 256-bit key per member at join (sent in the join
// body — cleartext-at-join is the accepted threat model, closing active
// forgery by an IP-spoofing LAN host but not a passive sniffer present at
// that one join), the follower stores it with its membership, and every
// subsequent leader→follower wire request (render/ping/leave) carries
// `ts`+`mac` = HMAC-SHA256(key, canonical-message).
//
// Enforcement auto-negotiates with no capability flag: a follower ENFORCES
// iff it holds a key; a leader SIGNS iff it minted one. New leader ↔ new
// follower ⇒ enforced; any pre-HMAC peer ⇒ unsigned passthrough that falls
// back to #313's IP binding (#276 fleet-convergence makes it universal).
//
// Natively tested by test_cluster_hmac; copied VERBATIM to FollowerEsp01
// (copy policy: fix bugs in both trees). A portable public-domain SHA-256
// keeps host tests, the S3 and the ESP-01 on one identical code path — key
// minting (RNG) is the only target-specific glue, injected by the caller.

#include <Arduino.h>

#include <stdint.h>
#include <string.h>

// Replay window: a signed request whose ts is farther than this from the
// follower's NTP-synced clock is rejected. Generous enough for LAN + clock
// skew, tight enough that a captured mac cannot be replayed indefinitely.
static const uint64_t CLUSTER_HMAC_WINDOW_MS = 30000ULL;

// Coarse-persist threshold for the monotonic replay high-water mark. The mark
// advances on every accepted request (~ping cadence) but is only written to
// flash once it has moved this far past the last persisted value — so a reboot
// reloads a mark at most this stale instead of resetting to 0, bounding the
// post-reboot replay window to this span while keeping writes rare (~1/hour).
static const uint64_t CLUSTER_HMAC_PERSIST_DELTA_MS = 3600000ULL;  // 1 hour

// True once the live mark has advanced a full persist-delta beyond what was
// last written to flash — the follower glue then stages a persist.
inline bool clusterHmacMarkNeedsPersist(uint64_t mark, uint64_t persisted) {
  return mark >= persisted + CLUSTER_HMAC_PERSIST_DELTA_MS;
}

// ---- SHA-256 (FIPS 180-4, public-domain reference implementation) ----------

struct ClusterSha256Ctx {
  uint32_t state[8];
  uint64_t bitlen;
  uint8_t buffer[64];
  uint32_t buflen;
};

inline uint32_t clusterSha256Rotr(uint32_t x, uint32_t n) {
  return (x >> n) | (x << (32 - n));
}

inline void clusterSha256Init(ClusterSha256Ctx& c) {
  c.state[0] = 0x6a09e667UL;
  c.state[1] = 0xbb67ae85UL;
  c.state[2] = 0x3c6ef372UL;
  c.state[3] = 0xa54ff53aUL;
  c.state[4] = 0x510e527fUL;
  c.state[5] = 0x9b05688cUL;
  c.state[6] = 0x1f83d9abUL;
  c.state[7] = 0x5be0cd19UL;
  c.bitlen = 0;
  c.buflen = 0;
}

inline void clusterSha256Block(ClusterSha256Ctx& c, const uint8_t* p) {
  static const uint32_t k[64] = {
      0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL, 0x3956c25bUL,
      0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL, 0xd807aa98UL, 0x12835b01UL,
      0x243185beUL, 0x550c7dc3UL, 0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL,
      0xc19bf174UL, 0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
      0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL, 0x983e5152UL,
      0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL, 0xc6e00bf3UL, 0xd5a79147UL,
      0x06ca6351UL, 0x14292967UL, 0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL,
      0x53380d13UL, 0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
      0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL, 0xd192e819UL,
      0xd6990624UL, 0xf40e3585UL, 0x106aa070UL, 0x19a4c116UL, 0x1e376c08UL,
      0x2748774cUL, 0x34b0bcb5UL, 0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL,
      0x682e6ff3UL, 0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
      0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL};
  uint32_t w[64];
  for (int i = 0; i < 16; i++) {
    w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
           ((uint32_t)p[i * 4 + 2] << 8) | ((uint32_t)p[i * 4 + 3]);
  }
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = clusterSha256Rotr(w[i - 15], 7) ^
                  clusterSha256Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = clusterSha256Rotr(w[i - 2], 17) ^
                  clusterSha256Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = c.state[0], b = c.state[1], cc = c.state[2], d = c.state[3];
  uint32_t e = c.state[4], f = c.state[5], g = c.state[6], h = c.state[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = clusterSha256Rotr(e, 6) ^ clusterSha256Rotr(e, 11) ^
                  clusterSha256Rotr(e, 25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t t1 = h + S1 + ch + k[i] + w[i];
    uint32_t S0 = clusterSha256Rotr(a, 2) ^ clusterSha256Rotr(a, 13) ^
                  clusterSha256Rotr(a, 22);
    uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
    uint32_t t2 = S0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = cc;
    cc = b;
    b = a;
    a = t1 + t2;
  }
  c.state[0] += a;
  c.state[1] += b;
  c.state[2] += cc;
  c.state[3] += d;
  c.state[4] += e;
  c.state[5] += f;
  c.state[6] += g;
  c.state[7] += h;
}

inline void clusterSha256Update(ClusterSha256Ctx& c, const uint8_t* data,
                                size_t len) {
  for (size_t i = 0; i < len; i++) {
    c.buffer[c.buflen++] = data[i];
    if (c.buflen == 64) {
      clusterSha256Block(c, c.buffer);
      c.bitlen += 512;
      c.buflen = 0;
    }
  }
}

inline void clusterSha256Final(ClusterSha256Ctx& c, uint8_t out[32]) {
  uint64_t totalBits = c.bitlen + (uint64_t)c.buflen * 8;
  uint8_t pad = 0x80;
  clusterSha256Update(c, &pad, 1);
  uint8_t zero = 0x00;
  while (c.buflen != 56) clusterSha256Update(c, &zero, 1);
  uint8_t lenBytes[8];
  for (int i = 0; i < 8; i++) {
    lenBytes[7 - i] = (uint8_t)(totalBits >> (i * 8));
  }
  clusterSha256Update(c, lenBytes, 8);
  for (int i = 0; i < 8; i++) {
    out[i * 4] = (uint8_t)(c.state[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(c.state[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(c.state[i] >> 8);
    out[i * 4 + 3] = (uint8_t)(c.state[i]);
  }
}

inline void clusterSha256(const uint8_t* data, size_t len, uint8_t out[32]) {
  ClusterSha256Ctx c;
  clusterSha256Init(c);
  clusterSha256Update(c, data, len);
  clusterSha256Final(c, out);
}

// ---- HMAC-SHA256 -----------------------------------------------------------

inline void clusterHmacSha256(const uint8_t* key, size_t keyLen,
                              const uint8_t* msg, size_t msgLen,
                              uint8_t out[32]) {
  uint8_t k[64];
  memset(k, 0, sizeof(k));
  if (keyLen > 64) {
    clusterSha256(key, keyLen, k);  // keys longer than the block are hashed
  } else {
    memcpy(k, key, keyLen);
  }
  uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; i++) {
    ipad[i] = k[i] ^ 0x36;
    opad[i] = k[i] ^ 0x5c;
  }
  uint8_t inner[32];
  ClusterSha256Ctx c;
  clusterSha256Init(c);
  clusterSha256Update(c, ipad, 64);
  clusterSha256Update(c, msg, msgLen);
  clusterSha256Final(c, inner);
  clusterSha256Init(c);
  clusterSha256Update(c, opad, 64);
  clusterSha256Update(c, inner, 32);
  clusterSha256Final(c, out);
}

// ---- key / mac hex helpers -------------------------------------------------

inline String clusterBytesToHex(const uint8_t* b, size_t len) {
  static const char* d = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out += d[b[i] >> 4];
    out += d[b[i] & 0x0F];
  }
  return out;
}

// Parse exactly `len` bytes from 2*len lowercase/uppercase hex chars.
inline bool clusterHexToBytes(const String& hex, uint8_t* out, size_t len) {
  if (hex.length() != len * 2) return false;
  for (size_t i = 0; i < len; i++) {
    int hi = -1, lo = -1;
    char a = hex[i * 2], b = hex[i * 2 + 1];
    hi = (a >= '0' && a <= '9')   ? a - '0'
         : (a >= 'a' && a <= 'f') ? a - 'a' + 10
         : (a >= 'A' && a <= 'F') ? a - 'A' + 10
                                  : -1;
    lo = (b >= '0' && b <= '9')   ? b - '0'
         : (b >= 'a' && b <= 'f') ? b - 'a' + 10
         : (b >= 'A' && b <= 'F') ? b - 'A' + 10
                                  : -1;
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

#define CLUSTER_HMAC_KEY_LEN 32

inline String clusterKeyToHex(const uint8_t key[CLUSTER_HMAC_KEY_LEN]) {
  return clusterBytesToHex(key, CLUSTER_HMAC_KEY_LEN);
}

inline bool clusterKeyFromHex(const String& hex,
                              uint8_t key[CLUSTER_HMAC_KEY_LEN]) {
  return clusterHexToBytes(hex, key, CLUSTER_HMAC_KEY_LEN);
}

// ---- canonical messages ----------------------------------------------------
// Explicit, ordered field lists (no map → no ordering drift). Leader and
// follower BOTH build these from typed fields via the same functions, so the
// signed bytes are identical on both ends by construction.

// Portable uint64 → decimal. Arduino's String(unsigned long long) exists on
// the ESP targets but NOT in ArduinoFake, so format it by hand to keep the
// one pure code path across host tests and both firmwares.
inline String clusterU64ToStr(uint64_t v) {
  char buf[21];
  int i = 20;
  buf[i] = '\0';
  if (v == 0) buf[--i] = '0';
  while (v > 0 && i > 0) {
    buf[--i] = (char)('0' + (int)(v % 10));
    v /= 10;
  }
  return String(&buf[i]);
}

// Inverse of clusterU64ToStr — parse a decimal uint64 (used to reload the
// persisted replay mark from NVS/EEPROM). Stops at the first non-digit.
inline uint64_t clusterU64FromStr(const String& s) {
  uint64_t v = 0;
  for (unsigned int i = 0; i < s.length(); i++) {
    char ch = s[i];
    if (ch < '0' || ch > '9') break;
    v = v * 10ULL + (uint64_t)(ch - '0');
  }
  return v;
}

inline String clusterHmacRenderMsg(uint64_t ts, uint32_t epoch, uint32_t seq,
                                   const String& text, int speed,
                                   uint64_t commitAtMs) {
  String m;
  m.reserve(48 + text.length());
  m += "render\n";
  m += clusterU64ToStr(ts);
  m += '\n';
  m += String((unsigned long)epoch);
  m += '\n';
  m += String((unsigned long)seq);
  m += '\n';
  m += text;  // signs the content: a captured mac cannot be reused for other text
  m += '\n';
  m += String(speed);
  m += '\n';
  m += clusterU64ToStr(commitAtMs);
  return m;
}

// Ping binds ts AND the #294 digest piggyback (its sha256 + this member's
// table index): the leader ships the promote-critical member table down the
// ping body, so the mac must cover it or an on-path attacker could swap the
// table under a valid (ts,mac) and poison a later takeover. The digest is
// hashed rather than inlined to keep the canonical bounded (it can be
// hundreds of bytes). A pre-#294 / digest-less ping signs sha256("") + (-1)
// — consistent on both ends. epoch is deliberately not bound: a reconfig
// that changes it also rotates the key, invalidating any old mac anyway.
inline String clusterHmacPingMsg(uint64_t ts, const String& digest,
                                 int youIndex) {
  uint8_t dh[32];
  clusterSha256((const uint8_t*)digest.c_str(), digest.length(), dh);
  String m;
  m.reserve(96);
  m += "ping\n";
  m += clusterU64ToStr(ts);
  m += '\n';
  m += clusterBytesToHex(dh, 32);
  m += '\n';
  m += String(youIndex);
  return m;
}

inline String clusterHmacLeaveMsg(uint64_t ts) {
  String m;
  m.reserve(24);
  m += "leave\n";
  m += clusterU64ToStr(ts);
  return m;
}

// ---- sign / verify ---------------------------------------------------------

inline String clusterHmacSign(const uint8_t key[CLUSTER_HMAC_KEY_LEN],
                              const String& msg) {
  uint8_t mac[32];
  clusterHmacSha256(key, CLUSTER_HMAC_KEY_LEN, (const uint8_t*)msg.c_str(),
                    msg.length(), mac);
  return clusterBytesToHex(mac, 32);
}

// Constant-time hex compare — same length, XOR-accumulate, no early return —
// so a matching prefix does not leak through timing (minor on a LAN, but the
// mac check is a security boundary).
inline bool clusterHmacConstTimeHexEqual(const String& a, const String& b) {
  if (a.length() != b.length()) return false;
  uint8_t diff = 0;
  for (unsigned int i = 0; i < a.length(); i++) {
    diff |= (uint8_t)(a[i] ^ b[i]);
  }
  return diff == 0;
}

// Accept a signed wire request: the recomputed mac must match, ts must clear
// a monotonic high-water mark, AND (when the clock is NTP-synced) ts must be
// within the replay window. Unsynced skips the window (matching the
// follower's existing "unsynced ⇒ render now" leniency) — authenticity still
// holds via the mac.
//
// The monotonic guard runs UNCONDITIONALLY (independent of `synced`): the
// window alone cannot stop replay while the clock is unsynced (boot-to-NTP on
// every reboot; open-ended on an internet-less LAN), and ping/leave carry no
// other ordering guard the way render does (seq). `lastAcceptedTs` is bumped
// only on full acceptance, so an attacker without the key can never advance
// it. The leader stamps one epoch-ms ts per round and sends at most one
// signed request per member per round, so genuine ts is strictly increasing;
// a captured mac thus becomes un-replayable the moment a newer request lands.
// The follower persists the mark coarsely (clusterHmacMarkNeedsPersist, ~1/h)
// so a follower REBOOT reloads it instead of resetting to 0 — otherwise a
// keyless attacker who captured a ping/leave could force a reboot (/reboot is
// only Origin-CSRF-gated) and replay it once. A re-join with fresh key
// material DOES reset it (durably) so a rebooted leader's fresh signing epoch
// is re-accepted. The residual after this is bounded to one persist-delta of
// staleness around a reboot.
inline bool clusterHmacAccept(const uint8_t key[CLUSTER_HMAC_KEY_LEN],
                              const String& msg, uint64_t ts,
                              const String& macHex, uint64_t nowEpochMs,
                              bool synced, uint64_t& lastAcceptedTs) {
  String expect = clusterHmacSign(key, msg);
  if (!clusterHmacConstTimeHexEqual(expect, macHex)) return false;
  if (synced) {
    uint64_t diff = nowEpochMs > ts ? nowEpochMs - ts : ts - nowEpochMs;
    if (diff > CLUSTER_HMAC_WINDOW_MS) return false;
  }
  if (ts <= lastAcceptedTs) return false;  // replay / out-of-order
  lastAcceptedTs = ts;
  return true;
}
