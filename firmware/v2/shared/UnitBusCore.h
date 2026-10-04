#pragma once
// UnitBusCore.h — the sketch-side I2C protocol a row master speaks to a Nano
// unit, once: every query and its validation, every single-unit op with its
// verify, and the folds of what was read into a unit's facts. Function
// templates over the tree's bus adapter (the one TwibootFlash.h runs on), so
// the S3 and the ESP-01 put the same bytes on the wire and judge the replies
// the same way; each tree keeps its orchestration, logging and scheduling.
// Natively tested by test_unit_bus_core against a scripted unit.
//
// The adapter, beyond what TwibootFlash.h needs:
//   int  endCounted()         end a sketch-protocol write transaction and
//                             count it (status 0 = ACKed)
//   void noteReadError()      count a failed read leg
//   void mark(act, addr)      breadcrumb of what the bus is about to do
//   void readFailed()         a read came back short — the S3 must rebuild
//                             its I2C driver before the bus is touched again

#include <stdint.h>

#include "BootIntegrity.h"
#include "BootUpdateReport.h"
#include "DriftLogPolicy.h"
#include "MaintenancePolicy.h"
#include "SplitFlapProtocol.h"
#include "UnitHealth.h"
#include "UnitProtocolHelpers.h"
#include "UnitWireContract.h"

// What the bus is about to do, for the adapter's breadcrumb.
enum class UnitBusAct : uint8_t { Write = 0, Read, Probe };

// Between an opcode write and clocking the reply, so the unit's receive ISR
// has time to stage its response.
#define UNIT_RESPONSE_SETTLE_MS 2

// --- per-unit error attribution (#367) --------------------------------------------

// The bus-wide error count cannot say WHICH unit's transactions fail. This
// charges a failed render write or a failed status read to its column, and
// folds the tally into the facts so /units/health shows err / errAge per
// unit. Lifetime since boot — deliberately NOT reset by a probe rescan (a
// reliability trend, unlike the re-baselined health masks).
template <int N>
struct UnitErrorLedger {
  uint16_t count[N] = {0};
  uint32_t lastMs[N] = {0};

  void note(int index, uint32_t nowMs) {
    if (index < 0 || index >= N) return;
    count[index] = unitErrBump(count[index]);
    lastMs[index] = nowMs;
  }
  void fold(UnitFacts* facts, int n) const {
    if (n > N) n = N;
    for (int i = 0; i < n; i++) fold(facts[i], i);
  }
  void fold(UnitFacts& fact, int index) const {
    if (index < 0 || index >= N) return;
    fact.i2cErrors = count[index];
    fact.lastErrorMs = lastMs[index];
  }
};

// --- framing -------------------------------------------------------------------

// No-argument mutations go out as opcode + ~opcode (#512, UnitWireContract.h):
// alone on the wire, the opcode byte is a complete command one bit flip away
// from a letter write or a poll. Units predating the guard drain the extra
// byte. Not for ENTER_BOOTLOADER — see unitEnterBootloader.
template <typename Bus>
inline int unitSendGuarded(Bus& bus, uint8_t i2cAddress, uint8_t opcode) {
  bus.mark(UnitBusAct::Write, i2cAddress);
  bus.beginTransmission(i2cAddress);
  bus.write(opcode);
  bus.write(noArgGuardByte(opcode));
  return bus.endCounted();
}

// opcode + payload in one transaction.
template <typename Bus>
inline int unitSendPayload(Bus& bus, uint8_t i2cAddress, uint8_t opcode,
                           const uint8_t* payload, uint8_t len) {
  bus.mark(UnitBusAct::Write, i2cAddress);
  bus.beginTransmission(i2cAddress);
  bus.write(opcode);
  for (uint8_t i = 0; i < len; i++) bus.write(payload[i]);
  return bus.endCounted();
}

