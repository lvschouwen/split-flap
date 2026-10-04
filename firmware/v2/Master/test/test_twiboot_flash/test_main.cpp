// Host-side unit tests for shared/TwibootFlash.h — the twiboot I2C flash
// client both row masters instantiate. The bus is a scripted twiboot device,
// so the wire framing, the verify/rewrite loop and the post-exit sketch wait
// are exercised without hardware.

#include <ArduinoFake.h>
#include <string.h>
#include <unity.h>

#include <vector>

#include "TwibootFlash.h"
#include "UnitBusTwiboot.h"  // the sequences built on the client

namespace {

const uint8_t ADDR = 0x05;
const int NACK = 2;

struct FakeTwiboot {
  // --- device model ---
  uint8_t flash[0x8000];
  uint8_t chipinfo[8] = {0x1E, 0x95, 0x0F, TWIBOOT_PAGE_SIZE, 0x7C, 0x00, 0x04, 0x00};
  uint32_t ackFromMs = 0;       // the address NACKs before this time
  uint32_t busyUntilMs = 0;     // page programming: NACKs until then
  uint32_t programMs = 5;
  bool stuckBusy = false;       // never ACKs again after a page write
  int corruptWrites = 0;        // this many page writes store a flipped byte
  int shortReads = 0;           // this many reads return one byte too few
  int replyLen = -1;            // >= 0: every read returns this many bytes
  size_t txCap = 256;           // bus buffer size
  bool nackChipRequest = false;

  // --- observations ---
  int pageWrites = 0;
  int probes = 0;               // zero-length address probes
  int pings = 0;
  int exits = 0;
  int readFailedCalls = 0;
  std::vector<uint32_t> probeTimes;
  int framingErrors = 0;        // wrong length or wrong stop/repeated-start
  std::vector<uint8_t> sketchOps;  // guarded sketch opcodes seen (REBOOT…)
  int counted = 0;

  // --- bus state ---
  uint32_t now = 0;
  uint8_t cur = 0;
  std::vector<uint8_t> tx;
  std::vector<uint8_t> rx;
  size_t rxPos = 0;
  uint8_t memType = 0;
  uint16_t memAddr = 0;

  FakeTwiboot() { memset(flash, 0xFF, sizeof(flash)); }

  void beginTransmission(uint8_t a) { cur = a; tx.clear(); }
  size_t write(uint8_t b) {
    if (tx.size() >= txCap) return 0;
    tx.push_back(b);
    return 1;
  }
  // A read request (4 bytes) is followed by a repeated start; every other
  // frame ends with a stop, or the bus stays held.
  int endTransmission(bool stop) {
    bool readRequest = tx.size() == 4 && tx[0] == TWIBOOT_CMD_ACCESS_MEMORY;
    if (stop == readRequest) framingErrors++;
    if (tx.empty()) { probes++; probeTimes.push_back(now); }
    if (cur != ADDR || now < ackFromMs) return NACK;
    if (stuckBusy || now < busyUntilMs) return NACK;
    if (tx.empty()) return 0;
    if (tx[0] == TWIBOOT_CMD_WAIT) {
      if (tx.size() != 1) framingErrors++;
      pings++;
      return 0;
    }
    if (tx[0] == TWIBOOT_CMD_SWITCH_APPLICATION) {
      if (tx.size() == 2 && tx[1] == TWIBOOT_BOOTTYPE_APPLICATION) exits++;
      else framingErrors++;
      return 0;
    }
    if (tx[0] == TWIBOOT_CMD_ACCESS_MEMORY && tx.size() >= 4) {
      if (tx.size() != 4 && tx.size() != 4 + TWIBOOT_PAGE_SIZE) framingErrors++;
      memType = tx[1];
      memAddr = (uint16_t)((tx[2] << 8) | tx[3]);
      if (memType == TWIBOOT_MEMTYPE_CHIPINFO && nackChipRequest) return NACK;
      if (tx.size() == 4 + TWIBOOT_PAGE_SIZE) {
        memcpy(flash + memAddr, tx.data() + 4, TWIBOOT_PAGE_SIZE);
        if (corruptWrites > 0) { corruptWrites--; flash[memAddr + 7] ^= 0x01; }
        pageWrites++;
        busyUntilMs = now + programMs;
      }
      return 0;
    }
    // A guarded no-argument sketch opcode (opcode + ~opcode), e.g. the
    // REBOOT that closes a flash.
    if (tx.size() == 2 && tx[1] == (uint8_t)~tx[0]) {
      sketchOps.push_back(tx[0]);
      return 0;
    }
    framingErrors++;
    return NACK;
  }
  int endCounted() {
    counted++;
    return endTransmission(true);
  }
  void noteReadError() {}
  void mark(UnitBusAct, uint8_t) {}
  uint8_t requestFrom(uint8_t a, uint8_t qty) {
    rx.clear();
    rxPos = 0;
    if (a != ADDR) return 0;
    const uint8_t* src = memType == TWIBOOT_MEMTYPE_CHIPINFO ? chipinfo
                                                             : flash + memAddr;
    uint8_t n = qty;
    if (shortReads > 0) { shortReads--; n = (uint8_t)(qty - 1); }
    if (replyLen >= 0 && replyLen < n) n = (uint8_t)replyLen;
    rx.assign(src, src + n);
    return n;
  }
  int read() { return rxPos < rx.size() ? rx[rxPos++] : -1; }
  int available() { return (int)(rx.size() - rxPos); }
  uint32_t nowMs() { return now; }
  void sleepMs(uint32_t ms) { now += ms; }
  void readFailed() { readFailedCalls++; }
};

void fillPage(uint8_t* page, uint8_t seed) {
  for (int i = 0; i < TWIBOOT_PAGE_SIZE; i++) page[i] = (uint8_t)(seed + i);
}

}  // namespace

