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
  bool nackExit = false;        // the order to start the application is refused
  // Identity (#541/#543). The original image answers the version read with
  // its stock string and wraps chipinfo at 8 bytes.
  uint8_t info[16] = {'T', 'W', 'I', 'B', 'O', 'O', 'T', ' ',
                      'v', '3', '.', '2', 0, 0, 0, 0};
  uint8_t fuses[4] = {0xFF, 0xCF, 0xFD, 0xDA};  // lfuse, lock, efuse, hfuse
  int chipinfoLen = 8;          // 12 with fuse bytes, 13 with a crash count
  uint8_t crashCount = 0;       // chipinfo byte 12
  int versionReads = 0;
  int chipinfoReads = 0;
  int longestChipinfoRead = 0;
  bool versionArmed = false;
  int cutVersionReads = 0;      // this many version reads come back one short
  int cutChipinfoReads = 0;     // this many chipinfo reads come back one short
  int cutFlashReads = 0;        // this many flash reads come back one short
  bool nackFlashRequest = false;

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
    bool versionRequest = tx.size() == 1 && tx[0] == TWIBOOT_CMD_READ_VERSION;
    bool readRequest = (tx.size() == 4 && tx[0] == TWIBOOT_CMD_ACCESS_MEMORY) ||
                       versionRequest;
    if (stop == readRequest) framingErrors++;
    versionArmed = false;
    if (tx.empty()) { probes++; probeTimes.push_back(now); }
    if (cur != ADDR || now < ackFromMs) return NACK;
    if (stuckBusy || now < busyUntilMs) return NACK;
    if (tx.empty()) return 0;
    if (tx[0] == TWIBOOT_CMD_WAIT) {
      if (tx.size() != 1) framingErrors++;
      pings++;
      return 0;
    }
    if (versionRequest) {
      versionArmed = true;
      return 0;
    }
    if (tx[0] == TWIBOOT_CMD_SWITCH_APPLICATION) {
      if (nackExit) return NACK;
      if (tx.size() == 2 && tx[1] == TWIBOOT_BOOTTYPE_APPLICATION) exits++;
      else framingErrors++;
      return 0;
    }
    if (tx[0] == TWIBOOT_CMD_ACCESS_MEMORY && tx.size() >= 4) {
      if (tx.size() != 4 && tx.size() != 4 + TWIBOOT_PAGE_SIZE) framingErrors++;
      memType = tx[1];
      memAddr = (uint16_t)((tx[2] << 8) | tx[3]);
      if (memType == TWIBOOT_MEMTYPE_CHIPINFO && nackChipRequest) return NACK;
      if (memType == TWIBOOT_MEMTYPE_FLASH && tx.size() == 4 && nackFlashRequest) {
        return NACK;
      }
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
    uint8_t n = qty;
    if (shortReads > 0) { shortReads--; n = (uint8_t)(qty - 1); }
    if (replyLen >= 0 && replyLen < n) n = (uint8_t)replyLen;
    if (versionArmed) {
      versionReads++;
      if (cutVersionReads > 0) { cutVersionReads--; n = (uint8_t)(n - 1); }
      for (uint8_t k = 0; k < n; k++) rx.push_back(info[k % 16]);
      return n;
    }
    if (memType == TWIBOOT_MEMTYPE_CHIPINFO) {
      chipinfoReads++;
      if (qty > longestChipinfoRead) longestChipinfoRead = qty;
      if (cutChipinfoReads > 0) { cutChipinfoReads--; n = (uint8_t)(n - 1); }
      for (uint8_t k = 0; k < n; k++) {
        int at = k % chipinfoLen;
        rx.push_back(at < 8 ? chipinfo[at] : at < 12 ? fuses[at - 8] : crashCount);
      }
      return n;
    }
    if (cutFlashReads > 0) { cutFlashReads--; n = (uint8_t)(n - 1); }
    const uint8_t* src = flash + memAddr;
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
  TwibootIdentity rescueId;
  TEST_ASSERT_TRUE(UnitRescueProbe::Bootloader == unitRescueProbe(bus, ADDR, rescueId));
  TEST_ASSERT_EQUAL(1, bus.exits);
}

