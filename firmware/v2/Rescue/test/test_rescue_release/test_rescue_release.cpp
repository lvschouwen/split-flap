// Host-side unit tests for RescueRelease.h (#583): what the rescue image
// takes from a release (the master image, nothing else), when it refuses to
// start, and what /rescue/status says about it. The order of verify, write
// and activate is ../release/ReleaseFetch.h, tested in Master.

#include <ArduinoFake.h>
#include <unity.h>

#include <map>
#include <string>
#include <vector>

#include "../../RescueRelease.h"

void setUp() {}
void tearDown() {}

// A "hash" the tests can make by hand: the sum of the bytes in the first
// place, how many in the second.
static void sumOf(const std::string& data, uint8_t out[32]) {
  memset(out, 0, 32);
  for (unsigned char c : data) out[0] = (uint8_t)(out[0] + c);
  out[1] = (uint8_t)data.size();
}

static std::string hexOf(const uint8_t* bytes, size_t n) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < n; i++) {
    out += digits[bytes[i] >> 4];
    out += digits[bytes[i] & 0x0F];
  }
  return out;
}

struct Site : ReleaseHooks {
  std::map<std::string, std::string> files;
  std::vector<std::string> log;
  std::string stream;
  size_t at = 0;
  size_t cutAfter = ~0u;
  std::string hashed;
  uint32_t lastDone = 0;

  int get(const char* url, uint8_t* out, size_t cap) override {
    log.push_back(std::string("get ") + url);
    auto it = files.find(url);
    if (it == files.end()) return -1;
    if (it->second.size() > cap) return -2;
    memcpy(out, it->second.data(), it->second.size());
    return (int)it->second.size();
  }
  bool signatureOk(const uint8_t*, size_t, const char* signature, size_t len) override {
    return std::string(signature, len) == "good";
  }
  long open(const char* url) override {
    log.push_back(std::string("open ") + url);
    auto it = files.find(url);
    if (it == files.end()) return -1;
    stream = it->second;
    at = 0;
    return (long)stream.size();
  }
  int read(uint8_t* out, size_t cap) override {
    if (at >= cutAfter) return -1;
    size_t n = stream.size() - at;
    if (n > cap) n = cap;
    if (n > cutAfter - at) n = cutAfter - at;
    memcpy(out, stream.data() + at, n);
    at += n;
    return (int)n;
  }
  void close() override {}
  void hashStart() override { hashed.clear(); }
  void hashAdd(const uint8_t* data, size_t len) override { hashed.append((const char*)data, len); }
  void hashEnd(uint8_t out[32]) override { sumOf(hashed, out); }
  void progress(uint32_t done, uint32_t) override { lastDone = done; }
};

struct Slot : ReleaseWriter {
  std::string written;
  int begins = 0, commits = 0, aborts = 0;
  ReleaseError begin(const ReleaseImage&) override {
    begins++;
    return ReleaseError::Ok;
  }
  bool write(const uint8_t* data, size_t len, uint32_t) override {
    written.append((const char*)data, len);
    return true;
  }
  bool commit() override {
    commits++;
    return true;
  }
  void abort() override { aborts++; }
};

static const std::string BASE = "https://" RELEASE_HOST "/test/";
static const std::string MASTER_IMAGE = "the master image, as published";

static std::string imageJson(const char* rev, const char* path, const std::string& content) {
  uint8_t sum[32];
  sumOf(content, sum);
  return std::string("{\"rev\":\"") + rev + "\",\"path\":\"" + path +
         "\",\"size\":" + std::to_string(content.size()) + ",\"sha256\":\"" + hexOf(sum, 32) + "\"";
}

// A site with one release on the test channel. Only the master image is
// there: the rescue image must not ask for another.
static Site siteWithRelease() {
  Site site;
  const std::string manifest =
      std::string("{\"format\":1,\"channel\":\"test\",\"tag\":\"test-abc1234\","
                  "\"notes\":\"https://example.org/notes\",\"commitTime\":1791633162,") +
      "\"master\":" + imageJson("abc1234", "test-abc1234/firmware-abc1234-master.bin", MASTER_IMAGE) +
      "},\"row\":" + imageJson("abc1234", "test-abc1234/follower-abc1234-gz.bin", "row") +
      ",\"md5\":\"00112233445566778899aabbccddeeff\"},\"rescue\":" +
      imageJson("def5678", "test-abc1234/rescue-def5678.bin", "rescue") + "}}";
  site.files[BASE + "latest.json"] = manifest;
  site.files[BASE + "latest.json.sig"] = "good";
  site.files[BASE + "test-abc1234/firmware-abc1234-master.bin"] = MASTER_IMAGE;
  return site;
}