void setUp() {}
void tearDown() {}

static void test_image_guard_stops_at_the_boot_section() {
  TEST_ASSERT_TRUE(twibootImageFits(0));
  TEST_ASSERT_TRUE(twibootImageFits(BOOT_SECTION_START));
  TEST_ASSERT_FALSE(twibootImageFits(BOOT_SECTION_START + 1));
  TEST_ASSERT_FALSE(twibootImageFits(0x8000));
}

static void test_page_is_written_at_its_address_and_verified() {
  FakeTwiboot bus;
  uint8_t page[TWIBOOT_PAGE_SIZE];
  fillPage(page, 0x40);
  uint8_t rewrites = 9;
  TEST_ASSERT_TRUE(TwibootStep::Ok ==
      twibootFlashAndVerifyPage(bus, ADDR, 0x1280, page, &rewrites));
  TEST_ASSERT_EQUAL(1, bus.pageWrites);
  TEST_ASSERT_EQUAL(0, rewrites);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(page, bus.flash + 0x1280, TWIBOOT_PAGE_SIZE);
  TEST_ASSERT_EQUAL_HEX8(0xFF, bus.flash[0x1280 - 1]);
  TEST_ASSERT_EQUAL_HEX8(0xFF, bus.flash[0x1280 + TWIBOOT_PAGE_SIZE]);
  TEST_ASSERT_EQUAL(0, bus.readFailedCalls);
}

static void test_one_verify_mismatch_is_rewritten() {
  FakeTwiboot bus;
  bus.corruptWrites = 1;
  uint8_t page[TWIBOOT_PAGE_SIZE];
  fillPage(page, 0x11);
  uint8_t rewrites = 0;
  TEST_ASSERT_TRUE(TwibootStep::Ok ==
      twibootFlashAndVerifyPage(bus, ADDR, 0x0080, page, &rewrites));
  TEST_ASSERT_EQUAL(2, bus.pageWrites);
  TEST_ASSERT_EQUAL(1, rewrites);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(page, bus.flash + 0x0080, TWIBOOT_PAGE_SIZE);
}

static void test_persistent_mismatch_gives_up_after_the_attempt_cap() {
  FakeTwiboot bus;
  bus.corruptWrites = 100;
  uint8_t page[TWIBOOT_PAGE_SIZE];
  fillPage(page, 0x22);
  uint8_t rewrites = 0;
  TEST_ASSERT_TRUE(TwibootStep::PageVerifyMismatch ==
      twibootFlashAndVerifyPage(bus, ADDR, 0x0100, page, &rewrites));
  TEST_ASSERT_EQUAL(TWIBOOT_PAGE_WRITE_ATTEMPTS, bus.pageWrites);
  TEST_ASSERT_EQUAL(TWIBOOT_PAGE_WRITE_ATTEMPTS, rewrites);
}

