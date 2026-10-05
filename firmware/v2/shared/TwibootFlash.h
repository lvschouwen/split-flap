#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <stdio.h>

#include "BootDump.h"  // BOOT_SECTION_START
#include "BootUpdateReport.h"  // bootLockFuseReadFellThrough
#include "TwibootProtocol.h"

// The twiboot I2C flash client, shared by every tree that reflashes units:
// the Master (UnitBus.cpp) and the ESP-01 follower (FollowerBus.cpp). Each
// tree instantiates these templates with its own bus adapter; nothing here
// touches Wire, the clock or a log directly.
//
// Bus adapter contract (all members required):
//   void    beginTransmission(uint8_t addr);
//   int     endTransmission(bool stop);   // 0 = ACKed
//   size_t  write(uint8_t b);             // bytes queued (0 = buffer full)
//   uint8_t requestFrom(uint8_t addr, uint8_t qty);
//   int     read();
//   int     available();
//   uint32_t nowMs();
//   void    sleepMs(uint32_t ms);
//   void    readFailed();   // called after every short read, before the bus
//                           // is touched again (the S3 rebuilds its driver
//                           // here; a tree without that need leaves it empty)
//
// Our DIP-patched twiboot listens on the unit's own address (not stock 0x29),
// so every command targets the unit's address directly. Write NACKs during
// the ~4.5 ms page-program window are EXPECTED (clock stretching is disabled
// in our twiboot build) — only a short READ is reported through readFailed().

// Bootloader liveness: pings, spaced, before the first command.
#define TWIBOOT_LIVE_ATTEMPTS      5
#define TWIBOOT_LIVE_INTERVAL_MS   100
// Ready-wait before the 132-byte burst, then for the SPM cycle after it.
#define TWIBOOT_READY_BEFORE_MS    100
#define TWIBOOT_READY_AFTER_MS     50
// A page is written at most this many times before the flash gives up.
#define TWIBOOT_PAGE_WRITE_ATTEMPTS 2
// Fresh-sketch liveness after the exit: a slow-booting unit (marginal supply,
// cold start) can need more than 2 s after twiboot's jump_to_app().
#define TWIBOOT_SKETCH_ATTEMPTS    5
#define TWIBOOT_SKETCH_INTERVAL_MS 500

// Where a chip check or page write stopped. Ok is the only success.
enum class TwibootStep : uint8_t {
  Ok,
  ChipRequestFailed,   // chipinfo request NACKed
  ChipShortRead,       // chipinfo reply shorter than 8 bytes
  ChipBadSignature,    // not an ATmega328P
  ChipBadPageSize,     // page size is not TWIBOOT_PAGE_SIZE
  PageNotReady,        // twiboot did not ACK before the burst
  PageBurstTruncated,  // the bus buffer cannot hold header + page
  PageWriteFailed,     // the burst was NACKed
  PageStuckBusy,       // twiboot did not ACK again after the burst
  PageReadFailed,      // the verify read-back failed
  PageVerifyMismatch,  // read-back differed on every attempt
};

inline const char* twibootStepName(TwibootStep s) {
  switch (s) {
    case TwibootStep::Ok:                 return "ok";
    case TwibootStep::ChipRequestFailed:  return "chipinfo request failed";
    case TwibootStep::ChipShortRead:      return "chipinfo read short";
    case TwibootStep::ChipBadSignature:   return "unexpected chip signature";
    case TwibootStep::ChipBadPageSize:    return "unexpected page size";
    case TwibootStep::PageNotReady:       return "twiboot not ready before page";
    case TwibootStep::PageBurstTruncated: return "page burst truncated";
    case TwibootStep::PageWriteFailed:    return "page write failed";
    case TwibootStep::PageStuckBusy:      return "twiboot stuck busy after page";
    case TwibootStep::PageReadFailed:     return "verify read failed";
    default:                              return "verify mismatch persisted";
  }
}

// An image reaching into the boot section would overwrite twiboot itself.
inline bool twibootImageFits(size_t len) { return len <= BOOT_SECTION_START; }

template <typename Bus>
int twibootPing(Bus& bus, uint8_t addr) {
  bus.beginTransmission(addr);
  bus.write((uint8_t)TWIBOOT_CMD_WAIT);
  return bus.endTransmission(true);
}

// A jump to the sketch, not a reset: the caller follows up with a clean
// watchdog restart once the sketch answers (twibootAwaitSketch).
template <typename Bus>
int twibootExit(Bus& bus, uint8_t addr) {
  bus.beginTransmission(addr);
  bus.write((uint8_t)TWIBOOT_CMD_SWITCH_APPLICATION);
  bus.write((uint8_t)TWIBOOT_BOOTTYPE_APPLICATION);
  return bus.endTransmission(true);
}

