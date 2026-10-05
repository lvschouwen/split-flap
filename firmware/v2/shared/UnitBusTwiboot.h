#pragma once
// UnitBusTwiboot.h — the sequences a row master runs against a unit's
// bootloader, on top of the twiboot client (TwibootFlash.h) and the sketch
// protocol (UnitBusCore.h): the rescue probe, the boot-section read, and one
// unit's flash. One order of steps for both row masters; each tree supplies
// the image, the watchdog/abort hook and the wording of its log lines.
// Natively tested by test_unit_bus_twiboot against a scripted unit.

#include <stddef.h>
#include <stdint.h>

#include "BootDump.h"  // BOOT_SECTION_START / _LEN
#include "TwibootFlash.h"
#include "UnitBusCore.h"
#include "UnitRescuePolicy.h"  // UnitRescueProbe

// --- rescue probe (#498) -----------------------------------------------------------

// What is at the address of a unit that stopped answering: nothing, a unit
// parked in twiboot (told to start its application), a unit its bootloader is
// holding because the application keeps crashing (#542 — left there: starting
// it again would repeat the crash, and it is a flash target where it sits),
// or a sketch that ACKs but cannot be read. `id` is what the bootloader said
// about itself, UNREAD unless one answered. Only call outside the
// probe-inhibit window — the chipinfo request pins twiboot.
template <typename Bus>
inline UnitRescueProbe unitRescueProbe(Bus& bus, uint8_t i2cAddress,
                                       TwibootIdentity& id) {
  id = TwibootIdentity{};
  bus.mark(UnitBusAct::Probe, i2cAddress);
  bus.beginTransmission(i2cAddress);
  if (bus.endTransmission(true) != 0) return UnitRescueProbe::NoAck;
  if (!twibootIsBootloader(bus, i2cAddress)) return UnitRescueProbe::SketchSilent;
  // Asked twice before the unit is started: one lost read must not put a
  // crash-looping unit back into its crash with a cleared count.
  if (!twibootReadIdentity(bus, i2cAddress, id)) {
    twibootReadIdentity(bus, i2cAddress, id);
  }
  if (twibootHeldForCrashing(id)) return UnitRescueProbe::CrashHeld;
  twibootExit(bus, i2cAddress);
  return UnitRescueProbe::Bootloader;
}

// --- leaving the bootloader ------------------------------------------------------

// After a twiboot exit the application has started from a JUMP, not a reset:
// give it time to come up, then — if it answers — a clean watchdog restart
// (v1 #113). Returns whether the unit answered.
#define UNIT_POST_EXIT_BOOT_MS 2000UL

// --- boot-section read (#511) ------------------------------------------------------

enum class UnitBootReadResult : uint8_t {
  Ok = 0,
  BootloaderSilent,  // twiboot never ACKed a ping at this address
  ChipMismatch,      // chipinfo signature / page size not an ATmega328P
  ReadFailed,        // a page read came back short twice
};

// Reads the BOOT_SECTION_LEN bytes of the boot section from a unit that is
// already in twiboot, then starts its application and restarts it cleanly.
// No flash write. `out` holds the bytes only on Ok. `keepAlive()` is called
// around the long steps (the S3 feeds its task watchdog there). A unit that
// never answered as a bootloader is not sent an exit.
template <typename Bus, typename KeepAlive>
inline UnitBootReadResult unitReadBootSection(Bus& bus, uint8_t i2cAddress,
                                              uint8_t* out,
                                              KeepAlive&& keepAlive,
                                              bool& exitAcked,
                                              bool& answeredAfter) {
  exitAcked = false;
  answeredAfter = false;
  if (!twibootAwaitBootloader(bus, i2cAddress)) {
    return UnitBootReadResult::BootloaderSilent;
  }
  UnitBootReadResult result = UnitBootReadResult::Ok;
  if (twibootVerifyChip(bus, i2cAddress) != TwibootStep::Ok) {
    result = UnitBootReadResult::ChipMismatch;
  } else {
    for (int page = 0; page < BOOT_SECTION_LEN / TWIBOOT_PAGE_SIZE; page++) {
      keepAlive();
      uint16_t flashAddr =
          (uint16_t)(BOOT_SECTION_START + page * TWIBOOT_PAGE_SIZE);
      uint8_t* dst = out + page * TWIBOOT_PAGE_SIZE;
      // One retry: a single short read is bus noise, two are a failure.
      if (!twibootReadFlashPage(bus, i2cAddress, flashAddr, dst) &&
          !twibootReadFlashPage(bus, i2cAddress, flashAddr, dst)) {
        result = UnitBootReadResult::ReadFailed;
        break;
      }
    }
  }
  // Whatever the read came to, the unit must not be left in its bootloader.
  for (int attempt = 0; attempt < 3 && !exitAcked; attempt++) {
    exitAcked = twibootExit(bus, i2cAddress) == 0;
    if (!exitAcked) bus.sleepMs(20);
  }
  if (!exitAcked) return result;
  keepAlive();
  bus.sleepMs(UNIT_POST_EXIT_BOOT_MS);
  bus.beginTransmission(i2cAddress);
  answeredAfter = bus.endTransmission(true) == 0;
  if (answeredAfter) unitRebootSketch(bus, i2cAddress);
  return result;
}