static void test_truncated_burst_never_reaches_the_bus() {
  FakeTwiboot bus;
  bus.txCap = 128;  // the stock Wire buffer: header + page does not fit
  uint8_t page[TWIBOOT_PAGE_SIZE];
  fillPage(page, 0x33);
  TEST_ASSERT_TRUE(TwibootStep::PageBurstTruncated ==
      twibootFlashAndVerifyPage(bus, ADDR, 0x0000, page));
  TEST_ASSERT_EQUAL(0, bus.pageWrites);
  TEST_ASSERT_EQUAL_HEX8(0xFF, bus.flash[0]);
}

static void test_short_verify_read_fails_and_reports_the_bus() {
  FakeTwiboot bus;
  bus.shortReads = 1;
  uint8_t page[TWIBOOT_PAGE_SIZE];
  fillPage(page, 0x44);
  TEST_ASSERT_TRUE(TwibootStep::PageReadFailed ==
      twibootFlashAndVerifyPage(bus, ADDR, 0x0000, page));
  TEST_ASSERT_EQUAL(1, bus.readFailedCalls);
  TEST_ASSERT_EQUAL(0, bus.available());
}

static void test_page_write_waits_out_a_busy_bootloader() {
  FakeTwiboot bus;
  bus.busyUntilMs = 60;  // inside the pre-burst budget
  uint8_t page[TWIBOOT_PAGE_SIZE];
  fillPage(page, 0x55);
  TEST_ASSERT_TRUE(TwibootStep::Ok ==
      twibootFlashAndVerifyPage(bus, ADDR, 0x0000, page));

  FakeTwiboot late;
  late.busyUntilMs = TWIBOOT_READY_BEFORE_MS + 50;
  TEST_ASSERT_TRUE(TwibootStep::PageNotReady ==
      twibootFlashAndVerifyPage(late, ADDR, 0x0000, page));
  TEST_ASSERT_EQUAL(0, late.pageWrites);

  FakeTwiboot stuck;
  stuck.programMs = 1000;  // never done inside the post-burst budget
  TEST_ASSERT_TRUE(TwibootStep::PageStuckBusy ==
      twibootFlashAndVerifyPage(stuck, ADDR, 0x0000, page));
  TEST_ASSERT_EQUAL(1, stuck.pageWrites);
}

static void test_chip_check_accepts_only_the_328p_with_our_page_size() {
  FakeTwiboot ok;
  TEST_ASSERT_TRUE(TwibootStep::Ok == twibootVerifyChip(ok, ADDR));
  TEST_ASSERT_EQUAL(0, ok.available());

  FakeTwiboot wrongChip;
  wrongChip.chipinfo[2] = 0x14;  // ATmega328 (no P)
  TEST_ASSERT_TRUE(TwibootStep::ChipBadSignature ==
                   twibootVerifyChip(wrongChip, ADDR));

  FakeTwiboot wrongPage;
  wrongPage.chipinfo[3] = 64;
  TEST_ASSERT_TRUE(TwibootStep::ChipBadPageSize ==
                   twibootVerifyChip(wrongPage, ADDR));

  FakeTwiboot nack;
  nack.nackChipRequest = true;
  TEST_ASSERT_TRUE(TwibootStep::ChipRequestFailed ==
                   twibootVerifyChip(nack, ADDR));
  TEST_ASSERT_EQUAL(0, nack.readFailedCalls);

  FakeTwiboot shortRead;
  shortRead.shortReads = 1;
  TEST_ASSERT_TRUE(TwibootStep::ChipShortRead ==
                   twibootVerifyChip(shortRead, ADDR));
  TEST_ASSERT_EQUAL(1, shortRead.readFailedCalls);
  TEST_ASSERT_EQUAL(0, shortRead.available());
}