// Spin-poll with CMD_WAIT until twiboot ACKs again (its async flash write
// finished) or the timeout elapses.
template <typename Bus>
bool twibootWaitReady(Bus& bus, uint8_t addr, uint16_t timeoutMs) {
  uint32_t deadline = bus.nowMs() + timeoutMs;
  while ((int32_t)(bus.nowMs() - deadline) < 0) {
    if (twibootPing(bus, addr) == 0) return true;
    bus.sleepMs(1);
  }
  return false;
}

template <typename Bus>
bool twibootAwaitBootloader(Bus& bus, uint8_t addr) {
  for (int attempt = 0; attempt < TWIBOOT_LIVE_ATTEMPTS; attempt++) {
    if (twibootPing(bus, addr) == 0) return true;
    bus.sleepMs(TWIBOOT_LIVE_INTERVAL_MS);
  }
  return false;
}

template <typename Bus>
void twibootDrain(Bus& bus) {
  while (bus.available()) bus.read();
}

// Is a bootloader answering at this address? Safe against a sketch-running
// unit: the sketch ignores a write of this length, so the probe never moves a
// drum. Any chipinfo request restarts twiboot's boot-window countdown.
template <typename Bus>
bool twibootIsBootloader(Bus& bus, uint8_t addr) {
  bus.beginTransmission(addr);
  bus.write((uint8_t)TWIBOOT_CMD_ACCESS_MEMORY);
  bus.write((uint8_t)TWIBOOT_MEMTYPE_CHIPINFO);
  bus.write((uint8_t)0x00);
  bus.write((uint8_t)0x00);
  if (bus.endTransmission(false) != 0) return false;
  uint8_t got = bus.requestFrom(addr, (uint8_t)8);
  if (got < 3) {
    twibootDrain(bus);
    bus.readFailed();
    return false;
  }
  uint8_t sig0 = (uint8_t)bus.read();
  uint8_t sig1 = (uint8_t)bus.read();
  uint8_t sig2 = (uint8_t)bus.read();
  twibootDrain(bus);
  return isAtmega328pSignature(sig0, sig1, sig2);
}

// --- identity (#541) and fuse/lock bytes (#543) ----------------------------------
// What a bootloader says about itself. Only ever asked of a unit that already
// answered as a bootloader (twibootIsBootloader): a sketch would read the
// version request as a letter.

// `info` holds TWIBOOT_VERSION_LEN bytes.
inline void twibootParseVersion(const uint8_t* info, TwibootIdentity& out) {
  out = TwibootIdentity{};
  if (info[0] == 'S' && info[1] == 'F' && info[2] >= 2 &&
      info[2] != TWIBOOT_GEN_UNKNOWN) {
    out.generation = info[2];
    out.caps = info[3];
  } else if (memcmp(info, "TWIBOOT", 7) == 0) {
    out.generation = TWIBOOT_GEN_NO_IDENTITY;
  } else {
    out.generation = TWIBOOT_GEN_UNKNOWN;
  }
}