// --- flashing one unit (#205) --------------------------------------------------------

// Any failure once the page stream began leaves the unit sitting in twiboot —
// boot auto-install or a retry recovers it; it is never exited onto a torn
// image.
enum class UnitFlashResult : uint8_t {
  Ok = 0,
  ImageTooLarge,     // the image would overwrite the bootloader; nothing sent
  BootloaderSilent,  // twiboot never ACKed a ping at this address
  ChipMismatch,      // chipinfo signature / page size not an ATmega328P
  PageFailed,        // write / readback-verify failed after one rewrite
  ExitFailed,        // SWITCH_APPLICATION not ACKed
  PostBootSilent,    // sketch did not answer after the exit
  Aborted,           // the tree asked to stop — unit left in twiboot
};

struct UnitFlashReport {
  UnitFlashResult result = UnitFlashResult::Ok;
  TwibootStep step = TwibootStep::Ok;  // the failing step, where one applies
  uint16_t pageAddr = 0;               // the failing page (PageFailed)
  int rebootStatus = 0;                // of the closing CMD_REBOOT (Ok only)
};

// Streams a page-padded image to a unit that is already in twiboot: size
// guard → bootloader liveness → chip check → write + read-back per page (one
// rewrite, v1 #110) → exit → wait for the sketch → clean restart (v1 #113).
//
//   pages(pageIndex, buf)   fills buf with TWIBOOT_PAGE_SIZE bytes of the
//                           image (a pointer on the S3, PROGMEM on the ESP-01)
//   watch.keepGoing()       called before every page; false aborts
//   watch.pageRewritten(flashAddr, rewrites)   a page verified wrong at least
//                           once — worth a log line
template <typename Bus, typename Pages, typename Watch>
inline UnitFlashReport unitFlashImage(Bus& bus, uint8_t i2cAddress,
                                      size_t imageLen, Pages&& pages,
                                      Watch& watch) {
  UnitFlashReport report;
  if (!twibootImageFits(imageLen)) {
    report.result = UnitFlashResult::ImageTooLarge;
    return report;
  }
  if (!twibootAwaitBootloader(bus, i2cAddress)) {
    report.result = UnitFlashResult::BootloaderSilent;
    return report;
  }
  report.step = twibootVerifyChip(bus, i2cAddress);
  if (report.step != TwibootStep::Ok) {
    report.result = UnitFlashResult::ChipMismatch;
    return report;
  }
  size_t pageCount = imageLen / TWIBOOT_PAGE_SIZE;
  uint8_t pageBuf[TWIBOOT_PAGE_SIZE];
  for (size_t pageIndex = 0; pageIndex < pageCount; pageIndex++) {
    if (!watch.keepGoing()) {
      report.result = UnitFlashResult::Aborted;
      return report;
    }
    uint16_t flashAddr = (uint16_t)(pageIndex * TWIBOOT_PAGE_SIZE);
    pages(pageIndex, pageBuf);
    uint8_t rewrites = 0;
    report.step = twibootFlashAndVerifyPage(bus, i2cAddress, flashAddr,
                                            pageBuf, &rewrites);
    if (rewrites > 0) watch.pageRewritten(flashAddr, rewrites);
    if (report.step != TwibootStep::Ok) {
      report.result = UnitFlashResult::PageFailed;
      report.pageAddr = flashAddr;
      return report;
    }
  }
  if (twibootExit(bus, i2cAddress) != 0) {
    report.result = UnitFlashResult::ExitFailed;
    return report;
  }
  if (!twibootAwaitSketch(bus, i2cAddress)) {
    report.result = UnitFlashResult::PostBootSilent;
    return report;
  }
  report.rebootStatus = unitRebootSketch(bus, i2cAddress);
  return report;
}
