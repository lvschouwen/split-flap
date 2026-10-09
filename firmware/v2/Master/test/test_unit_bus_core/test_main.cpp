// Host-side tests for shared/UnitBusCore.h — the sketch-side I2C protocol
// both row masters speak. The bus is a scripted Nano unit built from the same
// encoders the real unit uses, so framing, verification and the folds into
// UnitFacts are exercised without hardware.

#include <ArduinoFake.h>
#include <string.h>
#include <unity.h>

#include <vector>

#include "UnitBusCore.h"
#include "UnitExtDiag.h"
#include "UnitLifetime.h"
#include "UnitSelfTest.h"
#include "UnitVitals.h"

void setUp() {}
void tearDown() {}

namespace {

const uint8_t ADDR = 0x05;
const int NACK = 2;

struct FakeUnit {
  // --- device model ---
  bool present = true;
  bool oldFirmware = false;  // answers every query with its 1-byte status
  uint8_t moving = 0;
  int16_t offset = 54;
  bool offsetWriteSticks = true;
  uint8_t gates = 0x01;
  bool gatesWriteSticks = true;
  uint32_t odometer = 1234;
  const char* rev = "34c72e0";
  uint8_t protocol = SFP_PROTOCOL_VERSION;
  uint8_t shownLetter = 7;
  uint8_t statusPayload[STATUS_PAYLOAD_LEN] = {0x20, 0, 1, 2, 0x01, 0x2C, 3, 36};
  int corruptReplies = 0;  // this many replies get one byte flipped
  int shortReplies = 0;    // this many replies come back one byte short

  // --- observations ---
  std::vector<std::vector<uint8_t>> writes;  // every completed write
  int counted = 0;
  int readErrors = 0;
  int readFailedCalls = 0;
  int wakePings = 0;
  uint32_t now = 0;

  // --- bus state ---
  std::vector<uint8_t> tx;
  uint8_t txAddr = 0;
  uint8_t pendingOpcode = 0xFF;
  std::vector<uint8_t> rx;
  size_t rxPos = 0;

  void beginTransmission(uint8_t addr) {
    txAddr = addr;
    tx.clear();
  }
  size_t write(uint8_t b) {
    tx.push_back(b);
    return 1;
  }
  int finish() {
    if (!present || txAddr != ADDR) return NACK;
    if (tx.empty()) {
      wakePings++;
      return 0;
    }
    writes.push_back(tx);
    apply(tx);
    return 0;
  }
  int endTransmission(bool) { return finish(); }
  int endCounted() {
    counted++;
    return finish();
  }
  void noteReadError() { readErrors++; }
  void readFailed() { readFailedCalls++; }
  void mark(UnitBusAct, uint8_t) {}
  uint32_t nowMs() { return now; }
  void sleepMs(uint32_t ms) { now += ms; }
  int available() { return (int)(rx.size() - rxPos); }
  int read() { return rxPos < rx.size() ? rx[rxPos++] : -1; }

  void apply(const std::vector<uint8_t>& w) {
    uint8_t op = w[0];
    pendingOpcode = op;
    if (oldFirmware) return;
    if (op == SFP_CMD_SET_OFFSET && w.size() == 1 + SET_OFFSET_PAYLOAD_LEN) {
      if (offsetWriteSticks) {
        offset = (int16_t)((uint16_t)w[1] | ((uint16_t)w[2] << 8));
      }
    } else if (op == SFP_CMD_SET_GATES && w.size() == 1 + SET_GATES_PAYLOAD_LEN) {
      if (gatesWriteSticks) gates = w[1];
    }
  }

  uint8_t requestFrom(uint8_t addr, uint8_t qty) {
    rx.clear();
    rxPos = 0;
    if (!present || addr != ADDR) return 0;
    std::vector<uint8_t> reply = replyFor(pendingOpcode);
    pendingOpcode = 0xFF;
    if (oldFirmware || reply.empty()) reply = {moving};
    if (shortReplies > 0 && reply.size() > 1) {
      shortReplies--;
      reply.pop_back();
    }
    if (corruptReplies > 0) {
      corruptReplies--;
      reply[0] ^= 0x10;
    }
    for (uint8_t i = 0; i < qty && i < reply.size(); i++) rx.push_back(reply[i]);
    return (uint8_t)rx.size();
  }