static void test_rescue_probe_of_an_absent_unit_is_no_ack() {
  FakeTwiboot bus;
  TwibootIdentity rescueId;
  TEST_ASSERT_TRUE(UnitRescueProbe::NoAck == unitRescueProbe(bus, ADDR + 1, rescueId));
  TEST_ASSERT_EQUAL(0, bus.exits);
}

// A bootloader that did not take the order is still in it: not an exit.
static void test_rescue_probe_reports_a_refused_exit() {
  FakeTwiboot bus;
  TwibootIdentity rescueId;
  bus.nackExit = true;
  TEST_ASSERT_TRUE(UnitRescueProbe::ExitRefused == unitRescueProbe(bus, ADDR, rescueId));
  TEST_ASSERT_EQUAL(0, bus.exits);
}

// ACKs but is not twiboot: a sketch that cannot be read. Never sent an exit.
static void test_rescue_probe_of_a_silent_sketch() {
  FakeTwiboot bus;
  TwibootIdentity rescueId;
  bus.replyLen = 0;  // chipinfo read comes back empty
  TEST_ASSERT_TRUE(UnitRescueProbe::SketchSilent == unitRescueProbe(bus, ADDR, rescueId));
  TEST_ASSERT_EQUAL(0, bus.exits);
}

// --- bootloader identity (#541) and fuse/lock bytes (#543) ------------------

static void makeIdentityImage(FakeTwiboot& bus, uint8_t version, uint8_t caps) {
  memset(bus.info, 0xFF, sizeof(bus.info));
  bus.info[0] = 'S';
  bus.info[1] = 'F';
  bus.info[2] = version;
  bus.info[3] = caps;
  bus.chipinfoLen = 12;
}

static void test_identity_of_an_image_with_identity_and_fuse_bytes() {
  FakeTwiboot bus;
  makeIdentityImage(bus, 2, TWIBOOT_CAP_DO_SPM | TWIBOOT_CAP_BOUNDED_PIN |
                               TWIBOOT_CAP_FUSE_CHIPINFO);
  bus.flash[0] = 0x0C; bus.flash[1] = 0x94; bus.flash[2] = 0x5D; bus.flash[3] = 0x00;
  TwibootIdentity id;
  TEST_ASSERT_TRUE(twibootReadIdentity(bus, ADDR, id));
  TEST_ASSERT_EQUAL_UINT8(2, id.generation);
  TEST_ASSERT_EQUAL_HEX8(0x0B, id.caps);
  TEST_ASSERT_TRUE(id.fusesValid);
  TEST_ASSERT_EQUAL_HEX8(0xFF, id.lfuse);
  TEST_ASSERT_EQUAL_HEX8(0xCF, id.lock);
  TEST_ASSERT_EQUAL_HEX8(0xFD, id.efuse);
  TEST_ASSERT_EQUAL_HEX8(0xDA, id.hfuse);
  TEST_ASSERT_EQUAL(0, bus.framingErrors);
  TEST_ASSERT_EQUAL(0, bus.exits);
  TEST_ASSERT_EQUAL(0, bus.pageWrites);
  TEST_ASSERT_EQUAL(0, bus.readFailedCalls);
  TEST_ASSERT_EQUAL(TWIBOOT_CHIPINFO_FUSES_LEN, bus.longestChipinfoRead);
}

// The image every unit carried before #541: no identity bytes, and chipinfo
// wraps at 8 — so nothing past the version read is asked of it.
static void test_identity_of_an_image_without_identity_bytes() {
  FakeTwiboot bus;
  TwibootIdentity id;
  TEST_ASSERT_TRUE(twibootReadIdentity(bus, ADDR, id));
  TEST_ASSERT_EQUAL_UINT8(TWIBOOT_GEN_NO_IDENTITY, id.generation);
  TEST_ASSERT_EQUAL_HEX8(0, id.caps);
  TEST_ASSERT_FALSE(id.fusesValid);
  TEST_ASSERT_EQUAL(1, bus.versionReads);
  TEST_ASSERT_EQUAL(0, bus.chipinfoReads);
  TEST_ASSERT_EQUAL(0, bus.framingErrors);
}

static void test_identity_without_the_fuse_capability_reads_no_fuses() {
  FakeTwiboot bus;
  makeIdentityImage(bus, 2, TWIBOOT_CAP_DO_SPM);
  TwibootIdentity id;
  TEST_ASSERT_TRUE(twibootReadIdentity(bus, ADDR, id));
  TEST_ASSERT_EQUAL_UINT8(2, id.generation);
  TEST_ASSERT_FALSE(id.fusesValid);
  TEST_ASSERT_EQUAL(0, bus.chipinfoReads);
}

