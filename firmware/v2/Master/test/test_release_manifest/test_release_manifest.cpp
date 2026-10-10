// Native tests for ../release/ReleaseManifest.h: what a release says about
// itself and what a board refuses.
#include <unity.h>

#include <string>

#include "ReleaseManifest.h"

void setUp() {}
void tearDown() {}

#define SHA_A "a808ec127945aee4919c447b1adf1a19509a3f1d9ba606433f49df205d9aa68f"
#define SHA_B "111e97b1bc443cc87347f5a55d2792d06f07a8c7877f90d0b1fd5a086958c9ab"
#define SHA_C "7ed7232f144008d4c2d05b53f3643130946f8942236dd84c68762a152cb9f8fb"

// The manifest flashing/release.py writes, with one piece replaceable.
static std::string manifest(const char* from = nullptr, const char* to = nullptr) {
  std::string text =
      "{\n \"format\": 1,\n \"channel\": \"test\",\n \"tag\": \"test-18e3be0\",\n"
      " \"notes\": \"https://github.com/lvschouwen/split-flap/commit/18e3be0\",\n"
      " \"commitTime\": 1791622102,\n"
      " \"master\": {\"rev\": \"18e3be0\", \"path\": \"test-18e3be0/firmware-18e3be0-master.bin\","
      " \"size\": 1612384, \"sha256\": \"" SHA_A "\"},\n"
      " \"row\": {\"rev\": \"18e3be0\", \"path\": \"test-18e3be0/follower-18e3be0-gz.bin\","
      " \"size\": 326333, \"sha256\": \"" SHA_B "\", \"md5\": \"00eb07effbdd46ccab01b9f639c481e5\"},\n"
      " \"rescue\": {\"rev\": \"47640a1\", \"path\": \"test-18e3be0/rescue-47640a1.bin\","
      " \"size\": 997696, \"sha256\": \"" SHA_C "\"},\n"
      " \"units\": {\"revs\": [\"2262516\", \"3636a95\"]}\n}\n";
  if (from != nullptr) {
    const size_t at = text.find(from);
    TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, from);
    text.replace(at, strlen(from), to);
  }
  return text;
}

static ReleaseError read(const std::string& text, ReleaseManifest& out,
                         const char* channel = "test") {
  return releaseManifestRead((const uint8_t*)text.data(), text.size(), channel, out);
}

static void expect(ReleaseError want, const char* from, const char* to) {
  ReleaseManifest m;
  TEST_ASSERT_EQUAL_MESSAGE((int)want, (int)read(manifest(from, to), m), from);
  // Nothing of a refused manifest is left to be used.
  TEST_ASSERT_EQUAL_STRING("", m.master.path);
}

static void test_a_manifest_is_read_whole() {
  ReleaseManifest m;
  TEST_ASSERT_EQUAL((int)ReleaseError::Ok, (int)read(manifest(), m));
  TEST_ASSERT_EQUAL_STRING("test", m.channel);
  TEST_ASSERT_EQUAL_STRING("test-18e3be0", m.tag);
  TEST_ASSERT_EQUAL_STRING("https://github.com/lvschouwen/split-flap/commit/18e3be0", m.notes);
  TEST_ASSERT_EQUAL_UINT32(1791622102UL, m.commitTime);
  TEST_ASSERT_EQUAL_STRING("18e3be0", m.master.rev);
  TEST_ASSERT_EQUAL_STRING("test-18e3be0/firmware-18e3be0-master.bin", m.master.path);
  TEST_ASSERT_EQUAL_UINT32(1612384, m.master.size);
  TEST_ASSERT_EQUAL_HEX8(0xa8, m.master.sha256[0]);
  TEST_ASSERT_EQUAL_HEX8(0x8f, m.master.sha256[31]);
  TEST_ASSERT_EQUAL_UINT32(326333, m.row.size);
  TEST_ASSERT_EQUAL_HEX8(0x00, m.rowMd5[0]);
  TEST_ASSERT_EQUAL_HEX8(0xe5, m.rowMd5[15]);
  TEST_ASSERT_EQUAL_STRING("47640a1", m.rescue.rev);
  TEST_ASSERT_EQUAL_STRING("2262516,3636a95", m.unitRevs);
}

static void test_a_manifest_of_another_channel_is_refused() {
  ReleaseManifest m;
  TEST_ASSERT_EQUAL((int)ReleaseError::Channel, (int)read(manifest(), m, "stable"));
  TEST_ASSERT_EQUAL_STRING("", m.tag);
  expect(ReleaseError::Field, "\"channel\": \"test\",", "");
}

static void test_an_unknown_format_is_refused() {
  expect(ReleaseError::Format, "\"format\": 1", "\"format\": 2");
  expect(ReleaseError::Format, "\"format\": 1,", "");
  expect(ReleaseError::Format, "\"format\": 1", "\"format\": \"1\"");
}

static void test_too_large_or_no_json_is_refused() {
  ReleaseManifest m;
  std::string big = manifest();
  big.append(RELEASE_MANIFEST_MAX, ' ');
  TEST_ASSERT_EQUAL((int)ReleaseError::TooLarge, (int)read(big, m));
  TEST_ASSERT_EQUAL((int)ReleaseError::NotJson, (int)read("{\"format\": 1", m));
  TEST_ASSERT_EQUAL((int)ReleaseError::NotJson, (int)read("[1]", m));
}