// The opcode-write-then-read-back transaction behind every read: write the
// opcode, settle, clock `n` bytes into `buf`. Firmware that predates an
// opcode drops the write (the opcode namespace is reserved) but answers reads
// with its 1-byte rotation status, so a short reply means "unsupported" —
// drain and fail. `buf` holds all `n` bytes only on success.
template <typename Bus>
inline bool unitQuery(Bus& bus, uint8_t i2cAddress, uint8_t opcode,
                      uint8_t* buf, uint8_t n) {
  bus.mark(UnitBusAct::Write, i2cAddress);
  bus.beginTransmission(i2cAddress);
  bus.write(opcode);
  if (bus.endCounted() != 0) return false;
  bus.sleepMs(UNIT_RESPONSE_SETTLE_MS);
  bus.mark(UnitBusAct::Read, i2cAddress);
  uint8_t got = bus.requestFrom(i2cAddress, n);
  if (got != n) {
    while (bus.available()) bus.read();
    bus.noteReadError();
    bus.readFailed();
    return false;
  }
  for (uint8_t i = 0; i < n; i++) buf[i] = (uint8_t)bus.read();
  return true;
}

// --- reads -----------------------------------------------------------------------
// Each returns true only for a reply that passed its checksum / range check;
// `out` is untouched otherwise.

// The 8-byte health payload + checksum (#405). The highest-frequency read on
// the bus; it feeds fault flags, reset counts, uptime and stale detection.
template <typename Bus>
inline bool unitReadStatus(Bus& bus, uint8_t i2cAddress, UnitStatus& out) {
  uint8_t buf[STATUS_REPLY_LEN];
  if (!unitQuery(bus, i2cAddress, (uint8_t)SFP_CMD_GET_STATUS, buf,
                 STATUS_REPLY_LEN)) {
    return false;
  }
  uint8_t p[STATUS_PAYLOAD_LEN];
  if (!statusReadbackValid(buf, STATUS_REPLY_LEN, p)) return false;
  out.flags = p[0];
  out.mcusrAtBoot = p[1];
  out.lifetimeBrownoutCount = p[2];
  out.lifetimeWatchdogCount = p[3];
  out.uptimeSeconds = ((uint16_t)p[4] << 8) | (uint16_t)p[5];
  out.badCommandCount = p[6];
  // Byte 7 is last-homing-step / 16 (saturating); decode by reversing.
  out.lastHomingStepCount = (uint16_t)p[7] << 4;
  return true;
}

template <typename Bus>
inline bool unitReadOdometer(Bus& bus, uint8_t i2cAddress, uint32_t& out) {
  uint8_t buf[5];
  if (!unitQuery(bus, i2cAddress, (uint8_t)SFP_CMD_GET_ODOMETER, buf, 5)) {
    return false;
  }
  return odometerReadbackValid(buf, out);
}

// Drift diagnostics (#263/#264): physical letter, flags, drift events.
template <typename Bus>
inline bool unitReadDiag(Bus& bus, uint8_t i2cAddress, UnitDiagReading& out) {
  uint8_t buf[6];
  if (!unitQuery(bus, i2cAddress, (uint8_t)SFP_CMD_GET_DIAG, buf, 6)) {
    return false;
  }
  return diagReadbackValid(buf, (uint8_t)SFP_FLAP_AMOUNT, out);
}

template <typename Bus>
inline bool unitReadVitals(Bus& bus, uint8_t i2cAddress, UnitVitals& out) {
  uint8_t buf[VITALS_REPLY_LEN];
  if (!unitQuery(bus, i2cAddress, (uint8_t)SFP_CMD_GET_VITALS, buf,
                 VITALS_REPLY_LEN)) {
    return false;
  }
  return vitalsReadbackValid(buf, out);
}

template <typename Bus>
inline bool unitReadLifetime(Bus& bus, uint8_t i2cAddress,
                             UnitLifetimeFacts& out) {
  uint8_t buf[LIFETIME_REPLY_LEN];
  if (!unitQuery(bus, i2cAddress, (uint8_t)SFP_CMD_GET_LIFETIME, buf,
                 LIFETIME_REPLY_LEN)) {
    return false;
  }
  return lifetimeReadbackValid(buf, LIFETIME_REPLY_LEN, out);
}

