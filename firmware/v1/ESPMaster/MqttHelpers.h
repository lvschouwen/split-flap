#pragma once

#include <Arduino.h>

// Pure MQTT logic for the Home Assistant integration (issue #121): payload
// parsing, show-then-revert notification state, and topic/discovery/telemetry
// JSON assembly. No networking and no AsyncMqttClient types — everything in
// this header is exercised by `pio test -e native` (test_mqtt_helpers).
//
// Format strings live in PROGMEM on the device (PSTR/snprintf_P). The native
// test env has no pgmspace, so UNIT_TEST builds fall back to plain snprintf.
#ifdef UNIT_TEST
  #include <cstdio>
  #define MQTT_FMT(s) (s)
  #define mqttSnprintf snprintf
#else
  #define MQTT_FMT(s) PSTR(s)
  #define mqttSnprintf snprintf_P
#endif

// Default dwell for a notification without a JSON "dwell" override. The
// sketch can override via -D before including this header.
#ifndef MQTT_TEXT_DWELL_S
#define MQTT_TEXT_DWELL_S 60
#endif
#define MQTT_DWELL_MIN_S  5
#define MQTT_DWELL_MAX_S  3600

inline long clampDwellSeconds(long dwellSeconds) {
  if (dwellSeconds < MQTT_DWELL_MIN_S) return MQTT_DWELL_MIN_S;
  if (dwellSeconds > MQTT_DWELL_MAX_S) return MQTT_DWELL_MAX_S;
  return dwellSeconds;
}

// Minimal parser for {"text":"…","dwell":N}. Hand-rolled on purpose — the
// design doc rejects ArduinoJson (~30 KB flash) for a two-field object.
// Returns true iff `payload` is a JSON object with a string "text" member;
// textOut/dwellSecondsOut are written only on success. "dwell" is optional
// (default MQTT_TEXT_DWELL_S) and clamped. Escapes: \" \\ \n are decoded;
// any other \x yields the literal x. On false the caller treats the whole
// payload as plain text.
inline bool parseMqttTextPayload(const String& payload, String& textOut, long& dwellSecondsOut) {
  String trimmed = payload;
  trimmed.trim();
  if (!trimmed.startsWith("{") || !trimmed.endsWith("}")) return false;

  int keyIndex = trimmed.indexOf("\"text\"");
  if (keyIndex < 0) return false;
  int colonIndex = trimmed.indexOf(':', keyIndex + 6);
  if (colonIndex < 0) return false;
  unsigned int cursor = colonIndex + 1;
  while (cursor < trimmed.length() && (trimmed[cursor] == ' ' || trimmed[cursor] == '\t')) cursor++;
  if (cursor >= trimmed.length() || trimmed[cursor] != '"') return false;

  String text;
  // One-shot reservation: worst case the value runs to the end of the
  // payload. Avoids per-character realloc churn on the ~42 KB ESP-01 heap.
  text.reserve(trimmed.length() - cursor);
  bool closed = false;
  for (unsigned int i = cursor + 1; i < trimmed.length(); i++) {
    char c = trimmed[i];
    if (c == '\\' && i + 1 < trimmed.length()) {
      char next = trimmed[++i];
      if (next == 'n') text += '\n';
      else text += next;
      continue;
    }
    if (c == '"') { closed = true; break; }
    text += c;
  }
  if (!closed) return false;

  long dwellSeconds = MQTT_TEXT_DWELL_S;
  int dwellIndex = trimmed.indexOf("\"dwell\"");
  if (dwellIndex >= 0) {
    int dwellColon = trimmed.indexOf(':', dwellIndex + 7);
    if (dwellColon >= 0) {
      // toInt() skips leading whitespace and stops at the first non-digit.
      long parsed = trimmed.substring(dwellColon + 1).toInt();
      if (parsed > 0) dwellSeconds = parsed;
    }
  }

  textOut = text;
  dwellSecondsOut = clampDwellSeconds(dwellSeconds);
  return true;
}

// Show-then-revert notification state. The main loop stays declarative: it
// asks notificationTick() each pass — true means "the notification owns the
// display, show n.text"; false means normal deviceMode content. Expiry
// clears `active` in place, and the loop's existing lastWrittenText
// comparison re-flaps the previous clock/text content by itself — no saved
// mode, no EEPROM, no RTC.
//
// Times are uint32_t on purpose: millis() wraps at 2^32 on the ESP8266
// (32-bit unsigned long), while the native test host has a 64-bit unsigned
// long — fixed-width types make the wraparound arithmetic identical in
// both environments.
struct MqttNotification {
  bool active = false;
  String text;
  uint32_t deadlineMs = 0;
};

inline void notificationStart(MqttNotification& n, const String& text, long dwellSeconds, uint32_t nowMs) {
  n.active = true;
  n.text = text;
  n.deadlineMs = nowMs + (uint32_t)clampDwellSeconds(dwellSeconds) * 1000UL;
}

// millis()-wraparound-safe via signed difference of fixed-width unsigned.
inline bool notificationTick(MqttNotification& n, uint32_t nowMs) {
  if (!n.active) return false;
  if ((int32_t)(nowMs - n.deadlineMs) >= 0) {
    n.active = false;
    n.text = "";
    return false;
  }
  return true;
}