// Some chips serve the application's first four flash bytes where the fuses
// should be (#518). Bytes equal to those are not fuses.
static void test_fuse_bytes_that_are_the_reset_vector_are_not_fuses() {
  FakeTwiboot bus;
  makeIdentityImage(bus, 2, TWIBOOT_CAP_FUSE_CHIPINFO);
  // Z order on the wire: lfuse(0), lock(1), efuse(2), hfuse(3).
  bus.flash[0] = bus.fuses[0];
  bus.flash[1] = bus.fuses[1];
  bus.flash[2] = bus.fuses[2];
  bus.flash[3] = bus.fuses[3];
  TwibootIdentity id;
  TEST_ASSERT_TRUE(twibootReadIdentity(bus, ADDR, id));
  TEST_ASSERT_EQUAL_UINT8(2, id.generation);
  TEST_ASSERT_FALSE(id.fusesValid);
}

static void test_a_short_version_read_is_no_identity() {
  FakeTwiboot bus;
  makeIdentityImage(bus, 2, TWIBOOT_CAP_FUSE_CHIPINFO);
  bus.shortReads = 1;
  TwibootIdentity id;
  TEST_ASSERT_FALSE(twibootReadIdentity(bus, ADDR, id));
  TEST_ASSERT_EQUAL_UINT8(TWIBOOT_GEN_UNREAD, id.generation);
  TEST_ASSERT_EQUAL(1, bus.readFailedCalls);
  TEST_ASSERT_EQUAL(0, bus.chipinfoReads);
}

// --- crash record (#542) -----------------------------------------------------

static void makeCrashImage(FakeTwiboot& bus, uint8_t count) {
  makeIdentityImage(bus, 3, TWIBOOT_CAP_DO_SPM | TWIBOOT_CAP_BOUNDED_PIN |
                               TWIBOOT_CAP_CRASH_RECORD |
                               TWIBOOT_CAP_FUSE_CHIPINFO);
  bus.chipinfoLen = 13;
  bus.crashCount = count;
}

static void test_identity_carries_the_crash_count() {
  FakeTwiboot bus;
  makeCrashImage(bus, 2);
  TwibootIdentity id;
  TEST_ASSERT_TRUE(twibootReadIdentity(bus, ADDR, id));
  TEST_ASSERT_EQUAL_UINT8(3, id.generation);
  TEST_ASSERT_TRUE(id.crashValid);
  TEST_ASSERT_EQUAL_UINT8(2, id.crashCount);
  TEST_ASSERT_FALSE(twibootHeldForCrashing(id));
  TEST_ASSERT_TRUE(id.fusesValid);  // the fuse bytes are still read
  TEST_ASSERT_EQUAL(TWIBOOT_CHIPINFO_CRASH_LEN, bus.longestChipinfoRead);
  bus.crashCount = TWIBOOT_CRASH_HOLD_COUNT;
  TEST_ASSERT_TRUE(twibootReadIdentity(bus, ADDR, id));
  TEST_ASSERT_TRUE(twibootHeldForCrashing(id));
}

// An image without the capability wraps before byte 12: never asked for it,
// and whatever its chipinfo holds is never read as a crash count.
static void test_no_crash_count_without_the_capability() {
  FakeTwiboot bus;
  makeIdentityImage(bus, 2, TWIBOOT_CAP_FUSE_CHIPINFO);
  bus.crashCount = 9;
  TwibootIdentity id;
  TEST_ASSERT_TRUE(twibootReadIdentity(bus, ADDR, id));
  TEST_ASSERT_FALSE(id.crashValid);
  TEST_ASSERT_FALSE(twibootHeldForCrashing(id));
  TEST_ASSERT_EQUAL(TWIBOOT_CHIPINFO_FUSES_LEN, bus.longestChipinfoRead);
}