// Reads the identity and, from an image that serves them, the fuse and lock
// bytes. False when the version read failed (generation stays UNREAD). A
// failed fuse read keeps the identity and leaves fusesValid false.
//
// Some of the fielded chips answer a fuse read with flash bytes 0..3 (#518);
// the unit's application sees the same. Those four bytes are read back here
// and a reply equal to them is not reported as fuses.
template <typename Bus>
bool twibootReadIdentity(Bus& bus, uint8_t addr, TwibootIdentity& out) {
  out = TwibootIdentity{};
  bus.beginTransmission(addr);
  bus.write((uint8_t)TWIBOOT_CMD_READ_VERSION);
  if (bus.endTransmission(false) != 0) return false;
  uint8_t got = bus.requestFrom(addr, (uint8_t)TWIBOOT_VERSION_LEN);
  if (got != TWIBOOT_VERSION_LEN) {
    twibootDrain(bus);
    bus.readFailed();
    return false;
  }
  uint8_t info[TWIBOOT_VERSION_LEN];
  for (int i = 0; i < TWIBOOT_VERSION_LEN; i++) info[i] = (uint8_t)bus.read();
  twibootParseVersion(info, out);
  if (out.generation < 2 || out.generation == TWIBOOT_GEN_UNKNOWN) return true;
  const bool wantFuses = (out.caps & TWIBOOT_CAP_FUSE_CHIPINFO) != 0;
  const bool wantCrash = (out.caps & TWIBOOT_CAP_CRASH_RECORD) != 0;
  if (!wantFuses && !wantCrash) return true;
  // The crash count sits behind the fuse bytes; never ask an image for more
  // bytes than it says it serves.
  const uint8_t chipLen = wantCrash ? TWIBOOT_CHIPINFO_CRASH_LEN
                                    : TWIBOOT_CHIPINFO_FUSES_LEN;

  bus.beginTransmission(addr);
  bus.write((uint8_t)TWIBOOT_CMD_ACCESS_MEMORY);
  bus.write((uint8_t)TWIBOOT_MEMTYPE_CHIPINFO);
  bus.write((uint8_t)0x00);
  bus.write((uint8_t)0x00);
  if (bus.endTransmission(false) != 0) return true;
  got = bus.requestFrom(addr, chipLen);
  if (got != chipLen) {
    twibootDrain(bus);
    bus.readFailed();
    return true;
  }
  for (int i = 0; i < TWIBOOT_CHIPINFO_LEN; i++) bus.read();
  uint8_t lfuse = (uint8_t)bus.read();
  uint8_t lock = (uint8_t)bus.read();
  uint8_t efuse = (uint8_t)bus.read();
  uint8_t hfuse = (uint8_t)bus.read();
  if (wantCrash) {
    out.crashCount = (uint8_t)bus.read();
    out.crashValid = true;
  }
  if (!wantFuses) return true;

  bus.beginTransmission(addr);
  bus.write((uint8_t)TWIBOOT_CMD_ACCESS_MEMORY);
  bus.write((uint8_t)TWIBOOT_MEMTYPE_FLASH);
  bus.write((uint8_t)0x00);
  bus.write((uint8_t)0x00);
  if (bus.endTransmission(false) != 0) return true;
  got = bus.requestFrom(addr, (uint8_t)4);
  if (got != 4) {
    twibootDrain(bus);
    bus.readFailed();
    return true;
  }
  uint8_t flash0to3[4];
  for (int i = 0; i < 4; i++) flash0to3[i] = (uint8_t)bus.read();
  if (bootLockFuseReadFellThrough(lock, lfuse, hfuse, efuse, flash0to3)) {
    return true;
  }
  out.fusesValid = true;
  out.lfuse = lfuse;
  out.lock = lock;
  out.efuse = efuse;
  out.hfuse = hfuse;
  return true;
}

// The tail of a scan-log line for a unit found in its bootloader; "" when
// nothing was read. The text stays in flash on the ESP-01 (BootDump.h).
#define TWIBOOT_IDENTITY_TEXT_CAP 80
inline void twibootIdentityText(char* buf, size_t cap,
                                const TwibootIdentity& id) {
  if (id.generation == TWIBOOT_GEN_UNREAD) {
    if (cap > 0) buf[0] = '\0';
    return;
  }
  if (id.generation == TWIBOOT_GEN_NO_IDENTITY) {
    BOOT_DUMP_SNPRINTF(buf, cap, " (bootloader without identity bytes)");
    return;
  }
  if (id.generation == TWIBOOT_GEN_UNKNOWN) {
    BOOT_DUMP_SNPRINTF(buf, cap, " (bootloader identity not recognised)");
    return;
  }
  char crash[36] = "";
  if (id.crashValid && id.crashCount > 0) {
    BOOT_DUMP_SNPRINTF(crash, sizeof(crash), ", %s%u crash reset(s)",
                       twibootHeldForCrashing(id) ? "HELD after " : "",
                       (unsigned)id.crashCount);
  }
  if (!id.fusesValid) {
    BOOT_DUMP_SNPRINTF(buf, cap, " (bootloader v%u%s, lock/fuses unreadable)",
                       (unsigned)id.generation, crash);
  } else {
    BOOT_DUMP_SNPRINTF(buf, cap,
                       " (bootloader v%u%s, lock %02x, fuses l %02x h %02x e %02x)",
                       (unsigned)id.generation, crash, (unsigned)id.lock,
                       (unsigned)id.lfuse, (unsigned)id.hfuse,
                       (unsigned)id.efuse);
  }
}

// Chipinfo must name the ATmega328P with the expected page size.
template <typename Bus>
TwibootStep twibootVerifyChip(Bus& bus, uint8_t addr) {
  bus.beginTransmission(addr);
  bus.write((uint8_t)TWIBOOT_CMD_ACCESS_MEMORY);
  bus.write((uint8_t)TWIBOOT_MEMTYPE_CHIPINFO);
  bus.write((uint8_t)0x00);
  bus.write((uint8_t)0x00);
  if (bus.endTransmission(false) != 0) return TwibootStep::ChipRequestFailed;
  uint8_t got = bus.requestFrom(addr, (uint8_t)8);
  if (got != 8) {
    twibootDrain(bus);
    bus.readFailed();
    return TwibootStep::ChipShortRead;
  }
  uint8_t sig0 = (uint8_t)bus.read();
  uint8_t sig1 = (uint8_t)bus.read();
  uint8_t sig2 = (uint8_t)bus.read();
  uint8_t pageSize = (uint8_t)bus.read();
  twibootDrain(bus);  // flash + eeprom sizes, unused
  if (!isAtmega328pSignature(sig0, sig1, sig2)) {
    return TwibootStep::ChipBadSignature;
  }
  if (pageSize != TWIBOOT_PAGE_SIZE) return TwibootStep::ChipBadPageSize;
  return TwibootStep::Ok;
}