template <typename Bus>
inline bool unitReadOffset(Bus& bus, uint8_t i2cAddress, int16_t& out) {
  uint8_t buf[OFFSET_REPLY_LEN];
  if (!unitQuery(bus, i2cAddress, (uint8_t)SFP_CMD_GET_OFFSET, buf,
                 OFFSET_REPLY_LEN)) {
    return false;
  }
  return offsetReadbackValid(buf, OFFSET_REPLY_LEN, out);
}

// The unit's firmware rev (up to 8 printable ASCII bytes, null-terminated
// into `out`) and its SFP_PROTOCOL_VERSION (#405). `"` and `\` are rejected
// at this boundary: the string is emitted raw into the health JSON, and a
// real git short-rev never contains them. This reply's shape is frozen — you
// cannot ask which contract a unit speaks through an opcode whose format
// depends on the answer.
template <typename Bus>
inline bool unitReadVersion(Bus& bus, uint8_t i2cAddress, char* out,
                            uint8_t& protocolOut) {
  out[0] = '\0';
  protocolOut = 0;
  uint8_t buf[VERSION_REPLY_LEN];
  if (!unitQuery(bus, i2cAddress, (uint8_t)SFP_CMD_GET_VERSION, buf,
                 VERSION_REPLY_LEN)) {
    return false;
  }
  UnitVersionPacket pkt;
  if (!versionReadbackValid(buf, VERSION_REPLY_LEN, pkt)) return false;
  uint8_t len = 0;
  for (; len < VERSION_REV_LEN; len++) {
    if (pkt.rev[len] == 0) break;
    if (pkt.rev[len] < 32 || pkt.rev[len] > 126) return false;
    if (pkt.rev[len] == '"' || pkt.rev[len] == '\\') return false;
  }
  if (len == 0) return false;
  for (uint8_t i = 0; i < len; i++) out[i] = pkt.rev[i];
  out[len] = '\0';
  protocolOut = pkt.protocolVersion;
  return true;
}

// The letter index the unit shows (v1 #106): index + bitwise complement.
template <typename Bus>
inline bool unitReadDisplayedLetter(Bus& bus, uint8_t i2cAddress, int& out) {
  uint8_t buf[2];
  if (!unitQuery(bus, i2cAddress, (uint8_t)SFP_CMD_GET_LETTER, buf, 2)) {
    return false;
  }
  if (!letterReadbackValid(buf[0], buf[1], (uint8_t)SFP_FLAP_AMOUNT)) {
    return false;
  }
  out = buf[0];
  return true;
}

template <typename Bus>
inline bool unitReadSelfTest(Bus& bus, uint8_t i2cAddress,
                             UnitSelfTestReading& out) {
  uint8_t buf[SELFTEST_REPLY_LEN];
  if (!unitQuery(bus, i2cAddress, (uint8_t)SFP_CMD_GET_SELF_TEST, buf,
                 SELFTEST_REPLY_LEN)) {
    return false;
  }
  return selfTestReadbackValid(buf, out);
}

// Lock/fuse bytes, boot-section CRC32, classified state, last update result
// (#499). False on a unit in twiboot or on firmware predating the opcode.
template <typename Bus>
inline bool unitReadBootInfo(Bus& bus, uint8_t i2cAddress,
                             BootUpdateReport& out) {
  uint8_t buf[BOOT_INFO_REPLY_LEN];
  if (!unitQuery(bus, i2cAddress, (uint8_t)SFP_CMD_GET_BOOT_INFO, buf,
                 BOOT_INFO_REPLY_LEN)) {
    return false;
  }
  return bootInfoDecode(buf, out);
}

// The 1-byte rotation status every unit answers a bare read with: 1 moving,
// 0 idle, -1 no answer. Polled ~10x/s while a row moves, so it is not counted
// as a transaction. A silent unit gets an empty transmission as a wake-up
// pulse for its TWI peripheral — after the adapter's read-failure recovery,
// because that pulse is exactly the probe-after-failed-read shape (#207).
template <typename Bus>
inline int unitMovingStatus(Bus& bus, uint8_t i2cAddress) {
  bus.mark(UnitBusAct::Read, i2cAddress);
  bus.requestFrom(i2cAddress, (uint8_t)1);
  int active = bus.available() ? bus.read() : -1;
  if (active == -1) {
    bus.readFailed();
    bus.beginTransmission(i2cAddress);
    bus.endTransmission(true);
  }
  return active;
}

