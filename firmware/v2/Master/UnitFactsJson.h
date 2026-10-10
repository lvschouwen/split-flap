#pragma once
// UnitFactsJson.h — reads a row board's unit facts back into the one
// UnitFacts struct (#559/#566). The row sends the unit facts document
// exactly as the shared buildUnitHealthJson writes it (UnitHealth.h); this is
// its inverse, so the master judges and words a remote row's units with the
// same code as its own. Pure, natively tested by test_unit_facts_json: a
// document read here and written again by the serializer is the document.
// tests/test_unit_facts_keys.py fails when the serializer gains a key this
// reader does not name.
//
// A key that is absent leaves its facts "not read", as on the row. A key this
// build does not know is ignored (a newer row). A known key with a value its
// field cannot hold refuses the whole document: the last good one stands.
//
// Ages ("age", "errAge") are counted on the row's clock; they are turned into
// instants on the master's, against the moment the document arrived.

#include <ArduinoJson.h>

#include "UnitHealth.h"

struct UnitFactsDoc {
  int width = 0;
  int faulty = 0;
  UnitFacts units[UNITS_AMOUNT];
};

namespace unitfactsjson {

struct Reader {
  JsonObjectConst o;
  bool ok = true;

  bool has(const char* key) const { return !o[key].isNull(); }

  uint32_t number(const char* key, uint32_t max) {
    JsonVariantConst v = o[key];
    if (!v.is<uint32_t>() || v.as<uint32_t>() > max) {
      ok = false;
      return 0;
    }
    return v.as<uint32_t>();
  }
  int32_t signedNumber(const char* key, int32_t min, int32_t max) {
    JsonVariantConst v = o[key];
    if (!v.is<int32_t>() || v.as<int32_t>() < min || v.as<int32_t>() > max) {
      ok = false;
      return 0;
    }
    return v.as<int32_t>();
  }
  bool flag(const char* key) { return has(key) && number(key, 1) == 1; }

