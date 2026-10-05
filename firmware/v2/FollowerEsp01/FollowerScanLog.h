#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stdio.h>

#include "ReflashPlan.h"  // reflashUnitProtocolMismatch
#include "TwibootFlash.h"  // twibootIdentityText
#include "UnitHealth.h"

// One log line per unit a bus scan found in a state that needs attention:
// off the bundled firmware, unreadable, in its bootloader, on a protocol this
// build does not speak, or gone since the previous scan. A unit on the
// bundled firmware produces nothing, so a healthy scan stays the one summary
// line — the ring is small.
//
// The wording is the S3 master's scan log (Master/UnitBus.cpp), so one grep
// over the fleet log finds the same fact on either kind of row. The text
// stays in flash on this chip.
//
// Pure logic, natively tested (test_follower_ops).

#define FOLLOWER_SCAN_LINE_CAP 128

// What the line says; the caller logs a unit again only when this changes.
enum ScanFinding : uint8_t {
  SCAN_FINDING_NONE = 0,
  SCAN_FINDING_MISSING,
  SCAN_FINDING_BOOTLOADER,
  SCAN_FINDING_PROTOCOL,
  SCAN_FINDING_OUTDATED,
  SCAN_FINDING_UNREADABLE,
};

// `stateBefore` is the slot's UnitFacts::state from the previous scan.
// SCAN_FINDING_NONE = the unit needs no line; buf is then untouched.
inline uint8_t followerScanLine(char* buf, size_t cap, uint8_t i2cAddress,
                                const UnitFacts& fact, uint8_t stateBefore) {
  if (fact.state == 0) {
    if (stateBefore == 0) return SCAN_FINDING_NONE;
    snprintf_P(buf, cap,
               PSTR("- unit at 0x%02x is MISSING (answered the previous scan)"),
               i2cAddress);
    return SCAN_FINDING_MISSING;
  }
  if (fact.state == 2) {
    char identity[TWIBOOT_IDENTITY_TEXT_CAP];
    twibootIdentityText(identity, sizeof(identity), fact.bootloader);
    snprintf_P(buf, cap, PSTR("- unit at 0x%02x is in BOOTLOADER mode%s"),
               i2cAddress, identity);
    return SCAN_FINDING_BOOTLOADER;
  }
  if (reflashUnitProtocolMismatch(fact)) {
    snprintf_P(buf, cap,
               PSTR("- unit at 0x%02x speaks protocol v%u, we speak v%u — NOT "
                    "DRIVABLE, reflash target"),
               i2cAddress, (unsigned)fact.protocolVersion,
               (unsigned)SFP_PROTOCOL_VERSION);
    return SCAN_FINDING_PROTOCOL;
  }
  if (fact.fwStatus == 0) return SCAN_FINDING_NONE;
  if (fact.fwStatus == 1) {
    snprintf_P(buf, cap,
               PSTR("- unit at 0x%02x is running sketch (fw %s — OUTDATED)"),
               i2cAddress, fact.version);
    return SCAN_FINDING_OUTDATED;
  }
  snprintf_P(buf, cap,
             PSTR("- unit at 0x%02x is running sketch (fw UNKNOWN — unreadable "
                  "version reply)"),
             i2cAddress);
  return SCAN_FINDING_UNREADABLE;
}