  std::vector<uint8_t> replyFor(uint8_t op) {
    std::vector<uint8_t> out;
    switch (op) {
      case SFP_CMD_GET_STATUS: {
        out.resize(STATUS_REPLY_LEN);
        statusEncodeReply(statusPayload, out.data());
        break;
      }
      case SFP_CMD_GET_OFFSET: {
        out.resize(OFFSET_REPLY_LEN);
        offsetEncodeReply(offset, out.data());
        break;
      }
      case SFP_CMD_GET_VERSION: {
        out.resize(VERSION_REPLY_LEN);
        versionEncodeReply(rev, protocol, out.data());
        break;
      }
      case SFP_CMD_GET_ODOMETER: {
        out = {(uint8_t)odometer, (uint8_t)(odometer >> 8),
               (uint8_t)(odometer >> 16), (uint8_t)(odometer >> 24), 0};
        uint8_t x = 0;
        for (int i = 0; i < 4; i++) x ^= out[i];
        out[4] = (uint8_t)(x ^ 0xA5);
        break;
      }
      case SFP_CMD_GET_LETTER:
        out = {shownLetter, (uint8_t)~shownLetter};
        break;
      case SFP_CMD_GET_LIFETIME: {
        UnitLifetimeFacts f{};
        f.featureGates = gates;
        out.resize(LIFETIME_REPLY_LEN);
        lifetimeEncodeReply(f, out.data());
        break;
      }
      case SFP_CMD_GET_VITALS: {
        UnitVitals v{};
        v.vccNow_mV = 4913;
        out.resize(VITALS_REPLY_LEN);
        vitalsEncodeReply(v, out.data());
        break;
      }
      case SFP_CMD_GET_SELF_TEST: {
        SelfTestResult r;
        r.state = SELFTEST_STATE_OK;
        r.stepsPerRev = 2050;
        out.resize(SELFTEST_REPLY_LEN);
        selfTestEncodeReply(r, out.data());
        break;
      }
      case SFP_CMD_GET_BOOT_INFO: {
        BootUpdateReport r;
        r.state = BOOT_STATE_NEW;
        r.bootCrc32 = BOOT_CURRENT_CRC32;
        out.resize(BOOT_INFO_REPLY_LEN);
        bootInfoEncode(r, out.data());
        break;
      }
      default:
        break;
    }
    return out;
  }
};

}  // namespace

// --- framing ----------------------------------------------------------------

static void test_no_argument_mutations_carry_the_guard_byte() {
  FakeUnit u;
  TEST_ASSERT_EQUAL(0, unitHome(u, ADDR));
  TEST_ASSERT_EQUAL(0, unitIdentify(u, ADDR));
  TEST_ASSERT_EQUAL(0, unitResetOdometer(u, ADDR));
  TEST_ASSERT_EQUAL(0, unitStartSelfTest(u, ADDR));
  TEST_ASSERT_EQUAL(0, unitRebootSketch(u, ADDR));
  TEST_ASSERT_EQUAL(0, unitClearAddress(u, ADDR));
  const uint8_t ops[] = {SFP_CMD_HOME,           SFP_CMD_IDENTIFY,
                         SFP_CMD_RESET_ODOMETER, SFP_CMD_START_SELF_TEST,
                         SFP_CMD_REBOOT,         SFP_CMD_CLEAR_I2C_ADDRESS};
  TEST_ASSERT_EQUAL(6, (int)u.writes.size());
  for (int i = 0; i < 6; i++) {
    TEST_ASSERT_EQUAL(2, (int)u.writes[i].size());
    TEST_ASSERT_EQUAL_UINT8(ops[i], u.writes[i][0]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)~ops[i], u.writes[i][1]);
  }
  TEST_ASSERT_EQUAL(6, u.counted);
}