static void test_bootloader_probe_tells_twiboot_from_a_sketch() {
  FakeTwiboot boot;
  TEST_ASSERT_TRUE(twibootIsBootloader(boot, ADDR));
  TEST_ASSERT_EQUAL(0, boot.available());
  TEST_ASSERT_EQUAL(0, boot.pageWrites);

  FakeTwiboot absent;
  TEST_ASSERT_FALSE(twibootIsBootloader(absent, ADDR + 1));
  TEST_ASSERT_EQUAL(0, absent.readFailedCalls);  // NACKed before any read

  // A sketch ACKs the write but answers the read with something else.
  FakeTwiboot sketch;
  sketch.chipinfo[0] = 0x00;
  TEST_ASSERT_FALSE(twibootIsBootloader(sketch, ADDR));

  FakeTwiboot shortReply;
  shortReply.replyLen = 2;
  TEST_ASSERT_FALSE(twibootIsBootloader(shortReply, ADDR));
  TEST_ASSERT_EQUAL(1, shortReply.readFailedCalls);
  TEST_ASSERT_EQUAL(0, shortReply.available());

  // The signature alone decides: a clipped reply that still carries it counts.
  FakeTwiboot clipped;
  clipped.replyLen = 3;
  TEST_ASSERT_TRUE(twibootIsBootloader(clipped, ADDR));
}

static void test_read_page_returns_the_addressed_page() {
  FakeTwiboot bus;
  for (int i = 0; i < TWIBOOT_PAGE_SIZE; i++) bus.flash[0x7C00 + i] = (uint8_t)i;
  uint8_t out[TWIBOOT_PAGE_SIZE];
  TEST_ASSERT_TRUE(twibootReadFlashPage(bus, ADDR, 0x7C00, out));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(bus.flash + 0x7C00, out, TWIBOOT_PAGE_SIZE);
  TEST_ASSERT_FALSE(twibootReadFlashPage(bus, ADDR + 1, 0x7C00, out));
}

static void test_bootloader_wait_pings_a_bounded_number_of_times() {
  FakeTwiboot late;
  late.ackFromMs = 250;
  TEST_ASSERT_TRUE(twibootAwaitBootloader(late, ADDR));
  TEST_ASSERT_EQUAL(1, late.pings);

  FakeTwiboot silent;
  silent.ackFromMs = 0xFFFFFFFFUL;
  TEST_ASSERT_FALSE(twibootAwaitBootloader(silent, ADDR));
  TEST_ASSERT_EQUAL(TWIBOOT_LIVE_ATTEMPTS * TWIBOOT_LIVE_INTERVAL_MS,
                    silent.now);
}

static void test_exit_sends_the_start_application_command() {
  FakeTwiboot bus;
  TEST_ASSERT_EQUAL(0, twibootExit(bus, ADDR));
  TEST_ASSERT_EQUAL(1, bus.exits);
  TEST_ASSERT_NOT_EQUAL(0, twibootExit(bus, ADDR + 1));
}

static void test_sketch_wait_keeps_polling_a_slow_booting_unit() {
  // A unit that needs 2.2 s to boot: a single check after 2 s would miss it.
  FakeTwiboot slow;
  slow.ackFromMs = 2200;
  TEST_ASSERT_TRUE(twibootAwaitSketch(slow, ADDR));
  TEST_ASSERT_EQUAL(5, slow.probes);
  TEST_ASSERT_EQUAL(2500, slow.now);
}

static void test_sketch_wait_gives_the_sketch_time_before_the_first_probe() {
  FakeTwiboot quick;
  TEST_ASSERT_TRUE(twibootAwaitSketch(quick, ADDR));
  TEST_ASSERT_EQUAL(1, quick.probes);
  TEST_ASSERT_EQUAL(TWIBOOT_SKETCH_INTERVAL_MS, quick.probeTimes[0]);
}

static void test_sketch_wait_gives_up_on_a_silent_unit() {
  FakeTwiboot dead;
  dead.ackFromMs = 0xFFFFFFFFUL;
  TEST_ASSERT_FALSE(twibootAwaitSketch(dead, ADDR));
  TEST_ASSERT_EQUAL(TWIBOOT_SKETCH_ATTEMPTS, dead.probes);
  TEST_ASSERT_EQUAL(TWIBOOT_SKETCH_ATTEMPTS * TWIBOOT_SKETCH_INTERVAL_MS,
                    dead.now);
}