// Waits until every listed unit is back in its sketch and idle: restarted
// units answer 0 only once their sketch runs and their homing has finished
// (a unit still in twiboot answers 0xFF, a silent one -1 — neither is idle).
// The leading second lets a just-sent restart take effect before the first
// poll. Returns false on timeout. `keepAlive()` runs once per poll round.
template <typename Bus, typename KeepAlive>
inline bool unitWaitBatchIdle(Bus& bus, const uint8_t* addrs, int count,
                              uint32_t timeoutMs, KeepAlive&& keepAlive) {
  if (count <= 0) return true;
  bus.sleepMs(1000);
  uint32_t start = bus.nowMs();
  while ((uint32_t)(bus.nowMs() - start) < timeoutMs) {
    keepAlive();
    bool allIdle = true;
    for (int k = 0; k < count; k++) {
      if (unitMovingStatus(bus, addrs[k]) != 0) {
        allIdle = false;
        break;
      }
    }
    if (allIdle) return true;
    bus.sleepMs(100);
  }
  return false;
}

// --- folds into a unit's facts ---------------------------------------------------
// Each clears its valid flag first, so a unit that stops answering (or was
// reflashed to firmware predating the opcode) never keeps serving a stale
// reading.

// Returns what the drift counter did since the last look (#322): the unit's
// auto re-home is otherwise silent, so the tree logs one line when
// `shouldLog`. `reading` is valid only when the fact's diagValid is.
template <typename Bus>
inline DriftLogDecision unitRefreshDiag(Bus& bus, UnitFacts& fact,
                                        uint8_t i2cAddress,
                                        UnitDiagReading& reading) {
  fact.diagValid = false;
  if (!unitReadDiag(bus, i2cAddress, reading)) {
    return DriftLogDecision{false, 0, fact.driftEventsBaseline};
  }
  DriftLogDecision drift =
      driftLogEvaluate(fact.driftEventsBaseline, reading.driftEvents);
  fact.driftEventsBaseline = drift.newBaseline;
  fact.physLetter = reading.physicalLetter;
  fact.driftFlags = reading.flags;
  fact.driftEvents = reading.driftEvents;
  fact.lastDriftSteps = reading.lastDriftSteps;
  fact.diagValid = true;
  return drift;
}

template <typename Bus>
inline void unitRefreshVitals(Bus& bus, UnitFacts& fact, uint8_t i2cAddress) {
  fact.vitalsValid = false;
  UnitVitals v;
  if (!unitReadVitals(bus, i2cAddress, v)) return;
  fact.vitals = v;
  fact.vitalsValid = true;
}

// 11-byte base + 10-byte link extension (#502), each with its own checksum.
template <typename Bus>
inline void unitRefreshExtDiag(Bus& bus, UnitFacts& fact, uint8_t i2cAddress) {
  fact.extDiagValid = false;
  fact.linkValid = false;
  uint8_t buf[EXT_DIAG_LINK_REPLY_LEN];
  if (!unitQuery(bus, i2cAddress, (uint8_t)SFP_CMD_GET_EXT_DIAG, buf,
                 EXT_DIAG_LINK_REPLY_LEN)) {
    return;
  }
  unitFactsFoldExtDiag(fact, buf);
}

template <typename Bus>
inline void unitRefreshLifetime(Bus& bus, UnitFacts& fact, uint8_t i2cAddress) {
  fact.lifetimeValid = false;
  UnitLifetimeFacts lt;
  if (!unitReadLifetime(bus, i2cAddress, lt)) return;
  fact.lifetime = lt;
  fact.lifetimeValid = true;
}