  // `digits` hex digits, as the serializer prints them.
  uint32_t hex(const char* key, size_t digits) {
    const char* text = o[key].as<const char*>();
    if (text == nullptr || strlen(text) != digits) {
      ok = false;
      return 0;
    }
    uint32_t value = 0;
    for (size_t i = 0; i < digits; i++) {
      const char c = text[i];
      uint32_t nibble;
      if (c >= '0' && c <= '9') nibble = c - '0';
      else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
      else {
        ok = false;
        return 0;
      }
      value = (value << 4) | nibble;
    }
    return value;
  }
};

inline bool readUnit(JsonObjectConst o, int index, uint32_t nowMs, UnitFacts& u) {
  Reader r{o};
  u = UnitFacts{};
  if (r.number("i", UNITS_AMOUNT) != (uint32_t)index) return false;
  u.state = (uint8_t)r.number("st", 2);
  u.statusValid = r.number("v", 1) == 1;
  if (u.statusValid) {
    u.fwStatus = (uint8_t)r.number("fw", 2);
    const char* rev = o["rev"].as<const char*>();
    if (rev == nullptr || strlen(rev) >= sizeof(u.version)) return false;
    strcpy(u.version, rev);
    u.status.uptimeSeconds = (uint16_t)r.number("up", 0xFFFF);
    u.status.lifetimeBrownoutCount = (uint8_t)r.number("br", 0xFF);
    u.status.lifetimeWatchdogCount = (uint8_t)r.number("wd", 0xFF);
    u.status.badCommandCount = (uint8_t)r.number("bc", 0xFF);
    u.status.mcusrAtBoot = (uint8_t)r.number("mc", 0xFF);
    u.status.flags = (uint8_t)r.number("fl", 0xFF);
    u.status.lastHomingStepCount = (uint16_t)r.number("hs", 0xFFFF);
    // "ae" restates a bit of "fl".
    u.resetSeen = r.flag("rs");
  }
  if (r.has("odo")) {
    u.odometer = r.number("odo", 0xFFFFFFFFUL);
    u.odometerValid = true;
  }
  if (r.has("ofs")) {
    u.offset = (int16_t)r.signedNumber("ofs", -32768, 32767);
    u.offsetValid = true;
  }
  if (r.has("de")) {
    u.diagValid = true;
    u.driftEvents = (uint8_t)r.number("de", 0xFF);
    u.lastDriftSteps = (int8_t)r.signedNumber("ds", -128, 127);
    if (r.flag("dp")) u.driftFlags |= UNIT_DRIFT_FLAG_PENDING;
    if (r.has("phys")) {
      u.physLetter = (uint8_t)r.number("phys", 0xFE);
      u.driftFlags |= UNIT_DRIFT_FLAG_POSITION_KNOWN;
      u.mismatch = r.flag("mm");
    }
  }
  if (r.has("vcc")) {
    u.vitalsValid = true;
    u.vitals.vccNow_mV = (uint16_t)r.number("vcc", 0xFFFF);
    u.vitals.vccMin_mV = (uint16_t)r.number("vmin", 0xFFFF);
    u.vitals.cmdPos = (uint8_t)r.number("cp", 0xFF);
    u.vitals.freeRamMin = (uint16_t)r.number("ram", 0xFFFF);
  }
  if (r.has("se")) {
    u.extDiagValid = true;
    u.extDiag.stepExcessLast = (uint16_t)r.number("se", 0xFFFF);
    u.extDiag.stepExcessMax = (uint16_t)r.number("sx", 0xFFFF);
    u.extDiag.vccSagLastMove = (uint16_t)r.number("sag", 0xFFFF);
    u.extDiag.hallEdgesLastRev = (uint8_t)r.number("he", 0xFF);
    u.extDiag.dutyWindow = (uint16_t)r.number("dw", 0xFFFF);
    u.extDiag.statusBits = (uint8_t)r.number("sb", 0xFF);
  }
  if (r.has("ut")) {
    u.linkValid = true;
    u.link.uptimeSeconds = r.number("ut", 0xFFFFFFFFUL);
    u.link.rxFrames = (uint16_t)r.number("rx", 0xFFFF);
    u.link.txReplies = (uint16_t)r.number("tx", 0xFFFF);
    u.link.deafHeals = (uint8_t)r.number("dh", 0xFF);
  }
  if (r.has("pv")) {
    // "pmm" restates what this build makes of the version.
    u.protocolKnown = true;
    u.protocolVersion = (uint8_t)r.number("pv", 0xFF);
  }
  // The lifetime block: each key only when it is not zero, so a block that is
  // valid and all zero is the same document as one that was not read.
  UnitLifetimeFacts& lt = u.lifetime;
  if (r.has("hf")) lt.homeFailedCount = (uint8_t)r.number("hf", 0xFF);
  if (r.has("gates")) lt.featureGates = (uint8_t)r.number("gates", 0xFF);
  if (r.has("sxl")) lt.stepExcessLifetimeMax = (uint16_t)r.number("sxl", 0xFFFF);
  if (r.has("stw0")) {
    lt.selfTestFirstHallWindow = (uint16_t)r.number("stw0", 0xFFFF);
    lt.selfTestLastHallWindow = (uint16_t)r.number("stw1", 0xFFFF);
  }
  if (r.has("str0")) {
    lt.selfTestFirstStepsPerRev = (uint16_t)r.number("str0", 0xFFFF);
    lt.selfTestLastStepsPerRev = (uint16_t)r.number("str1", 0xFFFF);
  }
  if (r.has("fr")) lt.idleHallFutileRehomes = (uint8_t)r.number("fr", 0xFF);
  lt.idleHallStoodDown = r.flag("frd");
  u.lifetimeValid = r.has("hf") || r.has("gates") || r.has("sxl") || r.has("stw0") ||
                    r.has("str0") || r.has("fr") || r.has("frd");
  u.busRecordValid = r.has("bsn");
  if (u.busRecordValid) {
    UnitBusRecord& br = u.busRecord;
    br.silences = (uint8_t)r.number("bsn", 0xFF);
    br.lastMinutes = r.has("bsl") ? (uint8_t)r.number("bsl", 0xFF) : 0;
    br.longestMinutes = r.has("bsx") ? (uint8_t)r.number("bsx", 0xFF) : 0;
    br.reinits = r.has("bsr") ? (uint8_t)r.number("bsr", 0xFF) : 0;
    br.reinitsHeard = r.has("bsh") ? (uint8_t)r.number("bsh", 0xFF) : 0;
    br.selfRestarts = r.has("bss") ? (uint8_t)r.number("bss", 0xFF) : 0;
    br.flags = r.has("bsf") ? (uint8_t)r.number("bsf", 0xFF) : 0;
    u.busSilentNowMinutes = r.has("bsq") ? (uint8_t)r.number("bsq", 0xFF) : 0;
  }
  if (u.state == 1) {
    u.lastSeenMs = nowMs - (r.has("age") ? r.number("age", 0xFFFFFFFFUL) : 0);
    // Without a status read "fl" is absent, and "hs2" is what is left of it.
    if (!u.statusValid && r.has("hs2")) {
      const uint32_t homing = r.number("hs2", 2);
      u.status.flags = homing == 2 ? UNIT_FLAG_HOMED : homing == 1 ? UNIT_FLAG_MOVING : 0;
    }
    if (r.has("misses")) u.misses = (uint8_t)r.number("misses", 0xFF);
    u.stale = r.flag("stale");
    if (r.has("err")) {
      u.i2cErrors = (uint16_t)r.number("err", 0xFFFF);
      u.lastErrorMs = nowMs - r.number("errAge", 0xFFFFFFFFUL);
    }
    if (r.has("rsx")) u.rescueExits = (uint16_t)r.number("rsx", 0xFFFF);
  }
  if (r.has("bv")) {
    u.bootVerdict = (uint8_t)r.number("bv", 0xFF);
    if (r.has("bcrc")) u.bootCrc32 = r.hex("bcrc", 8);
  }
  if (r.has("blv")) {
    TwibootIdentity& b = u.bootloader;
    b.generation = (uint8_t)r.number("blv", 0xFF);
    b.caps = (uint8_t)r.number("blc", 0xFF);
    if (r.has("blk")) {
      b.fusesValid = true;
      b.lock = (uint8_t)r.hex("blk", 2);
      const uint32_t fuses = r.hex("blf", 6);
      b.lfuse = (uint8_t)(fuses >> 16);
      b.hfuse = (uint8_t)(fuses >> 8);
      b.efuse = (uint8_t)fuses;
    }
    if (r.has("blx")) {
      b.crashValid = true;
      b.crashCount = (uint8_t)r.number("blx", 0xFF);
    }
  }
  return r.ok;
}

}  // namespace unitfactsjson

