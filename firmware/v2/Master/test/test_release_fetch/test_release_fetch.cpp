// Native tests for ../release/ReleaseFetch.h: the order of a look and of an
// install, with stand-ins for the site, the signature check and the writer.
#include <unity.h>

#include <map>
#include <string>
#include <vector>

#include "ReleaseFetch.h"

void setUp() {}
void tearDown() {}

// A "hash" the tests can make by hand: the sum of the bytes in the first
// place, how many in the second.
static void sumOf(const std::string& data, uint8_t out[32]) {
  memset(out, 0, 32);
  for (unsigned char c : data) out[0] = (uint8_t)(out[0] + c);
  out[1] = (uint8_t)data.size();
}

struct Site : ReleaseHooks {
  std::map<std::string, std::string> files;
  std::string trustedSignature = "good";
  std::vector<std::string> log;
  std::string stream;
  size_t at = 0;
  long announce = -2;     // -2 = the file's real length
  size_t cutAfter = ~0u;  // the download breaks off after this many bytes
  std::string hashed;

  int get(const char* url, uint8_t* out, size_t cap) override {
    log.push_back(std::string("get ") + url);
    auto it = files.find(url);
    if (it == files.end()) return -1;
    if (it->second.size() > cap) return -2;
    memcpy(out, it->second.data(), it->second.size());
    return (int)it->second.size();
  }
  bool signatureOk(const uint8_t*, size_t, const char* signature, size_t len) override {
    log.push_back("verify");
    return std::string(signature, len) == trustedSignature;
  }
  long open(const char* url) override {
    log.push_back(std::string("open ") + url);
    auto it = files.find(url);
    if (it == files.end()) return -1;
    stream = it->second;
    at = 0;
    return announce == -2 ? (long)stream.size() : announce;
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
  void close() override { log.push_back("close"); }
  void hashStart() override { hashed.clear(); }
  void hashAdd(const uint8_t* data, size_t len) override { hashed.append((const char*)data, len); }
  void hashEnd(uint8_t out[32]) override { sumOf(hashed, out); }
};

struct Slot : ReleaseWriter {
  std::vector<std::string>& log;
  std::string written;
  ReleaseError beginAnswer = ReleaseError::Ok;
  bool writeOk = true;
  bool commitOk = true;
  int commits = 0;
  int aborts = 0;
  explicit Slot(std::vector<std::string>& l) : log(l) {}
  ReleaseError begin(const ReleaseImage&) override {
    log.push_back("begin");
    return beginAnswer;
  }
  bool write(const uint8_t* data, size_t len, uint32_t offset) override {
    TEST_ASSERT_EQUAL_UINT32(written.size(), offset);
    if (!writeOk) return false;
    written.append((const char*)data, len);
    return true;
  }
  bool commit() override {
    log.push_back("commit");
    commits++;
    return commitOk;
  }
  void abort() override {
    log.push_back("abort");
    aborts++;
  }
};

static const char* MANIFEST =
    "{\"format\":1,\"channel\":\"test\",\"tag\":\"t1\",\"notes\":\"n\",\"commitTime\":5,"
    "\"master\":{\"rev\":\"aaaaaaa\",\"path\":\"t1/m.bin\",\"size\":10,\"sha256\":"
    "\"0000000000000000000000000000000000000000000000000000000000000000\"},"
    "\"row\":{\"rev\":\"aaaaaaa\",\"path\":\"t1/r.bin\",\"size\":10,\"sha256\":"
    "\"0000000000000000000000000000000000000000000000000000000000000000\","
    "\"md5\":\"00000000000000000000000000000000\"},"
    "\"rescue\":{\"rev\":\"bbbbbbb\",\"path\":\"t1/s.bin\",\"size\":10,\"sha256\":"
    "\"0000000000000000000000000000000000000000000000000000000000000000\"}}";

#define BASE "https://" RELEASE_HOST "/test/"

static ReleaseLookBuffers work;

static void test_a_look_checks_the_signature_before_it_reads_the_manifest() {
  Site site;
  site.files[BASE "latest.json"] = MANIFEST;
  site.files[BASE "latest.json.sig"] = "good";
  ReleaseManifest m;
  TEST_ASSERT_EQUAL((int)ReleaseError::Ok, (int)releaseLook(site, "test", work, m));
  TEST_ASSERT_EQUAL_STRING("t1", m.tag);
  TEST_ASSERT_EQUAL(3, (int)site.log.size());
  TEST_ASSERT_EQUAL_STRING("get " BASE "latest.json", site.log[0].c_str());
  TEST_ASSERT_EQUAL_STRING("get " BASE "latest.json.sig", site.log[1].c_str());
  TEST_ASSERT_EQUAL_STRING("verify", site.log[2].c_str());
}

static void test_a_manifest_signed_by_another_key_is_never_read() {
  Site site;
  // Not even JSON: reading it would answer NotJson, so Signature shows it
  // was not read.
  site.files[BASE "latest.json"] = "not json at all";
  site.files[BASE "latest.json.sig"] = "forged";
  ReleaseManifest m;
  TEST_ASSERT_EQUAL((int)ReleaseError::Signature, (int)releaseLook(site, "test", work, m));
  site.files[BASE "latest.json"] = MANIFEST;
  TEST_ASSERT_EQUAL((int)ReleaseError::Signature, (int)releaseLook(site, "test", work, m));
  TEST_ASSERT_EQUAL_STRING("", m.tag);
}

static void test_a_look_fails_on_what_the_site_does_not_give() {
  Site site;
  ReleaseManifest m;
  TEST_ASSERT_EQUAL((int)ReleaseError::NoAnswer, (int)releaseLook(site, "test", work, m));
  site.files[BASE "latest.json"] = MANIFEST;
  TEST_ASSERT_EQUAL((int)ReleaseError::NoAnswer, (int)releaseLook(site, "test", work, m));
  site.files[BASE "latest.json"] = std::string(RELEASE_MANIFEST_MAX + 1, ' ');
  TEST_ASSERT_EQUAL((int)ReleaseError::TooLarge, (int)releaseLook(site, "test", work, m));
  site.files[BASE "latest.json"] = MANIFEST;
  site.files[BASE "latest.json.sig"] = std::string(RELEASE_SIGNATURE_MAX + 1, 'A');
  TEST_ASSERT_EQUAL((int)ReleaseError::Signature, (int)releaseLook(site, "test", work, m));
}

static void test_a_look_asks_the_channel_it_was_given_and_takes_no_other() {
  Site site;
  site.files["https://" RELEASE_HOST "/releases/latest.json"] = MANIFEST;  // signed for test
  site.files["https://" RELEASE_HOST "/releases/latest.json.sig"] = "good";
  ReleaseManifest m;
  TEST_ASSERT_EQUAL((int)ReleaseError::Channel, (int)releaseLook(site, "stable", work, m));
  TEST_ASSERT_EQUAL((int)ReleaseError::Channel, (int)releaseLook(site, "nightly", work, m));
}

static ReleaseImage imageOf(const std::string& data, const char* path = "t1/m.bin") {
  ReleaseImage image;
  strcpy(image.rev, "aaaaaaa");
  strcpy(image.path, path);
  image.size = (uint32_t)data.size();
  sumOf(data, image.sha256);
  return image;
}

static uint8_t buffer[4];

static void test_an_image_is_committed_only_after_every_byte_matched() {
  Site site;
  const std::string data = "0123456789";
  site.files[BASE "t1/m.bin"] = data;
  Slot slot(site.log);
  TEST_ASSERT_EQUAL((int)ReleaseError::Ok,
                    (int)releaseInstall(site, "test", imageOf(data), slot, buffer, sizeof(buffer)));
  TEST_ASSERT_EQUAL_STRING(data.c_str(), slot.written.c_str());
  TEST_ASSERT_EQUAL(1, slot.commits);
  TEST_ASSERT_EQUAL(0, slot.aborts);
  TEST_ASSERT_EQUAL(4, (int)site.log.size());
  TEST_ASSERT_EQUAL_STRING("open " BASE "t1/m.bin", site.log[0].c_str());
  TEST_ASSERT_EQUAL_STRING("begin", site.log[1].c_str());
  TEST_ASSERT_EQUAL_STRING("close", site.log[2].c_str());
  TEST_ASSERT_EQUAL_STRING("commit", site.log[3].c_str());
}

static void test_an_image_with_a_changed_byte_is_never_committed() {
  Site site;
  const std::string data = "0123456789";
  site.files[BASE "t1/m.bin"] = "0123456780";
  Slot slot(site.log);
  TEST_ASSERT_EQUAL((int)ReleaseError::Hash,
                    (int)releaseInstall(site, "test", imageOf(data), slot, buffer, sizeof(buffer)));
  TEST_ASSERT_EQUAL(0, slot.commits);
  TEST_ASSERT_EQUAL(1, slot.aborts);
}

static void test_a_download_cut_off_is_never_committed() {
  Site site;
  const std::string data = "0123456789";
  site.files[BASE "t1/m.bin"] = data;
  site.cutAfter = 6;
  Slot slot(site.log);
  TEST_ASSERT_EQUAL((int)ReleaseError::CutOff,
                    (int)releaseInstall(site, "test", imageOf(data), slot, buffer, sizeof(buffer)));
  TEST_ASSERT_EQUAL(0, slot.commits);
  TEST_ASSERT_EQUAL(1, slot.aborts);
  // The site ends the file early without an error.
  Site shortSite;
  shortSite.files[BASE "t1/m.bin"] = "01234";
  shortSite.announce = 10;
  Slot second(shortSite.log);
  TEST_ASSERT_EQUAL((int)ReleaseError::CutOff, (int)releaseInstall(shortSite, "test", imageOf(data),
                                                                    second, buffer, sizeof(buffer)));
  TEST_ASSERT_EQUAL(0, second.commits);
  TEST_ASSERT_EQUAL(1, second.aborts);
}

static void test_another_size_than_the_manifests_touches_nothing() {
  const std::string data = "0123456789";
  for (const char* served : {"0123456789ab", "01234"}) {
    Site site;
    site.files[BASE "t1/m.bin"] = served;
    Slot slot(site.log);
    TEST_ASSERT_EQUAL((int)ReleaseError::Length, (int)releaseInstall(site, "test", imageOf(data),
                                                                      slot, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL(2, (int)site.log.size());  // open, close: the writer never began
    TEST_ASSERT_EQUAL(0, slot.aborts);
  }
  Site missing;
  Slot slot(missing.log);
  TEST_ASSERT_EQUAL((int)ReleaseError::NoAnswer, (int)releaseInstall(missing, "test", imageOf(data),
                                                                      slot, buffer, sizeof(buffer)));
  TEST_ASSERT_EQUAL(2, (int)missing.log.size());
}

static void test_a_writer_that_refuses_ends_the_install() {
  const std::string data = "0123456789";
  {
    Site site;
    site.files[BASE "t1/m.bin"] = data;
    Slot slot(site.log);
    slot.beginAnswer = ReleaseError::Busy;
    TEST_ASSERT_EQUAL((int)ReleaseError::Busy, (int)releaseInstall(site, "test", imageOf(data), slot,
                                                                    buffer, sizeof(buffer)));
    // A writer that never began has nothing to abort.
    TEST_ASSERT_EQUAL(0, slot.aborts);
  }
  {
    Site site;
    site.files[BASE "t1/m.bin"] = data;
    Slot slot(site.log);
    slot.writeOk = false;
    TEST_ASSERT_EQUAL((int)ReleaseError::Write, (int)releaseInstall(site, "test", imageOf(data),
                                                                     slot, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL(0, slot.commits);
    TEST_ASSERT_EQUAL(1, slot.aborts);
  }
  {
    Site site;
    site.files[BASE "t1/m.bin"] = data;
    Slot slot(site.log);
    slot.commitOk = false;
    TEST_ASSERT_EQUAL((int)ReleaseError::Write, (int)releaseInstall(site, "test", imageOf(data),
                                                                     slot, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL(1, slot.aborts);
  }
}

static void test_a_path_outside_the_directory_is_never_opened() {
  Site site;
  Slot slot(site.log);
  const std::string data = "0123456789";
  TEST_ASSERT_EQUAL((int)ReleaseError::Path,
                    (int)releaseInstall(site, "test", imageOf(data, "../releases/m.bin"), slot,
                                        buffer, sizeof(buffer)));
  TEST_ASSERT_EQUAL(0, (int)site.log.size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_look_checks_the_signature_before_it_reads_the_manifest);
  RUN_TEST(test_a_manifest_signed_by_another_key_is_never_read);
  RUN_TEST(test_a_look_fails_on_what_the_site_does_not_give);
  RUN_TEST(test_a_look_asks_the_channel_it_was_given_and_takes_no_other);
  RUN_TEST(test_an_image_is_committed_only_after_every_byte_matched);
  RUN_TEST(test_an_image_with_a_changed_byte_is_never_committed);
  RUN_TEST(test_a_download_cut_off_is_never_committed);
  RUN_TEST(test_another_size_than_the_manifests_touches_nothing);
  RUN_TEST(test_a_writer_that_refuses_ends_the_install);
  RUN_TEST(test_a_path_outside_the_directory_is_never_opened);
  return UNITY_END();
}