static void test_a_whole_flash_frames_every_transaction_correctly() {
  FakeTwiboot bus;
  bus.corruptWrites = 1;  // include a rewrite
  uint8_t page[TWIBOOT_PAGE_SIZE];
  TEST_ASSERT_TRUE(twibootIsBootloader(bus, ADDR));
  TEST_ASSERT_TRUE(twibootAwaitBootloader(bus, ADDR));
  TEST_ASSERT_TRUE(TwibootStep::Ok == twibootVerifyChip(bus, ADDR));
  for (int p = 0; p < 3; p++) {
    fillPage(page, (uint8_t)(p * 7));
    TEST_ASSERT_TRUE(TwibootStep::Ok == twibootFlashAndVerifyPage(
        bus, ADDR, (uint16_t)(p * TWIBOOT_PAGE_SIZE), page));
  }
  TEST_ASSERT_EQUAL(0, twibootExit(bus, ADDR));
  TEST_ASSERT_TRUE(twibootAwaitSketch(bus, ADDR));
  TEST_ASSERT_EQUAL(4, bus.pageWrites);
  TEST_ASSERT_EQUAL(0, bus.framingErrors);
}

static void test_every_step_has_a_distinct_name() {
  const TwibootStep steps[] = {
      TwibootStep::Ok, TwibootStep::ChipRequestFailed,
      TwibootStep::ChipShortRead, TwibootStep::ChipBadSignature,
      TwibootStep::ChipBadPageSize, TwibootStep::PageNotReady,
      TwibootStep::PageBurstTruncated, TwibootStep::PageWriteFailed,
      TwibootStep::PageStuckBusy, TwibootStep::PageReadFailed,
      TwibootStep::PageVerifyMismatch};
  const int n = sizeof(steps) / sizeof(steps[0]);
  for (int i = 0; i < n; i++) {
    for (int j = i + 1; j < n; j++) {
      TEST_ASSERT_NOT_EQUAL(0, strcmp(twibootStepName(steps[i]),
                                      twibootStepName(steps[j])));
    }
  }
}

// --- UnitBusTwiboot.h: the sequences both row masters run ---------------------

namespace {

struct Watch {
  int pagesBeforeStop = -1;  // >= 0: keepGoing() turns false after that many
  int asked = 0;
  int rewrittenPages = 0;
  bool keepGoing() { return pagesBeforeStop < 0 || asked++ < pagesBeforeStop; }
  void pageRewritten(uint16_t, uint8_t) { rewrittenPages++; }
};

const size_t IMAGE_LEN = 3 * TWIBOOT_PAGE_SIZE;

struct Image {
  uint8_t bytes[IMAGE_LEN];
  Image() {
    for (size_t i = 0; i < IMAGE_LEN; i++) bytes[i] = (uint8_t)(i * 7 + 3);
  }
  void operator()(size_t pageIndex, uint8_t* buf) const {
    memcpy(buf, bytes + pageIndex * TWIBOOT_PAGE_SIZE, TWIBOOT_PAGE_SIZE);
  }
};

}  // namespace

static void test_flash_image_writes_every_page_and_restarts_the_unit() {
  FakeTwiboot bus;
  Image image;
  Watch watch;
  UnitFlashReport r = unitFlashImage(bus, ADDR, IMAGE_LEN, image, watch);
  TEST_ASSERT_TRUE(UnitFlashResult::Ok == r.result);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(image.bytes, bus.flash, IMAGE_LEN);
  TEST_ASSERT_EQUAL(1, bus.exits);
  // The closing clean restart is the guarded REBOOT, sent AFTER the exit.
  TEST_ASSERT_EQUAL(1, (int)bus.sketchOps.size());
  TEST_ASSERT_EQUAL_UINT8(SFP_CMD_REBOOT, bus.sketchOps[0]);
  TEST_ASSERT_EQUAL(0, r.rebootStatus);
  TEST_ASSERT_EQUAL(0, bus.framingErrors);
}

static void test_flash_image_too_large_sends_nothing() {
  FakeTwiboot bus;
  Image image;
  Watch watch;
  UnitFlashReport r =
      unitFlashImage(bus, ADDR, BOOT_SECTION_START + TWIBOOT_PAGE_SIZE, image,
                     watch);
  TEST_ASSERT_TRUE(UnitFlashResult::ImageTooLarge == r.result);
  TEST_ASSERT_EQUAL(0, bus.pings);
  TEST_ASSERT_EQUAL(0, bus.pageWrites);
}