// A unit already in twiboot ACKs exactly one byte: this opcode goes out bare.
static void test_enter_bootloader_is_one_bare_byte() {
  FakeUnit u;
  TEST_ASSERT_EQUAL(0, unitEnterBootloader(u, ADDR));
  TEST_ASSERT_EQUAL(1, (int)u.writes[0].size());
  TEST_ASSERT_EQUAL_UINT8(SFP_CMD_ENTER_BOOTLOADER, u.writes[0][0]);
}

static void test_letter_write_is_index_and_speed() {
  FakeUnit u;
  TEST_ASSERT_EQUAL(0, unitWriteLetter(u, ADDR, 12, 9));
  TEST_ASSERT_EQUAL(2, (int)u.writes[0].size());
  TEST_ASSERT_EQUAL_UINT8(12, u.writes[0][0]);
  TEST_ASSERT_EQUAL_UINT8(9, u.writes[0][1]);
}

static void test_boot_update_stage_carries_its_complement() {
  FakeUnit u;
  unitSendBootUpdate(u, ADDR, 2);
  TEST_ASSERT_EQUAL(3, (int)u.writes[0].size());
  TEST_ASSERT_EQUAL_UINT8(SFP_CMD_BOOT_UPDATE, u.writes[0][0]);
  TEST_ASSERT_EQUAL_UINT8(2, u.writes[0][1]);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)~2, u.writes[0][2]);
}

static void test_a_silent_unit_nacks_every_write() {
  FakeUnit u;
  u.present = false;
  TEST_ASSERT_EQUAL(NACK, unitHome(u, ADDR));
  TEST_ASSERT_EQUAL(NACK, unitWriteOffset(u, ADDR, 10));
  TEST_ASSERT_EQUAL(NACK, unitSetGates(u, ADDR, 1));
}

// --- reads ------------------------------------------------------------------

static void test_status_read_decodes_the_payload() {
  FakeUnit u;
  UnitStatus s;
  TEST_ASSERT_TRUE(unitReadStatus(u, ADDR, s));
  TEST_ASSERT_EQUAL_UINT8(0x20, s.flags);
  TEST_ASSERT_EQUAL_UINT8(1, s.lifetimeBrownoutCount);
  TEST_ASSERT_EQUAL_UINT8(2, s.lifetimeWatchdogCount);
  TEST_ASSERT_EQUAL_UINT16(300, s.uptimeSeconds);
  TEST_ASSERT_EQUAL_UINT16(36 << 4, s.lastHomingStepCount);
}

static void test_a_corrupted_reply_is_rejected_not_believed() {
  FakeUnit u;
  u.corruptReplies = 1;
  UnitStatus s;
  s.flags = 0x7F;
  TEST_ASSERT_FALSE(unitReadStatus(u, ADDR, s));
  TEST_ASSERT_EQUAL_UINT8(0x7F, s.flags);  // untouched
  // The transaction itself worked: no read error, no bus recovery.
  TEST_ASSERT_EQUAL(0, u.readErrors);
  TEST_ASSERT_EQUAL(0, u.readFailedCalls);
}

static void test_a_short_reply_counts_an_error_and_recovers_the_bus() {
  FakeUnit u;
  u.shortReplies = 1;
  int16_t offset = 0;
  TEST_ASSERT_FALSE(unitReadOffset(u, ADDR, offset));
  TEST_ASSERT_EQUAL(1, u.readErrors);
  TEST_ASSERT_EQUAL(1, u.readFailedCalls);
  TEST_ASSERT_EQUAL(0, u.available());  // drained
}

