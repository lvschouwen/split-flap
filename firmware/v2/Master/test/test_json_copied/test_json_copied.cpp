// Native test for JsonCopied.h: pins the ArduinoJson behaviour that makes it
// necessary, so a library change that moves it shows up here.
#include <ArduinoFake.h>
#include <unity.h>

#include <ArduinoJson.h>

#include "../../JsonCopied.h"

void setUp() {}
void tearDown() {}

struct Snapshot {
  char id[16];
};

static const char* written(const JsonDocument& doc) {
  static char out[64];
  serializeJson(doc, out, sizeof out);
  return out;
}

static void test_a_snapshots_char_array_is_only_pointed_at() {
  Snapshot snap;
  strcpy(snap.id, "row-a");
  const Snapshot& view = snap;
  JsonDocument doc;
  doc["id"] = view.id;
  strcpy(snap.id, "GONE!");  // the snapshot is reused before the document is written
  TEST_ASSERT_EQUAL_STRING("{\"id\":\"GONE!\"}", written(doc));
}

static void test_copied_keeps_the_text_the_snapshot_had() {
  Snapshot snap;
  strcpy(snap.id, "row-a");
  const Snapshot& view = snap;
  JsonDocument doc;
  doc["id"] = jsonCopied(view.id);
  strcpy(snap.id, "GONE!");
  TEST_ASSERT_EQUAL_STRING("{\"id\":\"row-a\"}", written(doc));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_snapshots_char_array_is_only_pointed_at);
  RUN_TEST(test_copied_keeps_the_text_the_snapshot_had);
  return UNITY_END();
}