// --- looking ------------------------------------------------------------------

static void test_look_says_what_the_release_is() {
  Site site = siteWithRelease();
  RescueReleaseStatus status;
  rescueReleaseStarted(status, RescueReleaseState::Looking, "test");
  TEST_ASSERT_EQUAL(ReleaseError::Ok, rescueReleaseLook(site, "test", status));
  TEST_ASSERT_EQUAL(RescueReleaseState::Found, status.state);
  TEST_ASSERT_EQUAL_STRING("test-abc1234", status.tag);
  TEST_ASSERT_EQUAL_STRING("abc1234", status.masterRev);
  TEST_ASSERT_EQUAL_STRING("https://example.org/notes", status.notes);
  TEST_ASSERT_EQUAL_STRING("test", status.channel);
}

static void test_look_refuses_another_signature() {
  Site site = siteWithRelease();
  site.files[BASE + "latest.json.sig"] = "forged";
  RescueReleaseStatus status;
  rescueReleaseStarted(status, RescueReleaseState::Looking, "test");
  TEST_ASSERT_EQUAL(ReleaseError::Signature, rescueReleaseLook(site, "test", status));
  TEST_ASSERT_EQUAL(RescueReleaseState::Failed, status.state);
  TEST_ASSERT_EQUAL(ReleaseError::Signature, status.error);
  // Nothing of a refused release is shown.
  TEST_ASSERT_EQUAL_STRING("", status.tag);
  TEST_ASSERT_EQUAL_STRING("", status.masterRev);
}

static void test_look_refuses_the_other_channel() {
  Site site = siteWithRelease();
  site.files["https://" RELEASE_HOST "/releases/latest.json"] = site.files[BASE + "latest.json"];
  site.files["https://" RELEASE_HOST "/releases/latest.json.sig"] = "good";
  RescueReleaseStatus status;
  rescueReleaseStarted(status, RescueReleaseState::Looking, "stable");
  TEST_ASSERT_EQUAL(ReleaseError::Channel, rescueReleaseLook(site, "stable", status));
  TEST_ASSERT_EQUAL(RescueReleaseState::Failed, status.state);
}

// --- installing ---------------------------------------------------------------

static void test_install_writes_the_master_image_and_nothing_else() {
  Site site = siteWithRelease();
  Slot slot;
  RescueReleaseStatus status;
  uint8_t buffer[8];
  rescueReleaseStarted(status, RescueReleaseState::Installing, "test");
  TEST_ASSERT_EQUAL(ReleaseError::Ok, rescueReleaseInstall(site, "test", "test-abc1234", slot, buffer,
                                                           sizeof(buffer), status));
  TEST_ASSERT_EQUAL(RescueReleaseState::Installed, status.state);
  TEST_ASSERT_EQUAL_STRING(MASTER_IMAGE.c_str(), slot.written.c_str());
  TEST_ASSERT_EQUAL(1, slot.commits);
  TEST_ASSERT_EQUAL(0, slot.aborts);
  TEST_ASSERT_EQUAL_STRING("test-abc1234", status.tag);
  for (const std::string& line : site.log) {
    TEST_ASSERT_NULL_MESSAGE(strstr(line.c_str(), "rescue-"), line.c_str());
    TEST_ASSERT_NULL_MESSAGE(strstr(line.c_str(), "follower-"), line.c_str());
  }
}

static void test_install_looks_again_first() {
  // Never an install from what a look found earlier: the signature is
  // checked in the same run that writes.
  Site site = siteWithRelease();
  site.files[BASE + "latest.json.sig"] = "forged";
  Slot slot;
  RescueReleaseStatus status;
  uint8_t buffer[8];
  rescueReleaseStarted(status, RescueReleaseState::Installing, "test");
  TEST_ASSERT_EQUAL(ReleaseError::Signature,
                    rescueReleaseInstall(site, "test", "test-abc1234", slot, buffer, sizeof(buffer),
                                         status));
  TEST_ASSERT_EQUAL(0, slot.begins);
  TEST_ASSERT_EQUAL(RescueReleaseState::Failed, status.state);
}