// Firmware predating an opcode answers with its 1-byte rotation status.
static void test_old_firmware_reads_as_unsupported_everywhere() {
  FakeUnit u;
  u.oldFirmware = true;
  UnitVitals v;
  UnitLifetimeFacts lt;
  BootUpdateReport r;
  UnitSelfTestReading st;
  uint32_t odo;
  TEST_ASSERT_FALSE(unitReadVitals(u, ADDR, v));
  TEST_ASSERT_FALSE(unitReadLifetime(u, ADDR, lt));
  TEST_ASSERT_FALSE(unitReadBootInfo(u, ADDR, r));
  TEST_ASSERT_FALSE(unitReadSelfTest(u, ADDR, st));
  TEST_ASSERT_FALSE(unitReadOdometer(u, ADDR, odo));
}

static void test_version_read_yields_rev_and_protocol() {
  FakeUnit u;
  char rev[VERSION_REV_LEN + 1];
  uint8_t protocol = 0;
  TEST_ASSERT_TRUE(unitReadVersion(u, ADDR, rev, protocol));
  TEST_ASSERT_EQUAL_STRING("34c72e0", rev);
  TEST_ASSERT_EQUAL_UINT8(SFP_PROTOCOL_VERSION, protocol);
}

// The rev goes raw into the health JSON: a quote must never get through.
static void test_version_with_a_json_breaking_character_is_rejected() {
  FakeUnit u;
  u.rev = "ab\"cdef";
  char rev[VERSION_REV_LEN + 1];
  uint8_t protocol = 9;
  TEST_ASSERT_FALSE(unitReadVersion(u, ADDR, rev, protocol));
  TEST_ASSERT_EQUAL_STRING("", rev);
  TEST_ASSERT_EQUAL_UINT8(0, protocol);
}

static void test_displayed_letter_needs_its_complement() {
  FakeUnit u;
  int shown = -1;
  TEST_ASSERT_TRUE(unitReadDisplayedLetter(u, ADDR, shown));
  TEST_ASSERT_EQUAL(7, shown);
  u.corruptReplies = 1;
  shown = -1;
  TEST_ASSERT_FALSE(unitReadDisplayedLetter(u, ADDR, shown));
  TEST_ASSERT_EQUAL(-1, shown);
}

static void test_moving_status_and_the_wake_ping() {
  FakeUnit u;
  u.moving = 1;
  TEST_ASSERT_EQUAL(1, unitMovingStatus(u, ADDR));
  u.moving = 0;
  TEST_ASSERT_EQUAL(0, unitMovingStatus(u, ADDR));
  TEST_ASSERT_EQUAL(0, u.counted);  // idle polls are not transactions
  u.present = false;
  TEST_ASSERT_EQUAL(-1, unitMovingStatus(u, ADDR));
  TEST_ASSERT_EQUAL(1, u.readFailedCalls);  // recover BEFORE the ping (#207)
}

// --- verified writes --------------------------------------------------------

static void test_offset_write_is_read_back() {
  FakeUnit u;
  TEST_ASSERT_EQUAL(0, unitWriteOffset(u, ADDR, -120));
  TEST_ASSERT_EQUAL(-120, u.offset);
}

static void test_offset_write_that_did_not_land_is_a_mismatch() {
  FakeUnit u;
  u.offsetWriteSticks = false;
  TEST_ASSERT_EQUAL(UNIT_BUS_OFFSET_MISMATCH, unitWriteOffset(u, ADDR, -120));
}

static void test_offset_write_with_unreadable_readback_is_unverified() {
  FakeUnit u;
  u.corruptReplies = 1;
  TEST_ASSERT_EQUAL(UNIT_BUS_OFFSET_UNVERIFIED, unitWriteOffset(u, ADDR, 33));
}

static void test_gates_write_refused_by_the_unit_is_a_mismatch() {
  FakeUnit u;
  TEST_ASSERT_EQUAL(0, unitSetGates(u, ADDR, 0x00));
  TEST_ASSERT_EQUAL_UINT8(0x00, u.gates);
  u.gatesWriteSticks = false;
  TEST_ASSERT_EQUAL(UNIT_BUS_GATES_MISMATCH, unitSetGates(u, ADDR, 0x01));
  u.oldFirmware = true;
  TEST_ASSERT_EQUAL(UNIT_BUS_GATES_UNVERIFIED, unitSetGates(u, ADDR, 0x01));
}

