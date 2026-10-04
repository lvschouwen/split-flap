#pragma once
// JsonEscape.h — the one JSON string writer of every board that builds
// replies by hand. Wire strings (a leader name, a device name) come off an
// unauthenticated LAN request and are re-served, so the escaping is a
// boundary and must not differ per tree.

#include <Arduino.h>

// Appends `value` as a quoted JSON string: quotes and backslashes escaped,
// the common control characters by their short form, any other byte below
// 0x20 as \u00XX.
inline void appendJsonString(String& out, const String& value) {
  out += '"';
  for (unsigned int i = 0; i < value.length(); i++) {
    char c = value[i];
    switch (c) {
      case '"':  out += F("\\\""); break;
      case '\\': out += F("\\\\"); break;
      case '\b': out += F("\\b");  break;
      case '\f': out += F("\\f");  break;
      case '\n': out += F("\\n");  break;
      case '\r': out += F("\\r");  break;
      case '\t': out += F("\\t");  break;
      default:
        if ((unsigned char)c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
}