static void test_flash_image_silent_bootloader() {
  FakeTwiboot bus;
  bus.stuckBusy = true;  // never ACKs
  Image image;
  Watch watch;
  TEST_ASSERT_TRUE(UnitFlashResult::BootloaderSilent ==
                   unitFlashImage(bus, ADDR, IMAGE_LEN, image, watch).result);
  TEST_ASSERT_EQUAL(0, bus.pageWrites);
}

static void test_flash_image_refuses_a_foreign_chip() {
  FakeTwiboot bus;
  bus.chipinfo[1] = 0x94;  // not a 328P
  Image image;
  Watch watch;
  UnitFlashReport r = unitFlashImage(bus, ADDR, IMAGE_LEN, image, watch);
  TEST_ASSERT_TRUE(UnitFlashResult::ChipMismatch == r.result);
  TEST_ASSERT_TRUE(TwibootStep::ChipBadSignature == r.step);
  TEST_ASSERT_EQUAL(0, bus.pageWrites);
}

// A page that never verifies leaves the unit in twiboot: no exit is sent.
static void test_flash_image_failed_page_never_exits_onto_a_torn_image() {
  FakeTwiboot bus;
  bus.corruptWrites = 100;
  Image image;
  Watch watch;
  UnitFlashReport r = unitFlashImage(bus, ADDR, IMAGE_LEN, image, watch);
  TEST_ASSERT_TRUE(UnitFlashResult::PageFailed == r.result);
  TEST_ASSERT_TRUE(TwibootStep::PageVerifyMismatch == r.step);
  TEST_ASSERT_EQUAL_UINT16(0, r.pageAddr);
  TEST_ASSERT_EQUAL(0, bus.exits);
  TEST_ASSERT_TRUE(bus.sketchOps.empty());
  TEST_ASSERT_EQUAL(1, watch.rewrittenPages);
}

static void test_flash_image_stop_request_leaves_the_unit_in_twiboot() {
  FakeTwiboot bus;
  Image image;
  Watch watch;
  watch.pagesBeforeStop = 1;
  UnitFlashReport r = unitFlashImage(bus, ADDR, IMAGE_LEN, image, watch);
  TEST_ASSERT_TRUE(UnitFlashResult::Aborted == r.result);
  TEST_ASSERT_EQUAL(1, bus.pageWrites);
  TEST_ASSERT_EQUAL(0, bus.exits);
}

static void test_boot_section_read_returns_the_bytes_and_restarts_the_unit() {
  FakeTwiboot bus;
  for (int i = 0; i < BOOT_SECTION_LEN; i++) {
    bus.flash[BOOT_SECTION_START + i] = (uint8_t)(i ^ 0x5A);
  }
  uint8_t out[BOOT_SECTION_LEN];
  bool exitAcked = false, answered = false;
  int keepAlives = 0;
  UnitBootReadResult r = unitReadBootSection(
      bus, ADDR, out, [&]() { keepAlives++; }, exitAcked, answered);
  TEST_ASSERT_TRUE(UnitBootReadResult::Ok == r);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(bus.flash + BOOT_SECTION_START, out,
                                BOOT_SECTION_LEN);
  TEST_ASSERT_TRUE(exitAcked);
  TEST_ASSERT_TRUE(answered);
  TEST_ASSERT_EQUAL(1, bus.exits);
  TEST_ASSERT_EQUAL(0, bus.pageWrites);  // a read never writes flash
  TEST_ASSERT_EQUAL_UINT8(SFP_CMD_REBOOT, bus.sketchOps.back());
  TEST_ASSERT_TRUE(keepAlives >= BOOT_SECTION_LEN / TWIBOOT_PAGE_SIZE);
}

// A failed read still starts the application: the unit must not stay in its
// bootloader because a diagnostic read went wrong.
static void test_boot_section_read_failure_still_exits_the_bootloader() {
  FakeTwiboot bus;
  uint8_t out[BOOT_SECTION_LEN];
  bool exitAcked = false, answered = false;
  bool armed = false;
  // Armed from the first keep-alive, i.e. after the chip check: one page read
  // then fails twice.
  UnitBootReadResult r = unitReadBootSection(
      bus, ADDR, out,
      [&]() {
        if (!armed) bus.shortReads = 2;
        armed = true;
      },
      exitAcked, answered);
  TEST_ASSERT_TRUE(UnitBootReadResult::ReadFailed == r);
  TEST_ASSERT_EQUAL(1, bus.exits);
  TEST_ASSERT_TRUE(exitAcked);
}