// --- folds ------------------------------------------------------------------

static void test_identity_grades_the_rev_and_reads_the_offset() {
  FakeUnit u;
  UnitFacts f;
  TEST_ASSERT_TRUE(unitReadIdentity(u, f, ADDR, "19098af", "34c72e0,795f0af"));
  TEST_ASSERT_EQUAL_STRING("34c72e0", f.version);
  TEST_ASSERT_EQUAL_UINT8(0, f.fwStatus);  // content-equivalent = current
  TEST_ASSERT_TRUE(f.protocolKnown);
  TEST_ASSERT_TRUE(f.offsetValid);
  TEST_ASSERT_EQUAL(54, f.offset);

  UnitFacts g;
  unitReadIdentity(u, g, ADDR, "19098af", "");
  TEST_ASSERT_EQUAL_UINT8(1, g.fwStatus);  // outdated
}

static void test_refresh_clears_validity_when_the_unit_stops_answering() {
  FakeUnit u;
  UnitFacts f;
  unitRefreshVitals(u, f, ADDR);
  unitRefreshLifetime(u, f, ADDR);
  TEST_ASSERT_TRUE(f.vitalsValid);
  TEST_ASSERT_TRUE(f.lifetimeValid);
  TEST_ASSERT_EQUAL_UINT16(4913, f.vitals.vccNow_mV);
  u.present = false;
  unitRefreshVitals(u, f, ADDR);
  unitRefreshLifetime(u, f, ADDR);
  TEST_ASSERT_FALSE(f.vitalsValid);
  TEST_ASSERT_FALSE(f.lifetimeValid);
}

static void test_poll_status_is_the_liveness_signal() {
  FakeUnit u;
  UnitFacts f;
  UnitResetBaseline baseline;
  TEST_ASSERT_TRUE(unitPollStatus(u, f, ADDR, baseline));
  TEST_ASSERT_TRUE(f.statusValid);
  TEST_ASSERT_FALSE(f.resetSeen);  // first look is the baseline
  u.statusPayload[2] = 2;          // a brownout while we were watching
  TEST_ASSERT_TRUE(unitPollStatus(u, f, ADDR, baseline));
  TEST_ASSERT_TRUE(f.resetSeen);
  u.present = false;
  TEST_ASSERT_FALSE(unitPollStatus(u, f, ADDR, baseline));
  TEST_ASSERT_FALSE(f.statusValid);
}

static void test_boot_verdict_logs_once_per_change() {
  FakeUnit u;
  UnitFacts f;
  uint8_t logged = 0;
  BootUpdateReport r;
  unitRefreshBootVerdict(u, f, ADDR, logged, r);
  TEST_ASSERT_EQUAL_UINT8(BOOT_INTEGRITY_OK, f.bootVerdict);
  TEST_ASSERT_FALSE(unitRefreshBootVerdict(u, f, ADDR, logged, r));
  u.present = false;
  unitRefreshBootVerdict(u, f, ADDR, logged, r);
  TEST_ASSERT_EQUAL_UINT8(BOOT_INTEGRITY_UNREAD, f.bootVerdict);
}

// --- batch-idle wait, error ledger ---------------------------------------------

static void test_batch_wait_returns_once_every_unit_is_idle() {
  FakeUnit u;
  uint8_t addrs[1] = {ADDR};
  int rounds = 0;
  TEST_ASSERT_TRUE(unitWaitBatchIdle(u, addrs, 1, 5000, [&]() { rounds++; }));
  TEST_ASSERT_EQUAL(1, rounds);
  TEST_ASSERT_TRUE(u.now >= 1000);  // the restart got its second first
}