// False when `json` is not a whole unit facts document; `out` is then not to
// be used. `allocator` is where ArduinoJson builds its tree (the document can
// be 9 KB of text); nullptr = the default heap.
inline bool unitFactsFromJson(const char* json, size_t length, uint32_t nowMs, UnitFactsDoc& out,
                              ArduinoJson::Allocator* allocator = nullptr) {
  JsonDocument doc = allocator ? JsonDocument(allocator) : JsonDocument();
  if (deserializeJson(doc, json, length) != DeserializationError::Ok) return false;
  JsonObjectConst root = doc.as<JsonObjectConst>();
  if (root.isNull()) return false;
  unitfactsjson::Reader r{root};
  out.width = (int)r.number("width", UNITS_AMOUNT);
  out.faulty = (int)r.number("faulty", UNITS_AMOUNT);
  JsonArrayConst units = root["units"].as<JsonArrayConst>();
  if (!r.ok || units.isNull() || (int)units.size() != out.width) return false;
  int index = 0;
  for (JsonVariantConst unit : units) {
    JsonObjectConst o = unit.as<JsonObjectConst>();
    if (o.isNull() || !unitfactsjson::readUnit(o, index, nowMs, out.units[index])) return false;
    index++;
  }
  for (; index < UNITS_AMOUNT; index++) out.units[index] = UnitFacts{};
  return true;
}