static void test_boot_section_read_from_a_silent_unit_sends_no_exit() {
  FakeTwiboot bus;
  bus.stuckBusy = true;
  uint8_t out[BOOT_SECTION_LEN];
  bool exitAcked = true, answered = true;
  UnitBootReadResult r =
      unitReadBootSection(bus, ADDR, out, []() {}, exitAcked, answered);
  TEST_ASSERT_TRUE(UnitBootReadResult::BootloaderSilent == r);
  TEST_ASSERT_FALSE(exitAcked);
  TEST_ASSERT_FALSE(answered);
  TEST_ASSERT_EQUAL(0, bus.exits);
}

static void test_rescue_probe_starts_a_unit_parked_in_twiboot() {
  FakeTwiboot bus;
  TEST_ASSERT_TRUE(UnitRescueProbe::Bootloader == unitRescueProbe(bus, ADDR));
  TEST_ASSERT_EQUAL(1, bus.exits);
}

static void test_rescue_probe_of_an_absent_unit_is_no_ack() {
  FakeTwiboot bus;
  TEST_ASSERT_TRUE(UnitRescueProbe::NoAck == unitRescueProbe(bus, ADDR + 1));
  TEST_ASSERT_EQUAL(0, bus.exits);
}

// ACKs but is not twiboot: a sketch that cannot be read. Never sent an exit.
static void test_rescue_probe_of_a_silent_sketch() {
  FakeTwiboot bus;
  bus.replyLen = 0;  // chipinfo read comes back empty
  TEST_ASSERT_TRUE(UnitRescueProbe::SketchSilent == unitRescueProbe(bus, ADDR));
  TEST_ASSERT_EQUAL(0, bus.exits);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_image_guard_stops_at_the_boot_section);
  RUN_TEST(test_page_is_written_at_its_address_and_verified);
  RUN_TEST(test_one_verify_mismatch_is_rewritten);
  RUN_TEST(test_persistent_mismatch_gives_up_after_the_attempt_cap);
  RUN_TEST(test_truncated_burst_never_reaches_the_bus);
  RUN_TEST(test_short_verify_read_fails_and_reports_the_bus);
  RUN_TEST(test_page_write_waits_out_a_busy_bootloader);
  RUN_TEST(test_chip_check_accepts_only_the_328p_with_our_page_size);
  RUN_TEST(test_bootloader_probe_tells_twiboot_from_a_sketch);
  RUN_TEST(test_read_page_returns_the_addressed_page);
  RUN_TEST(test_bootloader_wait_pings_a_bounded_number_of_times);
  RUN_TEST(test_exit_sends_the_start_application_command);
  RUN_TEST(test_sketch_wait_keeps_polling_a_slow_booting_unit);
  RUN_TEST(test_sketch_wait_gives_up_on_a_silent_unit);
  RUN_TEST(test_sketch_wait_gives_the_sketch_time_before_the_first_probe);
  RUN_TEST(test_a_whole_flash_frames_every_transaction_correctly);
  RUN_TEST(test_every_step_has_a_distinct_name);
  RUN_TEST(test_flash_image_writes_every_page_and_restarts_the_unit);
  RUN_TEST(test_flash_image_too_large_sends_nothing);
  RUN_TEST(test_flash_image_silent_bootloader);
  RUN_TEST(test_flash_image_refuses_a_foreign_chip);
  RUN_TEST(test_flash_image_failed_page_never_exits_onto_a_torn_image);
  RUN_TEST(test_flash_image_stop_request_leaves_the_unit_in_twiboot);
  RUN_TEST(test_boot_section_read_returns_the_bytes_and_restarts_the_unit);
  RUN_TEST(test_boot_section_read_failure_still_exits_the_bootloader);
  RUN_TEST(test_boot_section_read_from_a_silent_unit_sends_no_exit);
  RUN_TEST(test_rescue_probe_starts_a_unit_parked_in_twiboot);
  RUN_TEST(test_rescue_probe_of_an_absent_unit_is_no_ack);
  RUN_TEST(test_rescue_probe_of_a_silent_sketch);
  return UNITY_END();
}