static void test_a_changed_image_is_not_made_the_next_to_start() {
  Site site = siteWithRelease();
  std::string& image = site.files[BASE + "test-abc1234/firmware-abc1234-master.bin"];
  image[3] = (char)(image[3] + 1);
  Slot slot;
  RescueReleaseStatus status;
  uint8_t buffer[8];
  rescueReleaseStarted(status, RescueReleaseState::Installing, "test");
  TEST_ASSERT_EQUAL(ReleaseError::Hash,
                    rescueReleaseInstall(site, "test", "test-abc1234", slot, buffer, sizeof(buffer),
                                         status));
  TEST_ASSERT_EQUAL(0, slot.commits);
  TEST_ASSERT_EQUAL(1, slot.aborts);
  TEST_ASSERT_EQUAL(RescueReleaseState::Failed, status.state);
  TEST_ASSERT_EQUAL(ReleaseError::Hash, status.error);
  // What was being installed stays named.
  TEST_ASSERT_EQUAL_STRING("test-abc1234", status.tag);
}

static void test_a_download_cut_off_is_not_made_the_next_to_start() {
  Site site = siteWithRelease();
  site.cutAfter = 10;
  Slot slot;
  RescueReleaseStatus status;
  uint8_t buffer[8];
  rescueReleaseStarted(status, RescueReleaseState::Installing, "test");
  TEST_ASSERT_EQUAL(ReleaseError::CutOff,
                    rescueReleaseInstall(site, "test", "test-abc1234", slot, buffer, sizeof(buffer),
                                         status));
  TEST_ASSERT_EQUAL(0, slot.commits);
  TEST_ASSERT_EQUAL(1, slot.aborts);
}

static void test_install_writes_only_the_release_that_was_shown() {
  // The person confirmed another release than the site has by now.
  Site site = siteWithRelease();
  Slot slot;
  RescueReleaseStatus status;
  uint8_t buffer[8];
  rescueReleaseStarted(status, RescueReleaseState::Installing, "test");
  TEST_ASSERT_EQUAL(ReleaseError::Ok, rescueReleaseInstall(site, "test", "test-0000000", slot, buffer,
                                                           sizeof(buffer), status));
  TEST_ASSERT_EQUAL(0, slot.begins);
  TEST_ASSERT_EQUAL(RescueReleaseState::Found, status.state);
  TEST_ASSERT_TRUE(status.changed);
  TEST_ASSERT_EQUAL_STRING("test-abc1234", status.tag);
  char out[RESCUE_RELEASE_JSON_MAX];
  TEST_ASSERT_TRUE(rescueReleaseJson(status, out, sizeof(out)));
  TEST_ASSERT_NOT_NULL(strstr(out, "\"changed\":true"));
}

// --- when a job may start -----------------------------------------------------

static void test_refusals() {
  // online, an upload is running, a job is running, restart staged, channel
  TEST_ASSERT_EQUAL(0, rescueReleaseRefusal(true, false, false, false, "stable").status);
  TEST_ASSERT_EQUAL(0, rescueReleaseRefusal(true, false, false, false, "test").status);
  TEST_ASSERT_EQUAL(400, rescueReleaseRefusal(true, false, false, false, "nightly").status);
  TEST_ASSERT_EQUAL(400, rescueReleaseRefusal(true, false, false, false, "").status);
  TEST_ASSERT_EQUAL(409, rescueReleaseRefusal(false, false, false, false, "stable").status);
  TEST_ASSERT_EQUAL(409, rescueReleaseRefusal(true, true, false, false, "stable").status);
  TEST_ASSERT_EQUAL(409, rescueReleaseRefusal(true, false, true, false, "stable").status);
  TEST_ASSERT_EQUAL(409, rescueReleaseRefusal(true, false, false, true, "stable").status);
  // An install names the release it is for.
  TEST_ASSERT_EQUAL(0, rescueReleaseRefusal(true, false, false, false, "stable", "v2026.10.11").status);
  TEST_ASSERT_EQUAL(400, rescueReleaseRefusal(true, false, false, false, "stable", "").status);
  TEST_ASSERT_EQUAL(400, rescueReleaseRefusal(true, false, false, false, "stable",
                                              "a-tag-that-is-longer-than-any-release-has")
                             .status);
  // Each says why.
  TEST_ASSERT_NOT_NULL(strstr(rescueReleaseRefusal(false, false, false, false, "stable").why,
                              "internet"));
  TEST_ASSERT_NOT_NULL(strstr(rescueReleaseRefusal(true, true, false, false, "stable").why,
                              "upload"));
}

