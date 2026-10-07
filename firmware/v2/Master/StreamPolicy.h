#pragma once
// StreamPolicy.h — what GET /api/v2/stream sends and when (#559/#572). Pure,
// natively tested by test_stream_policy. The stream carries a few topics,
// each a small JSON document; a topic is sent when its document differs from
// the one last sent, and all of them again when a reader connects.
//
//   event: wall     {"mode","quiet","rows":[{"id","text"},...]}
//   event: verdict  {"wall":"note","boards":[{"id","level","reason","unitLevels"},...]}
//   event: jobs     [{"op","name","state","board","unit","detail"},...]
//   event: history  {"seq":N}     the newest entry of GET /api/v2/history
// "id" / "board" is "" for the master's own row.

#include <stddef.h>
#include <stdint.h>

enum class StreamTopic : uint8_t { Wall = 0, Verdict, Jobs, History };
#define STREAM_TOPIC_COUNT 4

inline const char* streamTopicName(StreamTopic topic) {
  switch (topic) {
    case StreamTopic::Wall: return "wall";
    case StreamTopic::Verdict: return "verdict";
    case StreamTopic::Jobs: return "jobs";
    case StreamTopic::History: return "history";
  }
  return "?";
}

// FNV-1a over the document: what was sent is remembered as this, not kept.
inline uint32_t streamDigest(const char* text) {
  uint32_t h = 2166136261UL;
  for (const char* p = text; *p != 0; p++) {
    h ^= (uint8_t)*p;
    h *= 16777619UL;
  }
  return h;
}

struct StreamTracker {
  bool sent[STREAM_TOPIC_COUNT] = {false};
  uint32_t digest[STREAM_TOPIC_COUNT] = {0};

  // True when this document is to be sent now; it then counts as sent.
  bool due(StreamTopic topic, const char* document) {
    const int i = (int)topic;
    const uint32_t d = streamDigest(document);
    if (sent[i] && digest[i] == d) return false;
    sent[i] = true;
    digest[i] = d;
    return true;
  }

  // A reader connected: every topic goes out again.
  void sendAllAgain() {
    for (bool& s : sent) s = false;
  }
};