// Reads one page: the write framing with no payload, then a repeated-start
// read — the flow twiboot's own host tool uses. `out` holds TWIBOOT_PAGE_SIZE.
template <typename Bus>
bool twibootReadFlashPage(Bus& bus, uint8_t addr, uint16_t flashAddr,
                          uint8_t* out) {
  bus.beginTransmission(addr);
  bus.write((uint8_t)TWIBOOT_CMD_ACCESS_MEMORY);
  bus.write((uint8_t)TWIBOOT_MEMTYPE_FLASH);
  bus.write((uint8_t)((flashAddr >> 8) & 0xFF));
  bus.write((uint8_t)(flashAddr & 0xFF));
  if (bus.endTransmission(false) != 0) return false;
  uint8_t got = bus.requestFrom(addr, (uint8_t)TWIBOOT_PAGE_SIZE);
  if (got != TWIBOOT_PAGE_SIZE) {
    twibootDrain(bus);
    bus.readFailed();
    return false;
  }
  for (int i = 0; i < TWIBOOT_PAGE_SIZE; i++) out[i] = (uint8_t)bus.read();
  return true;
}

// The 4-byte header + page must fit the bus buffer in ONE transaction. write()
// only queues into RAM, so a short count is caught before anything reaches
// the bus — a truncated burst would otherwise program a page with a missing
// tail.
template <typename Bus>
TwibootStep twibootWriteFlashPage(Bus& bus, uint8_t addr, uint16_t flashAddr,
                                  const uint8_t* page) {
  bus.beginTransmission(addr);
  size_t queued = 0;
  queued += bus.write((uint8_t)TWIBOOT_CMD_ACCESS_MEMORY);
  queued += bus.write((uint8_t)TWIBOOT_MEMTYPE_FLASH);
  queued += bus.write((uint8_t)((flashAddr >> 8) & 0xFF));
  queued += bus.write((uint8_t)(flashAddr & 0xFF));
  for (int i = 0; i < TWIBOOT_PAGE_SIZE; i++) queued += bus.write(page[i]);
  if (queued != TWIBOOT_PAGE_SIZE + 4) return TwibootStep::PageBurstTruncated;
  return bus.endTransmission(true) == 0 ? TwibootStep::Ok
                                        : TwibootStep::PageWriteFailed;
}

// Writes one page and reads it back, rewriting on a mismatch. On any failure
// the caller must stop while the unit still sits in twiboot, so it never
// boots a corrupted sketch. `rewrites` (optional) counts mismatched attempts.
template <typename Bus>
TwibootStep twibootFlashAndVerifyPage(Bus& bus, uint8_t addr,
                                      uint16_t flashAddr, const uint8_t* page,
                                      uint8_t* rewrites = nullptr) {
  if (rewrites) *rewrites = 0;
  for (int attempt = 0; attempt < TWIBOOT_PAGE_WRITE_ATTEMPTS; attempt++) {
    if (!twibootWaitReady(bus, addr, TWIBOOT_READY_BEFORE_MS)) {
      return TwibootStep::PageNotReady;
    }
    TwibootStep wrote = twibootWriteFlashPage(bus, addr, flashAddr, page);
    if (wrote != TwibootStep::Ok) return wrote;
    if (!twibootWaitReady(bus, addr, TWIBOOT_READY_AFTER_MS)) {
      return TwibootStep::PageStuckBusy;
    }
    uint8_t readBuf[TWIBOOT_PAGE_SIZE];
    if (!twibootReadFlashPage(bus, addr, flashAddr, readBuf)) {
      return TwibootStep::PageReadFailed;
    }
    if (memcmp(readBuf, page, TWIBOOT_PAGE_SIZE) == 0) return TwibootStep::Ok;
    if (rewrites) (*rewrites)++;
  }
  return TwibootStep::PageVerifyMismatch;
}

// Polls the bare address until the freshly started sketch ACKs. True = it is
// up and can take the reboot command that gives it a clean watchdog restart.
template <typename Bus>
bool twibootAwaitSketch(Bus& bus, uint8_t addr) {
  for (int attempt = 0; attempt < TWIBOOT_SKETCH_ATTEMPTS; attempt++) {
    bus.sleepMs(TWIBOOT_SKETCH_INTERVAL_MS);
    bus.beginTransmission(addr);
    if (bus.endTransmission(true) == 0) return true;
  }
  return false;
}