// Boot-section integrity (BootIntegrity.h, #520), judged on every health
// poll. `logged` is the tree's per-unit memory of the last verdict it logged
// — kept outside the facts so a probe rescan, which rebuilds them, does not
// repeat a finding. Returns true when the tree should log; `report` is then
// what the unit said.
template <typename Bus>
inline bool unitRefreshBootVerdict(Bus& bus, UnitFacts& fact,
                                   uint8_t i2cAddress, uint8_t& logged,
                                   BootUpdateReport& report) {
  fact.bootVerdict = BOOT_INTEGRITY_UNREAD;
  if (!unitReadBootInfo(bus, i2cAddress, report)) return false;
  fact.bootCrc32 = report.bootCrc32;
  fact.bootVerdict = bootIntegrityJudge(report, BOOT_CURRENT_CRC32);
  BootIntegrityEdge e = bootIntegrityEdge(logged, fact.bootVerdict);
  logged = e.logged;
  return e.log;
}

// What a probe learns from a unit it found running its sketch: firmware rev
// graded against the bundle, protocol version, calibration offset. Returns
// whether the version was readable.
template <typename Bus>
inline bool unitReadIdentity(Bus& bus, UnitFacts& fact, uint8_t i2cAddress,
                             const char* bundledRev,
                             const char* bundledEquivRevs) {
  uint8_t protocolVersion = 0;
  bool versionOk =
      unitReadVersion(bus, i2cAddress, fact.version, protocolVersion);
  if (versionOk) {
    fact.fwStatus =
        unitFwStatusFromRev(fact.version, bundledRev, bundledEquivRevs);
    fact.protocolVersion = protocolVersion;
    fact.protocolKnown = true;
  }
  int16_t offset;
  if (unitReadOffset(bus, i2cAddress, offset)) {
    unitFactsApplyOffsetWrite(fact, offset);
  }
  return versionOk;
}

// The status half of a health poll: flags, reset counts, uptime, and whether
// a reset happened while this master was watching (#502). Returns the
// CMD_GET_STATUS liveness signal the heartbeat counts.
template <typename Bus>
inline bool unitPollStatus(Bus& bus, UnitFacts& fact, uint8_t i2cAddress,
                           UnitResetBaseline& baseline) {
  fact.statusValid = false;
  UnitStatus s;
  if (!unitReadStatus(bus, i2cAddress, s)) return false;
  fact.status = s;
  fact.statusValid = true;
  fact.resetSeen = unitResetBaselineFold(baseline, s.lifetimeBrownoutCount,
                                         s.lifetimeWatchdogCount);
  return true;
}

template <typename Bus>
inline void unitRefreshOdometer(Bus& bus, UnitFacts& fact, uint8_t i2cAddress) {
  uint32_t odometer;
  if (unitReadOdometer(bus, i2cAddress, odometer)) {
    fact.odometer = odometer;
    fact.odometerValid = true;
  }
}

// --- what a probe and a health poll read ------------------------------------------
// `Notes` is the tree's log sink — the shared code decides WHAT is worth a
// line, the tree words it (the ESP-01 keeps its text in flash):
//   void identityRead(uint8_t addr, const UnitFacts&, bool versionReadable)
//        (a probe only; called before the diagnostics so a scan-log entry is
//        complete before any other line can follow it)
//   void driftSeen(uint8_t addr, const DriftLogDecision&, const UnitDiagReading&)
//   void bootVerdictChanged(uint8_t addr, const UnitFacts&, const BootUpdateReport&)
// `bootLogged` is the tree's per-unit memory for unitRefreshBootVerdict.