// A unit that is absent (or still in twiboot, answering non-zero) is not
// idle: the wait runs to its timeout instead of returning early.
static void test_batch_wait_times_out_on_a_unit_that_never_comes_back() {
  FakeUnit u;
  u.present = false;
  uint8_t addrs[1] = {ADDR};
  TEST_ASSERT_FALSE(unitWaitBatchIdle(u, addrs, 1, 2000, []() {}));
  TEST_ASSERT_TRUE(u.now >= 3000);
  FakeUnit moving;
  moving.moving = 0xFF;  // twiboot's answer to a bare read
  TEST_ASSERT_FALSE(unitWaitBatchIdle(moving, addrs, 1, 500, []() {}));
}

static void test_batch_wait_with_no_units_returns_at_once() {
  FakeUnit u;
  TEST_ASSERT_TRUE(unitWaitBatchIdle(u, nullptr, 0, 5000, []() {}));
  TEST_ASSERT_EQUAL_UINT32(0, u.now);
}

static void test_error_ledger_charges_a_unit_and_survives_a_rescan() {
  UnitErrorLedger<4> ledger;
  ledger.note(2, 1234);
  ledger.note(2, 2000);
  ledger.note(9, 1);  // out of range: ignored
  UnitFacts facts[4];
  ledger.fold(facts, 4);
  TEST_ASSERT_EQUAL_UINT16(2, facts[2].i2cErrors);
  TEST_ASSERT_EQUAL_UINT32(2000, facts[2].lastErrorMs);
  TEST_ASSERT_EQUAL_UINT16(0, facts[0].i2cErrors);
  facts[2] = UnitFacts{};  // a probe rebuilds the slot
  ledger.fold(facts[2], 2);
  TEST_ASSERT_EQUAL_UINT16(2, facts[2].i2cErrors);
}

// --- the composed probe / poll reads -----------------------------------------

namespace {
struct NotesSpy {
  std::vector<int> order;  // 1 identity, 2 drift, 3 boot verdict
  bool versionReadable = false;
  void identityRead(uint8_t, const UnitFacts&, bool readable) {
    order.push_back(1);
    versionReadable = readable;
  }
  void driftSeen(uint8_t, const DriftLogDecision&, const UnitDiagReading&) {
    order.push_back(2);
  }
  void bootVerdictChanged(uint8_t, const UnitFacts&, const BootUpdateReport&) {
    order.push_back(3);
  }
};
}  // namespace

// The identity note comes first, so a scan-log entry is complete before any
// other line the diagnostics may produce.
static void test_probe_reads_identity_then_diagnostics() {
  FakeUnit u;
  NotesSpy notes;
  UnitFacts f;
  uint8_t bootLogged = 0;
  TEST_ASSERT_TRUE(
      unitProbeSketchUnit(u, notes, f, ADDR, "34c72e0", "", bootLogged));
  TEST_ASSERT_TRUE(notes.versionReadable);
  TEST_ASSERT_FALSE(notes.order.empty());
  TEST_ASSERT_EQUAL(1, notes.order.front());
  TEST_ASSERT_EQUAL_UINT8(0, f.fwStatus);
  TEST_ASSERT_TRUE(f.offsetValid);
  TEST_ASSERT_TRUE(f.odometerValid);
  TEST_ASSERT_TRUE(f.vitalsValid);
  TEST_ASSERT_TRUE(f.lifetimeValid);
  TEST_ASSERT_EQUAL_UINT8(BOOT_INTEGRITY_OK, f.bootVerdict);
  TEST_ASSERT_FALSE(f.statusValid);  // the status is a poll's read, not a probe's
}

static void test_poll_returns_liveness_and_refreshes_the_diagnostics() {
  FakeUnit u;
  NotesSpy notes;
  UnitFacts f;
  UnitResetBaseline baseline;
  uint8_t bootLogged = 0;
  TEST_ASSERT_TRUE(unitPollHealth(u, notes, f, ADDR, baseline, bootLogged));
  TEST_ASSERT_TRUE(f.statusValid);
  TEST_ASSERT_TRUE(f.vitalsValid);
  u.present = false;
  TEST_ASSERT_FALSE(unitPollHealth(u, notes, f, ADDR, baseline, bootLogged));
  TEST_ASSERT_FALSE(f.statusValid);
  TEST_ASSERT_FALSE(f.vitalsValid);
  TEST_ASSERT_EQUAL_UINT8(BOOT_INTEGRITY_UNREAD, f.bootVerdict);
}