// --- what the page reads ------------------------------------------------------

static void test_status_json_idle() {
  RescueReleaseStatus status;
  char out[RESCUE_RELEASE_JSON_MAX];
  TEST_ASSERT_TRUE(rescueReleaseJson(status, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("{\"state\":\"idle\"}", out);
}

static void test_status_json_found_and_failed() {
  Site site = siteWithRelease();
  RescueReleaseStatus status;
  rescueReleaseStarted(status, RescueReleaseState::Looking, "test");
  rescueReleaseLook(site, "test", status);
  char out[RESCUE_RELEASE_JSON_MAX];
  TEST_ASSERT_TRUE(rescueReleaseJson(status, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING(
      "{\"state\":\"found\",\"channel\":\"test\",\"tag\":\"test-abc1234\","
      "\"notes\":\"https://example.org/notes\",\"master\":\"abc1234\"}",
      out);

  site.files.erase(BASE + "latest.json");
  rescueReleaseStarted(status, RescueReleaseState::Looking, "test");
  rescueReleaseLook(site, "test", status);
  TEST_ASSERT_TRUE(rescueReleaseJson(status, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING(
      "{\"state\":\"failed\",\"channel\":\"test\","
      "\"error\":\"the release site did not answer\"}",
      out);
}

static void test_status_json_installing_carries_progress() {
  RescueReleaseStatus status;
  rescueReleaseStarted(status, RescueReleaseState::Installing, "stable");
  strcpy(status.tag, "v2026.10.11");
  strcpy(status.masterRev, "abc1234");
  strcpy(status.notes, "https://example.org/n");
  status.done = 4096;
  status.size = 1700000;
  char out[RESCUE_RELEASE_JSON_MAX];
  TEST_ASSERT_TRUE(rescueReleaseJson(status, out, sizeof(out)));
  TEST_ASSERT_NOT_NULL(strstr(out, "\"state\":\"installing\""));
  TEST_ASSERT_NOT_NULL(strstr(out, "\"done\":4096,\"size\":1700000"));
}

static void test_status_json_fits_the_longest_release() {
  RescueReleaseStatus status;
  rescueReleaseStarted(status, RescueReleaseState::Failed, "stable");
  // A control character is six bytes in JSON; the manifest reader takes any.
  memset(status.tag, 1, RELEASE_TAG_MAX);
  memset(status.notes, 1, RELEASE_NOTES_MAX);
  memset(status.masterRev, 1, RELEASE_REV_MAX);
  status.changed = true;
  status.error = ReleaseError::Format;  // the longest wording
  status.done = status.size = 0xFFFFFFFFu;
  char out[RESCUE_RELEASE_JSON_MAX];
  TEST_ASSERT_TRUE(rescueReleaseJson(status, out, sizeof(out)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_look_says_what_the_release_is);
  RUN_TEST(test_look_refuses_another_signature);
  RUN_TEST(test_look_refuses_the_other_channel);
  RUN_TEST(test_install_writes_the_master_image_and_nothing_else);
  RUN_TEST(test_install_looks_again_first);
  RUN_TEST(test_a_changed_image_is_not_made_the_next_to_start);
  RUN_TEST(test_a_download_cut_off_is_not_made_the_next_to_start);
  RUN_TEST(test_install_writes_only_the_release_that_was_shown);
  RUN_TEST(test_refusals);
  RUN_TEST(test_status_json_idle);
  RUN_TEST(test_status_json_found_and_failed);
  RUN_TEST(test_status_json_installing_carries_progress);
  RUN_TEST(test_status_json_fits_the_longest_release);
  return UNITY_END();
}