// The rescue must not put a crash-looping unit back into its crash: it is
// left in the bootloader, where the update job can flash it.
static void test_rescue_probe_leaves_a_crash_held_unit_in_its_bootloader() {
  FakeTwiboot bus;
  makeCrashImage(bus, TWIBOOT_CRASH_HOLD_COUNT);
  TwibootIdentity id;
  TEST_ASSERT_TRUE(UnitRescueProbe::CrashHeld == unitRescueProbe(bus, ADDR, id));
  TEST_ASSERT_EQUAL(0, bus.exits);
  TEST_ASSERT_TRUE(twibootHeldForCrashing(id));
}

// Below the threshold the unit is in its bootloader for another reason (a
// reset caught in the boot window): started as before.
static void test_rescue_probe_starts_a_unit_below_the_crash_threshold() {
  FakeTwiboot bus;
  makeCrashImage(bus, TWIBOOT_CRASH_HOLD_COUNT - 1);
  TwibootIdentity id;
  TEST_ASSERT_TRUE(UnitRescueProbe::Bootloader == unitRescueProbe(bus, ADDR, id));
  TEST_ASSERT_EQUAL(1, bus.exits);
}

// One lost read must not send a crash-looping unit back into its crash.
static void test_rescue_probe_asks_twice_before_starting_the_application() {
  FakeTwiboot bus;
  makeCrashImage(bus, TWIBOOT_CRASH_HOLD_COUNT);
  // The first read of the probe is the 8-byte chipinfo check; cut the
  // version read that follows it.
  bus.cutVersionReads = 1;
  TwibootIdentity id;
  TEST_ASSERT_TRUE(UnitRescueProbe::CrashHeld == unitRescueProbe(bus, ADDR, id));
  TEST_ASSERT_EQUAL(0, bus.exits);
  TEST_ASSERT_EQUAL(2, bus.versionReads);
}

// The crash count is read before the fuse check: a failed fuse read keeps it.
static void test_the_crash_count_survives_a_failed_fuse_check() {
  FakeTwiboot bus;
  makeCrashImage(bus, TWIBOOT_CRASH_HOLD_COUNT);
  bus.cutFlashReads = 1;
  TwibootIdentity id;
  TEST_ASSERT_TRUE(twibootReadIdentity(bus, ADDR, id));
  TEST_ASSERT_TRUE(twibootHeldForCrashing(id));
  TEST_ASSERT_FALSE(id.fusesValid);
  TEST_ASSERT_EQUAL(1, bus.readFailedCalls);
  // A short 13-byte reply is no crash count at all, reported once.
  FakeTwiboot shortBus;
  makeCrashImage(shortBus, TWIBOOT_CRASH_HOLD_COUNT);
  shortBus.cutChipinfoReads = 1;
  TEST_ASSERT_TRUE(twibootReadIdentity(shortBus, ADDR, id));
  TEST_ASSERT_FALSE(id.crashValid);
  TEST_ASSERT_EQUAL(1, shortBus.readFailedCalls);
}

// The identity stands when only the fuse read fails, wherever it fails; a
// short read is reported to the bus once, a NACKed request is not a read.
static void test_a_failed_fuse_read_keeps_the_identity() {
  struct Case { int cutChip; int cutFlash; bool nackChip; bool nackFlash; int readFailed; };
  const Case cases[] = {
      {1, 0, false, false, 1},  // chipinfo reply one byte short
      {0, 1, false, false, 1},  // flash bytes 0..3 one byte short
      {0, 0, true, false, 0},   // chipinfo request NACKed
      {0, 0, false, true, 0},   // flash request NACKed
  };
  for (const Case& c : cases) {
    FakeTwiboot bus;
    makeIdentityImage(bus, 2, TWIBOOT_CAP_FUSE_CHIPINFO);
    bus.cutChipinfoReads = c.cutChip;
    bus.cutFlashReads = c.cutFlash;
    bus.nackChipRequest = c.nackChip;
    bus.nackFlashRequest = c.nackFlash;
    TwibootIdentity id;
    TEST_ASSERT_TRUE(twibootReadIdentity(bus, ADDR, id));
    TEST_ASSERT_EQUAL_UINT8(2, id.generation);
    TEST_ASSERT_FALSE(id.fusesValid);
    TEST_ASSERT_EQUAL(c.readFailed, bus.readFailedCalls);
    TEST_ASSERT_EQUAL(0, bus.available());  // nothing left for a later read
  }
}