// Everything beyond the status that both a probe and a poll refresh:
// odometer, drift diagnostics, vitals, ext-diag, lifetime, boot verdict.
// Firmware predating an opcode fails its checksum and the matching valid flag
// stays false. None of these is a liveness signal: they fail routinely on old
// firmware and must not be charged to a unit as bus errors (#367).
template <typename Bus, typename Notes>
inline void unitRefreshDiagnostics(Bus& bus, Notes& notes, UnitFacts& fact,
                                   uint8_t i2cAddress, uint8_t& bootLogged) {
  unitRefreshOdometer(bus, fact, i2cAddress);
  UnitDiagReading diag;
  DriftLogDecision drift = unitRefreshDiag(bus, fact, i2cAddress, diag);
  if (drift.shouldLog) notes.driftSeen(i2cAddress, drift, diag);
  unitRefreshVitals(bus, fact, i2cAddress);
  unitRefreshExtDiag(bus, fact, i2cAddress);
  unitRefreshLifetime(bus, fact, i2cAddress);
  BootUpdateReport report;
  if (unitRefreshBootVerdict(bus, fact, i2cAddress, bootLogged, report)) {
    notes.bootVerdictChanged(i2cAddress, fact, report);
  }
}

// A unit the probe found running its sketch: identity (rev graded against
// the bundle, protocol, offset), then the diagnostics. Returns whether the
// version was readable. Both the rev (a hash) and the protocol version are
// compared for EQUALITY only: neither says "older", just "not ours". An
// unreadable version stays "unknown" — a unit we simply cannot read must not
// trigger a reflash cycle at every power-up (v1 #114).
template <typename Bus, typename Notes>
inline bool unitProbeSketchUnit(Bus& bus, Notes& notes, UnitFacts& fact,
                                uint8_t i2cAddress, const char* bundledRev,
                                const char* bundledEquivRevs,
                                uint8_t& bootLogged) {
  bool versionOk =
      unitReadIdentity(bus, fact, i2cAddress, bundledRev, bundledEquivRevs);
  notes.identityRead(i2cAddress, fact, versionOk);
  unitRefreshDiagnostics(bus, notes, fact, i2cAddress, bootLogged);
  return versionOk;
}

// One unit's health poll: the status read (the heartbeat's liveness signal,
// returned) and the diagnostics on the same cadence.
template <typename Bus, typename Notes>
inline bool unitPollHealth(Bus& bus, Notes& notes, UnitFacts& fact,
                           uint8_t i2cAddress, UnitResetBaseline& baseline,
                           uint8_t& bootLogged) {
  bool ok = unitPollStatus(bus, fact, i2cAddress, baseline);
  unitRefreshDiagnostics(bus, notes, fact, i2cAddress, bootLogged);
  return ok;
}

// --- writes ----------------------------------------------------------------------
// Each returns the transaction status (0 = ACKed) unless noted.

// Letter index + wire speed: a render's one write per unit.
template <typename Bus>
inline int unitWriteLetter(Bus& bus, uint8_t i2cAddress, uint8_t letter,
                           uint8_t unitSpeed) {
  bus.mark(UnitBusAct::Write, i2cAddress);
  bus.beginTransmission(i2cAddress);
  bus.write(letter);
  bus.write(unitSpeed);
  return bus.endCounted();
}

// Persists the calibration offset and reads it back (#405): returns
// UNIT_BUS_OFFSET_UNVERIFIED / _MISMATCH instead of 0 when the write did not
// demonstrably land.
template <typename Bus>
inline int unitWriteOffset(Bus& bus, uint8_t i2cAddress, int16_t value) {
  uint8_t payload[SET_OFFSET_PAYLOAD_LEN];
  setOffsetEncode(value, payload);
  int status = unitSendPayload(bus, i2cAddress, (uint8_t)SFP_CMD_SET_OFFSET,
                               payload, SET_OFFSET_PAYLOAD_LEN);
  if (status != 0) return status;
  bus.sleepMs(UNIT_OFFSET_WRITE_SETTLE_MS);
  int16_t readBack = 0;
  if (!unitReadOffset(bus, i2cAddress, readBack)) {
    return UNIT_BUS_OFFSET_UNVERIFIED;
  }
  return readBack == value ? 0 : UNIT_BUS_OFFSET_MISMATCH;
}

// ±127 steps, not persisted.
template <typename Bus>
inline int unitJog(Bus& bus, uint8_t i2cAddress, int steps) {
  uint8_t payload[JOG_PAYLOAD_LEN];
  jogEncode(maintEncodeJogByte(steps), payload);
  return unitSendPayload(bus, i2cAddress, (uint8_t)SFP_CMD_JOG, payload,
                         JOG_PAYLOAD_LEN);
}

