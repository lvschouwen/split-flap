// Native tests for StreamPolicy.h: when a topic of GET /api/v2/stream is sent.
#include <unity.h>

#include <string.h>

#include "StreamPolicy.h"

void setUp() {}
void tearDown() {}

static void test_a_document_is_sent_once_and_again_only_when_it_changes() {
  StreamTracker t;
  TEST_ASSERT_TRUE(t.due(StreamTopic::Wall, "{\"rows\":[]}"));
  TEST_ASSERT_FALSE(t.due(StreamTopic::Wall, "{\"rows\":[]}"));
  TEST_ASSERT_TRUE(t.due(StreamTopic::Wall, "{\"rows\":[1]}"));
  TEST_ASSERT_TRUE(t.due(StreamTopic::Wall, "{\"rows\":[]}"));  // back again is a change
}

static void test_the_first_document_is_sent_even_when_it_is_empty() {
  StreamTracker t;
  TEST_ASSERT_TRUE(t.due(StreamTopic::Jobs, ""));
  TEST_ASSERT_FALSE(t.due(StreamTopic::Jobs, ""));
}

static void test_topics_do_not_share_what_was_sent() {
  StreamTracker t;
  TEST_ASSERT_TRUE(t.due(StreamTopic::Wall, "{}"));
  TEST_ASSERT_TRUE(t.due(StreamTopic::Verdict, "{}"));
  TEST_ASSERT_FALSE(t.due(StreamTopic::Wall, "{}"));
}

static void test_a_new_reader_gets_every_topic_again() {
  StreamTracker t;
  TEST_ASSERT_TRUE(t.due(StreamTopic::Wall, "a"));
  TEST_ASSERT_TRUE(t.due(StreamTopic::History, "b"));
  t.sendAllAgain();
  TEST_ASSERT_TRUE(t.due(StreamTopic::Wall, "a"));
  TEST_ASSERT_TRUE(t.due(StreamTopic::History, "b"));
  TEST_ASSERT_FALSE(t.due(StreamTopic::Wall, "a"));
}

static void test_every_topic_has_its_own_name() {
  for (int a = 0; a < STREAM_TOPIC_COUNT; a++) {
    TEST_ASSERT_TRUE(strcmp("?", streamTopicName((StreamTopic)a)) != 0);
    for (int b = a + 1; b < STREAM_TOPIC_COUNT; b++) {
      TEST_ASSERT_TRUE(strcmp(streamTopicName((StreamTopic)a), streamTopicName((StreamTopic)b)) != 0);
    }
  }
  TEST_ASSERT_EQUAL_STRING("?", streamTopicName((StreamTopic)STREAM_TOPIC_COUNT));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_document_is_sent_once_and_again_only_when_it_changes);
  RUN_TEST(test_the_first_document_is_sent_even_when_it_is_empty);
  RUN_TEST(test_topics_do_not_share_what_was_sent);
  RUN_TEST(test_a_new_reader_gets_every_topic_again);
  RUN_TEST(test_every_topic_has_its_own_name);
  return UNITY_END();
}