// A unit that keeps missing its status is asked nothing else: five more
// transactions would each fail the same way and count as a bus error. The
// first miss still asks — a unit that is there keeps its facts through one
// bad read.
static void test_poll_of_a_silent_unit_is_one_read() {
  FakeUnit u;
  NotesSpy notes;
  UnitFacts f;
  UnitResetBaseline baseline;
  uint8_t bootLogged = 0;
  unitPollHealth(u, notes, f, ADDR, baseline, bootLogged);
  u.present = false;
  int countedBefore = u.counted;
  TEST_ASSERT_FALSE(unitPollHealth(u, notes, f, ADDR, baseline, bootLogged));
  TEST_ASSERT_GREATER_THAN(1, u.counted - countedBefore);
  f.misses = 1;  // what the heartbeat made of that
  countedBefore = u.counted;
  TEST_ASSERT_FALSE(unitPollHealth(u, notes, f, ADDR, baseline, bootLogged));
  TEST_ASSERT_EQUAL(1, u.counted - countedBefore);
  // Nothing it said before is served as current.
  TEST_ASSERT_FALSE(f.diagValid);
  TEST_ASSERT_FALSE(f.vitalsValid);
  TEST_ASSERT_FALSE(f.extDiagValid);
  TEST_ASSERT_FALSE(f.linkValid);
  TEST_ASSERT_FALSE(f.lifetimeValid);
  TEST_ASSERT_EQUAL_UINT8(BOOT_INTEGRITY_UNREAD, f.bootVerdict);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_poll_of_a_silent_unit_is_one_read);
  RUN_TEST(test_no_argument_mutations_carry_the_guard_byte);
  RUN_TEST(test_enter_bootloader_is_one_bare_byte);
  RUN_TEST(test_letter_write_is_index_and_speed);
  RUN_TEST(test_boot_update_stage_carries_its_complement);
  RUN_TEST(test_a_silent_unit_nacks_every_write);
  RUN_TEST(test_status_read_decodes_the_payload);
  RUN_TEST(test_a_corrupted_reply_is_rejected_not_believed);
  RUN_TEST(test_a_short_reply_counts_an_error_and_recovers_the_bus);
  RUN_TEST(test_old_firmware_reads_as_unsupported_everywhere);
  RUN_TEST(test_version_read_yields_rev_and_protocol);
  RUN_TEST(test_version_with_a_json_breaking_character_is_rejected);
  RUN_TEST(test_displayed_letter_needs_its_complement);
  RUN_TEST(test_moving_status_and_the_wake_ping);
  RUN_TEST(test_offset_write_is_read_back);
  RUN_TEST(test_offset_write_that_did_not_land_is_a_mismatch);
  RUN_TEST(test_offset_write_with_unreadable_readback_is_unverified);
  RUN_TEST(test_gates_write_refused_by_the_unit_is_a_mismatch);
  RUN_TEST(test_identity_grades_the_rev_and_reads_the_offset);
  RUN_TEST(test_refresh_clears_validity_when_the_unit_stops_answering);
  RUN_TEST(test_poll_status_is_the_liveness_signal);
  RUN_TEST(test_boot_verdict_logs_once_per_change);
  RUN_TEST(test_probe_reads_identity_then_diagnostics);
  RUN_TEST(test_poll_returns_liveness_and_refreshes_the_diagnostics);
  RUN_TEST(test_batch_wait_returns_once_every_unit_is_idle);
  RUN_TEST(test_batch_wait_times_out_on_a_unit_that_never_comes_back);
  RUN_TEST(test_batch_wait_with_no_units_returns_at_once);
  RUN_TEST(test_error_ledger_charges_a_unit_and_survives_a_rescan);
  return UNITY_END();
}