template <typename Bus>
inline int unitHome(Bus& bus, uint8_t i2cAddress) {
  return unitSendGuarded(bus, i2cAddress, SFP_CMD_HOME);
}

template <typename Bus>
inline int unitIdentify(Bus& bus, uint8_t i2cAddress) {
  return unitSendGuarded(bus, i2cAddress, SFP_CMD_IDENTIFY);
}

template <typename Bus>
inline int unitResetOdometer(Bus& bus, uint8_t i2cAddress) {
  return unitSendGuarded(bus, i2cAddress, SFP_CMD_RESET_ODOMETER);
}

template <typename Bus>
inline int unitStartSelfTest(Bus& bus, uint8_t i2cAddress) {
  return unitSendGuarded(bus, i2cAddress, SFP_CMD_START_SELF_TEST);
}

// A clean watchdog restart of a unit running its sketch (v1 #113).
template <typename Bus>
inline int unitRebootSketch(Bus& bus, uint8_t i2cAddress) {
  return unitSendGuarded(bus, i2cAddress, SFP_CMD_REBOOT);
}

template <typename Bus>
inline int unitClearAddress(Bus& bus, uint8_t i2cAddress) {
  return unitSendGuarded(bus, i2cAddress, SFP_CMD_CLEAR_I2C_ADDRESS);
}

// Persists the unit's feature-gate byte (#409) and reads GET_LIFETIME back:
// returns UNIT_BUS_GATES_UNVERIFIED / _MISMATCH instead of 0 when it did not
// demonstrably land. A unit refuses bits it has no code for, so a refused
// write surfaces as _MISMATCH rather than as a silent success.
template <typename Bus>
inline int unitSetGates(Bus& bus, uint8_t i2cAddress, uint8_t gates) {
  uint8_t payload[SET_GATES_PAYLOAD_LEN];
  setGatesEncode(gates, payload);
  int status = unitSendPayload(bus, i2cAddress, (uint8_t)SFP_CMD_SET_GATES,
                               payload, SET_GATES_PAYLOAD_LEN);
  if (status != 0) return status;
  bus.sleepMs(UNIT_GATES_WRITE_SETTLE_MS);
  UnitLifetimeFacts lt;
  if (!unitReadLifetime(bus, i2cAddress, lt)) return UNIT_BUS_GATES_UNVERIFIED;
  return lt.featureGates == gates ? 0 : UNIT_BUS_GATES_MISMATCH;
}

// In-system twiboot update (#499): stage (1 or 2) + its complement.
template <typename Bus>
inline int unitSendBootUpdate(Bus& bus, uint8_t i2cAddress, uint8_t stage) {
  uint8_t payload[2] = {stage, (uint8_t)~stage};
  return unitSendPayload(bus, i2cAddress, (uint8_t)SFP_CMD_BOOT_UPDATE,
                         payload, 2);
}

// Resets the unit into twiboot, which listens ~1 s on its DIP address. Sent
// BARE, without the #512 guard byte: the one-byte form is the fixed-forever
// one every unit accepts, and a unit already sitting in twiboot ACKs exactly
// one byte — a guard byte would turn that status into a NACK. Never probe
// while a unit can be in that window (UnitTimings.h).
template <typename Bus>
inline int unitEnterBootloader(Bus& bus, uint8_t i2cAddress) {
  return unitSendPayload(bus, i2cAddress, (uint8_t)SFP_CMD_ENTER_BOOTLOADER,
                         nullptr, 0);
}

// Burns a new EEPROM I2C address; the unit reboots onto it.
template <typename Bus>
inline int unitSetAddress(Bus& bus, uint8_t i2cAddress, uint8_t newAddress) {
  uint8_t payload[SET_ADDRESS_PAYLOAD_LEN];
  setAddressEncode(newAddress, payload);
  return unitSendPayload(bus, i2cAddress, (uint8_t)SFP_CMD_SET_I2C_ADDRESS,
                         payload, SET_ADDRESS_PAYLOAD_LEN);
}