static void test_a_path_that_leaves_its_directory_is_refused() {
  const char* path = "test-18e3be0/rescue-47640a1.bin";
  expect(ReleaseError::Path, path, "../releases/v1/rescue-47640a1.bin");
  expect(ReleaseError::Path, path, "test-18e3be0/../../rescue.bin");
  expect(ReleaseError::Path, path, "/rescue-47640a1.bin");
  expect(ReleaseError::Path, path, "//evil.example/rescue.bin");
  expect(ReleaseError::Path, path, "https://evil.example/rescue.bin");
  expect(ReleaseError::Path, path, "test-18e3be0//rescue.bin");
  expect(ReleaseError::Path, path, "test-18e3be0/rescue.bin?x=1");
  expect(ReleaseError::Path, path, "test-18e3be0/%2e%2e/rescue.bin");
  expect(ReleaseError::Path, path, "test-18e3be0\\\\rescue.bin");
  expect(ReleaseError::Path, path, "user@evil.example/rescue.bin");
  expect(ReleaseError::Path, path, "test-18e3be0/");
  expect(ReleaseError::Path, path, "");
  TEST_ASSERT_TRUE(releasePathOk("v2026.10.10/firmware-9509ccc-master.bin"));
}

static void test_a_missing_or_misshapen_field_is_refused() {
  expect(ReleaseError::Field, "\"tag\": \"test-18e3be0\",", "");
  expect(ReleaseError::Field, "\"commitTime\": 1791622102", "\"commitTime\": -5");
  expect(ReleaseError::Field, "\"commitTime\": 1791622102", "\"commitTime\": 0");
  expect(ReleaseError::Field, "\"size\": 997696", "\"size\": 0");
  expect(ReleaseError::Field, "\"size\": 997696", "\"size\": \"997696\"");
  expect(ReleaseError::Field, SHA_C, "7ed7");
  expect(ReleaseError::Field, SHA_A, "A808EC127945AEE4919C447B1ADF1A19509A3F1D9BA606433F49DF205D9AA68F");
  expect(ReleaseError::Field, "\"rev\": \"47640a1\",", "");
  expect(ReleaseError::Field, ", \"md5\": \"00eb07effbdd46ccab01b9f639c481e5\"", "");
  expect(ReleaseError::Field, "\"rev\": \"47640a1\"",
         "\"rev\": \"0123456789012345678901234567890123456789x\"");
}

static void test_a_release_without_unit_revs_still_reads() {
  ReleaseManifest m;
  TEST_ASSERT_EQUAL((int)ReleaseError::Ok,
                    (int)read(manifest(",\n \"units\": {\"revs\": [\"2262516\", \"3636a95\"]}", ""), m));
  TEST_ASSERT_EQUAL_STRING("", m.unitRevs);
  // A list that does not fit is not known, never cut.
  std::string many = "\"revs\": [";
  for (int i = 0; i < 20; i++) many += "\"0123456\", ";
  many += "\"2262516\"]";
  TEST_ASSERT_EQUAL((int)ReleaseError::Ok,
                    (int)read(manifest("\"revs\": [\"2262516\", \"3636a95\"]", many.c_str()), m));
  TEST_ASSERT_EQUAL_STRING("", m.unitRevs);
}

static void test_newer_is_a_later_commit_only() {
  ReleaseManifest m;
  m.commitTime = 1000;
  TEST_ASSERT_TRUE(releaseNewer(m, 999));
  TEST_ASSERT_FALSE(releaseNewer(m, 1000));
  TEST_ASSERT_FALSE(releaseNewer(m, 1001));
}

static void test_an_image_differs_by_rev() {
  ReleaseImage image;
  strcpy(image.rev, "47640a1");
  TEST_ASSERT_FALSE(releaseImageDiffers(image, "47640a1"));
  TEST_ASSERT_TRUE(releaseImageDiffers(image, "9509ccc"));
  TEST_ASSERT_TRUE(releaseImageDiffers(image, ""));
  TEST_ASSERT_TRUE(releaseImageDiffers(image, nullptr));
}

static void test_a_url_is_the_site_the_channel_and_the_name() {
  char url[RELEASE_URL_MAX];
  TEST_ASSERT_TRUE(releaseUrl(url, sizeof(url), "stable", "latest.json"));
  TEST_ASSERT_EQUAL_STRING("https://" RELEASE_HOST "/releases/latest.json", url);
  TEST_ASSERT_TRUE(releaseUrl(url, sizeof(url), "test", "t/x.bin"));
  TEST_ASSERT_EQUAL_STRING("https://" RELEASE_HOST "/test/t/x.bin", url);
  TEST_ASSERT_FALSE(releaseUrl(url, sizeof(url), "nightly", "latest.json"));
  TEST_ASSERT_FALSE(releaseUrl(url, 20, "test", "latest.json"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_manifest_is_read_whole);
  RUN_TEST(test_a_manifest_of_another_channel_is_refused);
  RUN_TEST(test_an_unknown_format_is_refused);
  RUN_TEST(test_too_large_or_no_json_is_refused);
  RUN_TEST(test_a_path_that_leaves_its_directory_is_refused);
  RUN_TEST(test_a_missing_or_misshapen_field_is_refused);
  RUN_TEST(test_a_release_without_unit_revs_still_reads);
  RUN_TEST(test_newer_is_a_later_commit_only);
  RUN_TEST(test_an_image_differs_by_rev);
  RUN_TEST(test_a_url_is_the_site_the_channel_and_the_name);
  return UNITY_END();
}