static void test_unrecognised_version_bytes_are_named_unknown() {
  FakeTwiboot bus;
  memset(bus.info, 0x00, sizeof(bus.info));
  TwibootIdentity id;
  TEST_ASSERT_TRUE(twibootReadIdentity(bus, ADDR, id));
  TEST_ASSERT_EQUAL_UINT8(TWIBOOT_GEN_UNKNOWN, id.generation);
  TEST_ASSERT_EQUAL(0, bus.chipinfoReads);
}

static void test_identity_text_for_the_scan_log() {
  char buf[TWIBOOT_IDENTITY_TEXT_CAP];
  TwibootIdentity id;
  twibootIdentityText(buf, sizeof(buf), id);
  TEST_ASSERT_EQUAL_STRING("", buf);
  id.generation = TWIBOOT_GEN_NO_IDENTITY;
  twibootIdentityText(buf, sizeof(buf), id);
  TEST_ASSERT_EQUAL_STRING(" (bootloader without identity bytes)", buf);
  id.generation = 2;
  id.caps = 0x0B;
  twibootIdentityText(buf, sizeof(buf), id);
  TEST_ASSERT_EQUAL_STRING(" (bootloader v2, lock/fuses unreadable)", buf);
  id.fusesValid = true;
  id.lfuse = 0xFF; id.lock = 0xCF; id.efuse = 0xFD; id.hfuse = 0xDA;
  twibootIdentityText(buf, sizeof(buf), id);
  TEST_ASSERT_EQUAL_STRING(" (bootloader v2, lock cf, fuses l ff h da e fd)", buf);
  id.generation = 3;
  id.crashValid = true;
  id.crashCount = 0;
  twibootIdentityText(buf, sizeof(buf), id);
  TEST_ASSERT_EQUAL_STRING(" (bootloader v3, lock cf, fuses l ff h da e fd)", buf);
  // 1 is what every intentional reset into the bootloader leaves: no news.
  id.crashCount = 1;
  twibootIdentityText(buf, sizeof(buf), id);
  TEST_ASSERT_EQUAL_STRING(" (bootloader v3, lock cf, fuses l ff h da e fd)", buf);
  id.crashCount = 2;
  twibootIdentityText(buf, sizeof(buf), id);
  TEST_ASSERT_EQUAL_STRING(
      " (bootloader v3, 2 crash reset(s), lock cf, fuses l ff h da e fd)", buf);
  id.crashCount = 15;
  id.fusesValid = false;
  twibootIdentityText(buf, sizeof(buf), id);
  TEST_ASSERT_EQUAL_STRING(
      " (bootloader v3, HELD after 15 crash reset(s), lock/fuses unreadable)",
      buf);
  TEST_ASSERT_TRUE(strlen(buf) < TWIBOOT_IDENTITY_TEXT_CAP - 1);
  id.generation = TWIBOOT_GEN_UNKNOWN;
  twibootIdentityText(buf, sizeof(buf), id);
  TEST_ASSERT_EQUAL_STRING(" (bootloader identity not recognised)", buf);
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
  RUN_TEST(test_rescue_probe_reports_a_refused_exit);
  RUN_TEST(test_rescue_probe_of_a_silent_sketch);
  RUN_TEST(test_identity_of_an_image_with_identity_and_fuse_bytes);
  RUN_TEST(test_identity_of_an_image_without_identity_bytes);
  RUN_TEST(test_identity_without_the_fuse_capability_reads_no_fuses);
  RUN_TEST(test_fuse_bytes_that_are_the_reset_vector_are_not_fuses);
  RUN_TEST(test_a_short_version_read_is_no_identity);
  RUN_TEST(test_a_failed_fuse_read_keeps_the_identity);
  RUN_TEST(test_unrecognised_version_bytes_are_named_unknown);
  RUN_TEST(test_identity_text_for_the_scan_log);
  RUN_TEST(test_identity_carries_the_crash_count);
  RUN_TEST(test_no_crash_count_without_the_capability);
  RUN_TEST(test_rescue_probe_leaves_a_crash_held_unit_in_its_bootloader);
  RUN_TEST(test_rescue_probe_starts_a_unit_below_the_crash_threshold);
  RUN_TEST(test_rescue_probe_asks_twice_before_starting_the_application);
  RUN_TEST(test_the_crash_count_survives_a_failed_fuse_check);
  return UNITY_END();
}
