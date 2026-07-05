# MQTT / Home Assistant Integration Implementation Plan (#121)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let Home Assistant push notification-style text to the split-flap display over MQTT (show for a dwell time, then revert) and monitor its health, compiled out entirely by default and never touching the OTA / EEPROM / RTC paths.

**Architecture:** A new sibling sketch file `ServiceMqttFunctions.ino` joins the single ESPMaster translation unit, using `marvinroger/async-mqtt-client` on the existing ESPAsyncTCP stack. All pure logic (payload parsing, show-then-revert state machine, topic/discovery/telemetry JSON assembly) lives in a new header `MqttHelpers.h` so the native test env exercises it. Async callbacks only copy data and set flags; all display/I2C work happens in the main loop.

**Tech Stack:** PlatformIO, ESP8266 Arduino core (ESP-01 1MB), AsyncMqttClient v0.9.0, Unity + ArduinoFake for native tests.

**Spec:** `docs/superpowers/specs/2026-07-05-mqtt-ha-integration-design.md` (approved). Read it before starting.

## Global Constraints

- `MQTT_ENABLE` defaults to **false** in committed source; with it off the feature is compiled out: zero flash, zero RAM, zero behavior change.
- **Zero new EEPROM slots, zero RTC memory use.** `SettingsEepromLayout.h` and `RtcBootState.h` are byte-for-byte untouched.
- Library pinned to an exact tag in `platformio.ini`: `https://github.com/marvinroger/async-mqtt-client.git#v0.9.0` (same convention as the dvarrel libs).
- No ArduinoJson. Hand-rolled two-field parser + snprintf assembly only.
- No TLS. Plain MQTT on the trusted LAN, compile-time credentials in gitignored `MqttCredentials.h`.
- Dwell clamped to 5–3600 s, default 60 s (`MQTT_TEXT_DWELL_S`). Telemetry every 60 s (`MQTT_TELEMETRY_INTERVAL_S`).
- MQTT never initializes in quiet-OTA or recovery boots; disconnects when a `/firmware/master` upload starts.
- async-mqtt-client callbacks run in LWIP context: **copy + flag only**, never display/I2C/String-heavy work.
- ESP-01 RAM is ~42 KB free at rest — before merge, measure the heap and sketch-size gates (Task 8).
- Working branch: `pcb-v2-rs485-48v` (where all current firmware work lands). Conventional commits referencing #121.
- All `pio` commands run from `firmware/v1/ESPMaster/`.

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `firmware/v1/ESPMaster/MqttHelpers.h` | create | Pure logic: dwell clamp, payload parser, notification state machine, topic/telemetry/discovery builders. Natively testable (UNIT_TEST shim for `snprintf_P`/`PSTR`). |
| `firmware/v1/ESPMaster/test/test_mqtt_helpers/test_main.cpp` | create | Native tests for everything in MqttHelpers.h. |
| `firmware/v1/ESPMaster/ServiceMqttFunctions.ino` | create | AsyncMqttClient wiring: init, callbacks→flags, main-loop pump (reconnect backoff, message processing, telemetry), OTA stop/resume hooks. Whole file `#if MQTT_ENABLE == true`, with no-op stubs in `#else`. |
| `firmware/v1/ESPMaster/MqttCredentials.h.example` | create | Broker host/port/user/pass/device-id template (real file gitignored). |
| `firmware/v1/ESPMaster/ESPMaster.ino` | modify | `MQTT_ENABLE` define block, credentials include, `initMqtt()` in setup, `loopMqtt()` + notification tick in loop, OTA-handler hooks, `lastShowUnitWriteErrors` global. |
| `firmware/v1/ESPMaster/ESPMaster.h` | modify | Prototypes + `extern int lastShowUnitWriteErrors`. |
| `firmware/v1/ESPMaster/ServiceFlapFunctions.ino` | modify | `writeToUnit` returns Wire status; `showMessage` tallies write failures into `lastShowUnitWriteErrors`. |
| `firmware/v1/ESPMaster/platformio.ini` | modify | lib_deps pin + new `[env:espmaster_mqtt]`. |
| `.gitignore` | modify | `firmware/**/MqttCredentials.h`. |
| `.github/workflows/build.yml` | modify | Build the `espmaster_mqtt` env so the default-off code path can't bit-rot. |
| `CLAUDE.md` | modify | Document the new sketch file, header, and config knobs. |

---

### Task 1: Payload parser + dwell clamp (MqttHelpers.h)

**Files:**
- Create: `firmware/v1/ESPMaster/MqttHelpers.h`
- Create: `firmware/v1/ESPMaster/test/test_mqtt_helpers/test_main.cpp`

**Interfaces:**
- Produces: `long clampDwellSeconds(long)`, `bool parseMqttTextPayload(const String& payload, String& textOut, long& dwellSecondsOut)`, defines `MQTT_TEXT_DWELL_S` (60), `MQTT_DWELL_MIN_S` (5), `MQTT_DWELL_MAX_S` (3600), and the `MQTT_FMT`/`mqttSnprintf` UNIT_TEST shim used by Task 3.

- [ ] **Step 1: Write the failing tests**

Create `firmware/v1/ESPMaster/test/test_mqtt_helpers/test_main.cpp`:

```cpp
// Host-side unit tests for the pure MQTT logic in MqttHelpers.h (#121).
// No networking, no AsyncMqttClient — only parsing, state, and JSON assembly.

#include <ArduinoFake.h>
#include <unity.h>
#include <cstring>
#include "../../MqttHelpers.h"

using namespace fakeit;

void setUp() {
  ArduinoFakeReset();
}
void tearDown() {}

// ---- clampDwellSeconds ----
static void test_clampDwell_passes_through_in_range() {
  TEST_ASSERT_EQUAL_INT32(60, clampDwellSeconds(60));
  TEST_ASSERT_EQUAL_INT32(5, clampDwellSeconds(5));
  TEST_ASSERT_EQUAL_INT32(3600, clampDwellSeconds(3600));
}
static void test_clampDwell_clamps_out_of_range() {
  TEST_ASSERT_EQUAL_INT32(5, clampDwellSeconds(0));
  TEST_ASSERT_EQUAL_INT32(5, clampDwellSeconds(-100));
  TEST_ASSERT_EQUAL_INT32(3600, clampDwellSeconds(99999));
}

// ---- parseMqttTextPayload ----
static void test_parse_plain_text_returns_false() {
  String text; long dwell = -1;
  TEST_ASSERT_FALSE(parseMqttTextPayload(String("HELLO WORLD"), text, dwell));
}
static void test_parse_valid_json_text_only_uses_default_dwell() {
  String text; long dwell = -1;
  TEST_ASSERT_TRUE(parseMqttTextPayload(String("{\"text\":\"DOORBELL\"}"), text, dwell));
  TEST_ASSERT_EQUAL_STRING("DOORBELL", text.c_str());
  TEST_ASSERT_EQUAL_INT32(MQTT_TEXT_DWELL_S, dwell);
}
static void test_parse_valid_json_with_dwell() {
  String text; long dwell = -1;
  TEST_ASSERT_TRUE(parseMqttTextPayload(String("{\"text\":\"HI\",\"dwell\":120}"), text, dwell));
  TEST_ASSERT_EQUAL_STRING("HI", text.c_str());
  TEST_ASSERT_EQUAL_INT32(120, dwell);
}
static void test_parse_dwell_is_clamped() {
  String text; long dwell = -1;
  TEST_ASSERT_TRUE(parseMqttTextPayload(String("{\"text\":\"A\",\"dwell\":1}"), text, dwell));
  TEST_ASSERT_EQUAL_INT32(MQTT_DWELL_MIN_S, dwell);
  TEST_ASSERT_TRUE(parseMqttTextPayload(String("{\"text\":\"A\",\"dwell\":99999}"), text, dwell));
  TEST_ASSERT_EQUAL_INT32(MQTT_DWELL_MAX_S, dwell);
}
static void test_parse_whitespace_tolerant() {
  String text; long dwell = -1;
  TEST_ASSERT_TRUE(parseMqttTextPayload(String("  { \"text\" : \"SPACED\" , \"dwell\" : 30 }  "), text, dwell));
  TEST_ASSERT_EQUAL_STRING("SPACED", text.c_str());
  TEST_ASSERT_EQUAL_INT32(30, dwell);
}
static void test_parse_escaped_quote_and_backslash() {
  String text; long dwell = -1;
  TEST_ASSERT_TRUE(parseMqttTextPayload(String("{\"text\":\"A\\\"B\\\\C\"}"), text, dwell));
  TEST_ASSERT_EQUAL_STRING("A\"B\\C", text.c_str());
}
static void test_parse_escaped_newline() {
  String text; long dwell = -1;
  TEST_ASSERT_TRUE(parseMqttTextPayload(String("{\"text\":\"LINE1\\nLINE2\"}"), text, dwell));
  TEST_ASSERT_EQUAL_STRING("LINE1\nLINE2", text.c_str());
}
static void test_parse_malformed_json_returns_false() {
  String text; long dwell = -1;
  // Starts with { but is garbage — caller must fall back to plain text.
  TEST_ASSERT_FALSE(parseMqttTextPayload(String("{garbage"), text, dwell));
  TEST_ASSERT_FALSE(parseMqttTextPayload(String("{\"dwell\":30}"), text, dwell));        // no "text" key
  TEST_ASSERT_FALSE(parseMqttTextPayload(String("{\"text\":\"UNTERMINATED}"), text, dwell)); // unterminated string
  TEST_ASSERT_FALSE(parseMqttTextPayload(String("{\"text\":42}"), text, dwell));          // non-string text
}
static void test_parse_empty_text_is_valid() {
  String text = "x"; long dwell = -1;
  TEST_ASSERT_TRUE(parseMqttTextPayload(String("{\"text\":\"\"}"), text, dwell));
  TEST_ASSERT_EQUAL_STRING("", text.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_clampDwell_passes_through_in_range);
  RUN_TEST(test_clampDwell_clamps_out_of_range);
  RUN_TEST(test_parse_plain_text_returns_false);
  RUN_TEST(test_parse_valid_json_text_only_uses_default_dwell);
  RUN_TEST(test_parse_valid_json_with_dwell);
  RUN_TEST(test_parse_dwell_is_clamped);
  RUN_TEST(test_parse_whitespace_tolerant);
  RUN_TEST(test_parse_escaped_quote_and_backslash);
  RUN_TEST(test_parse_escaped_newline);
  RUN_TEST(test_parse_malformed_json_returns_false);
  RUN_TEST(test_parse_empty_text_is_valid);
  return UNITY_END();
}
```

Note: check how the existing `test/test_string_handling/test_main.cpp` declares its runner (`main` vs `setup/loop`) and copy that convention exactly; adjust the runner block above if it differs.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `pio test -e native -f test_mqtt_helpers`
Expected: FAIL — `MqttHelpers.h: No such file or directory`

- [ ] **Step 3: Write the implementation**

Create `firmware/v1/ESPMaster/MqttHelpers.h`:

```cpp
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
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `pio test -e native -f test_mqtt_helpers`
Expected: PASS (11 tests)

Also run the full suite to prove no regression: `pio test -e native`
Expected: all test dirs PASS

- [ ] **Step 5: Commit**

```bash
git add firmware/v1/ESPMaster/MqttHelpers.h firmware/v1/ESPMaster/test/test_mqtt_helpers/test_main.cpp
git commit -m "feat(firmware/v1/ESPMaster): MQTT payload parser + dwell clamp helpers (#121)"
```

---

### Task 2: Show-then-revert notification state machine (MqttHelpers.h)

**Files:**
- Modify: `firmware/v1/ESPMaster/MqttHelpers.h` (append)
- Modify: `firmware/v1/ESPMaster/test/test_mqtt_helpers/test_main.cpp` (append)

**Interfaces:**
- Consumes: `clampDwellSeconds` from Task 1.
- Produces: `struct MqttNotification { bool active; String text; unsigned long deadlineMs; }`, `void notificationStart(MqttNotification&, const String& text, long dwellSeconds, unsigned long nowMs)`, `bool notificationTick(MqttNotification&, unsigned long nowMs)` (true while the notification owns the display; flips itself inactive on expiry).

Design note (documented deviation from the spec's wording): the spec says "save current device mode and displayed text, restore on expiry". The main loop is already declarative — every second it re-derives the display content from `deviceMode`/`inputText`. So no save/restore state is needed: while a notification is active the loop shows its text; on expiry the normal mode selection resumes and `showText`'s `lastWrittenText` comparison re-flaps the previous content automatically. Same observable behavior, strictly less state.

- [ ] **Step 1: Write the failing tests** (append to `test_main.cpp` before `main`, and add the `RUN_TEST` lines)

```cpp
// ---- MqttNotification state machine ----
static void test_notification_inactive_by_default() {
  MqttNotification n;
  TEST_ASSERT_FALSE(notificationTick(n, 1000UL));
}
static void test_notification_active_until_deadline() {
  MqttNotification n;
  notificationStart(n, String("DOORBELL"), 60, 10000UL);
  TEST_ASSERT_TRUE(notificationTick(n, 10000UL));
  TEST_ASSERT_TRUE(notificationTick(n, 10000UL + 59999UL));
  TEST_ASSERT_EQUAL_STRING("DOORBELL", n.text.c_str());
}
static void test_notification_expires_at_deadline() {
  MqttNotification n;
  notificationStart(n, String("DOORBELL"), 60, 10000UL);
  TEST_ASSERT_FALSE(notificationTick(n, 10000UL + 60000UL));
  // and stays expired
  TEST_ASSERT_FALSE(notificationTick(n, 10000UL + 60001UL));
  TEST_ASSERT_FALSE(n.active);
}
static void test_notification_replacement_resets_deadline() {
  MqttNotification n;
  notificationStart(n, String("FIRST"), 60, 0UL);
  notificationStart(n, String("SECOND"), 60, 50000UL);
  TEST_ASSERT_TRUE(notificationTick(n, 100000UL));  // 60 s past FIRST's start, but SECOND runs to 110 s
  TEST_ASSERT_EQUAL_STRING("SECOND", n.text.c_str());
  TEST_ASSERT_FALSE(notificationTick(n, 110000UL));
}
static void test_notification_start_clamps_dwell() {
  MqttNotification n;
  notificationStart(n, String("X"), 999999, 0UL);
  TEST_ASSERT_TRUE(notificationTick(n, 3599999UL));   // 3600 s - 1 ms
  TEST_ASSERT_FALSE(notificationTick(n, 3600000UL));
}
static void test_notification_survives_millis_wraparound() {
  MqttNotification n;
  // Start 5 s before millis() wraps; a 60 s dwell must stay active across 0.
  unsigned long nearWrap = 0xFFFFFFFFUL - 5000UL;
  notificationStart(n, String("WRAP"), 60, nearWrap);
  TEST_ASSERT_TRUE(notificationTick(n, 0xFFFFFFFFUL));
  TEST_ASSERT_TRUE(notificationTick(n, 10000UL));    // 15 s in, post-wrap
  TEST_ASSERT_FALSE(notificationTick(n, 55001UL));   // 60.001 s in
}
```

`RUN_TEST` additions in `main`:

```cpp
  RUN_TEST(test_notification_inactive_by_default);
  RUN_TEST(test_notification_active_until_deadline);
  RUN_TEST(test_notification_expires_at_deadline);
  RUN_TEST(test_notification_replacement_resets_deadline);
  RUN_TEST(test_notification_start_clamps_dwell);
  RUN_TEST(test_notification_survives_millis_wraparound);
```

- [ ] **Step 2: Run tests to verify the new ones fail**

Run: `pio test -e native -f test_mqtt_helpers`
Expected: FAIL — `MqttNotification` not declared

- [ ] **Step 3: Write the implementation** (append to `MqttHelpers.h`)

```cpp
// Show-then-revert notification state. The main loop stays declarative: it
// asks notificationTick() each pass — true means "the notification owns the
// display, show n.text"; false means normal deviceMode content. Expiry
// clears `active` in place, and the loop's existing lastWrittenText
// comparison re-flaps the previous clock/text content by itself — no saved
// mode, no EEPROM, no RTC.
struct MqttNotification {
  bool active = false;
  String text;
  unsigned long deadlineMs = 0;
};

inline void notificationStart(MqttNotification& n, const String& text, long dwellSeconds, unsigned long nowMs) {
  n.active = true;
  n.text = text;
  n.deadlineMs = nowMs + (unsigned long)clampDwellSeconds(dwellSeconds) * 1000UL;
}

// millis()-wraparound-safe via signed difference of unsigned longs.
inline bool notificationTick(MqttNotification& n, unsigned long nowMs) {
  if (!n.active) return false;
  if ((long)(nowMs - n.deadlineMs) >= 0) {
    n.active = false;
    n.text = "";
    return false;
  }
  return true;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `pio test -e native -f test_mqtt_helpers`
Expected: PASS (17 tests)

- [ ] **Step 5: Commit**

```bash
git add firmware/v1/ESPMaster/MqttHelpers.h firmware/v1/ESPMaster/test/test_mqtt_helpers/test_main.cpp
git commit -m "feat(firmware/v1/ESPMaster): MQTT show-then-revert notification state machine (#121)"
```

---

### Task 3: Topic, telemetry, and HA discovery builders (MqttHelpers.h)

**Files:**
- Modify: `firmware/v1/ESPMaster/MqttHelpers.h` (append)
- Modify: `firmware/v1/ESPMaster/test/test_mqtt_helpers/test_main.cpp` (append)

**Interfaces:**
- Consumes: `MQTT_FMT`/`mqttSnprintf` shim from Task 1.
- Produces:
  - `String mqttTopic(const String& deviceId, const char* suffix)` → `splitflap/<id>/<suffix>`
  - `enum MqttDiscoveryEntity { DISCOVERY_TEXT = 0, DISCOVERY_HEAP, DISCOVERY_RSSI, DISCOVERY_UNIT_ERRORS, DISCOVERY_ENTITY_COUNT }`
  - `size_t buildDiscoveryTopic(char* buf, size_t bufLen, int entity, const char* deviceId)`
  - `size_t buildDiscoveryPayload(char* buf, size_t bufLen, int entity, const char* deviceId, const char* fwVersion)`
  - `size_t buildTelemetryPayload(char* buf, size_t bufLen, uint32_t freeHeap, long rssi, int unitErrors)`

- [ ] **Step 1: Write the failing tests** (append + `RUN_TEST` lines)

```cpp
// ---- topics / telemetry / discovery ----
static void test_mqttTopic_builds_expected_paths() {
  TEST_ASSERT_EQUAL_STRING("splitflap/flappy/text/set", mqttTopic(String("flappy"), "text/set").c_str());
  TEST_ASSERT_EQUAL_STRING("splitflap/flappy/availability", mqttTopic(String("flappy"), "availability").c_str());
  TEST_ASSERT_EQUAL_STRING("splitflap/flappy/telemetry", mqttTopic(String("flappy"), "telemetry").c_str());
}
static void test_telemetry_payload_exact_shape() {
  char buf[96];
  size_t n = buildTelemetryPayload(buf, sizeof(buf), 25048UL, -61L, 2);
  TEST_ASSERT_EQUAL_STRING("{\"heap\":25048,\"rssi\":-61,\"unitErrors\":2}", buf);
  TEST_ASSERT_EQUAL_UINT32(strlen(buf), (uint32_t)n);
}
static void test_discovery_topics_per_entity() {
  char buf[64];
  buildDiscoveryTopic(buf, sizeof(buf), DISCOVERY_TEXT, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/text/flappy/config", buf);
  buildDiscoveryTopic(buf, sizeof(buf), DISCOVERY_HEAP, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/flappy_heap/config", buf);
  buildDiscoveryTopic(buf, sizeof(buf), DISCOVERY_RSSI, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/flappy_rssi/config", buf);
  buildDiscoveryTopic(buf, sizeof(buf), DISCOVERY_UNIT_ERRORS, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/flappy_unit_errors/config", buf);
}
// The discovery payloads are fixed-shape; assert the load-bearing fragments
// rather than byte-for-byte JSON, so cosmetic edits don't churn tests.
static void assert_contains(const char* haystack, const char* needle) {
  if (strstr(haystack, needle) == nullptr) {
    TEST_FAIL_MESSAGE(needle);
  }
}
static void test_discovery_text_payload_fragments() {
  char buf[512];
  size_t n = buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_TEXT, "flappy", "abc1234");
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));  // no truncation
  TEST_ASSERT_EQUAL_CHAR('{', buf[0]);
  TEST_ASSERT_EQUAL_CHAR('}', buf[n - 1]);
  assert_contains(buf, "\"cmd_t\":\"splitflap/flappy/text/set\"");
  assert_contains(buf, "\"avty_t\":\"splitflap/flappy/availability\"");
  assert_contains(buf, "\"uniq_id\":\"flappy_text\"");
  assert_contains(buf, "\"ids\":[\"flappy\"]");
  assert_contains(buf, "\"sw\":\"abc1234\"");
}
static void test_discovery_sensor_payload_fragments() {
  char buf[512];
  size_t n = buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_HEAP, "flappy", "abc1234");
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  assert_contains(buf, "\"stat_t\":\"splitflap/flappy/telemetry\"");
  assert_contains(buf, "\"val_tpl\":\"{{ value_json.heap }}\"");
  assert_contains(buf, "\"uniq_id\":\"flappy_heap\"");

  buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_RSSI, "flappy", "abc1234");
  assert_contains(buf, "\"val_tpl\":\"{{ value_json.rssi }}\"");
  assert_contains(buf, "\"dev_cla\":\"signal_strength\"");

  buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_UNIT_ERRORS, "flappy", "abc1234");
  assert_contains(buf, "\"val_tpl\":\"{{ value_json.unitErrors }}\"");
}
```

`RUN_TEST` additions:

```cpp
  RUN_TEST(test_mqttTopic_builds_expected_paths);
  RUN_TEST(test_telemetry_payload_exact_shape);
  RUN_TEST(test_discovery_topics_per_entity);
  RUN_TEST(test_discovery_text_payload_fragments);
  RUN_TEST(test_discovery_sensor_payload_fragments);
```

- [ ] **Step 2: Run tests to verify the new ones fail**

Run: `pio test -e native -f test_mqtt_helpers`
Expected: FAIL — `mqttTopic` not declared

- [ ] **Step 3: Write the implementation** (append to `MqttHelpers.h`)

```cpp
// ---- Topics ----
inline String mqttTopic(const String& deviceId, const char* suffix) {
  return String("splitflap/") + deviceId + "/" + suffix;
}

// ---- Telemetry ----
// One JSON packet serves all three HA sensors via value_json templates.
inline size_t buildTelemetryPayload(char* buf, size_t bufLen, uint32_t freeHeap, long rssi, int unitErrors) {
  return (size_t)mqttSnprintf(buf, bufLen,
      MQTT_FMT("{\"heap\":%lu,\"rssi\":%ld,\"unitErrors\":%d}"),
      (unsigned long)freeHeap, rssi, unitErrors);
}

// ---- HA MQTT Discovery ----
// One Text entity + three sensors sharing a single device block, so HA
// groups them under one device. Abbreviated keys (cmd_t, avty_t, uniq_id,
// stat_t, val_tpl, dev_cla, dev, ids, mf, mdl, sw) are the documented HA
// discovery short names — they keep payloads well inside the 512-byte
// assembly buffer.
enum MqttDiscoveryEntity {
  DISCOVERY_TEXT = 0,
  DISCOVERY_HEAP,
  DISCOVERY_RSSI,
  DISCOVERY_UNIT_ERRORS,
  DISCOVERY_ENTITY_COUNT
};

inline size_t buildDiscoveryTopic(char* buf, size_t bufLen, int entity, const char* deviceId) {
  switch (entity) {
    case DISCOVERY_TEXT:        return (size_t)mqttSnprintf(buf, bufLen, MQTT_FMT("homeassistant/text/%s/config"), deviceId);
    case DISCOVERY_HEAP:        return (size_t)mqttSnprintf(buf, bufLen, MQTT_FMT("homeassistant/sensor/%s_heap/config"), deviceId);
    case DISCOVERY_RSSI:        return (size_t)mqttSnprintf(buf, bufLen, MQTT_FMT("homeassistant/sensor/%s_rssi/config"), deviceId);
    case DISCOVERY_UNIT_ERRORS: return (size_t)mqttSnprintf(buf, bufLen, MQTT_FMT("homeassistant/sensor/%s_unit_errors/config"), deviceId);
  }
  if (bufLen > 0) buf[0] = '\0';
  return 0;
}

// Shared device block; %s slots are (deviceId, deviceId, fwVersion).
#define MQTT_DEVICE_BLOCK "\"dev\":{\"ids\":[\"%s\"],\"name\":\"Split-Flap %s\",\"mf\":\"split-flap\",\"mdl\":\"v1 ESPMaster\",\"sw\":\"%s\"}"

inline size_t buildDiscoveryPayload(char* buf, size_t bufLen, int entity, const char* deviceId, const char* fwVersion) {
  switch (entity) {
    case DISCOVERY_TEXT:
      return (size_t)mqttSnprintf(buf, bufLen,
        MQTT_FMT("{\"name\":\"Text\",\"cmd_t\":\"splitflap/%s/text/set\",\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s_text\",\"max\":255," MQTT_DEVICE_BLOCK "}"),
        deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion);
    case DISCOVERY_HEAP:
      return (size_t)mqttSnprintf(buf, bufLen,
        MQTT_FMT("{\"name\":\"Free heap\",\"stat_t\":\"splitflap/%s/telemetry\",\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s_heap\",\"val_tpl\":\"{{ value_json.heap }}\",\"unit_of_meas\":\"B\"," MQTT_DEVICE_BLOCK "}"),
        deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion);
    case DISCOVERY_RSSI:
      return (size_t)mqttSnprintf(buf, bufLen,
        MQTT_FMT("{\"name\":\"WiFi RSSI\",\"stat_t\":\"splitflap/%s/telemetry\",\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s_rssi\",\"val_tpl\":\"{{ value_json.rssi }}\",\"unit_of_meas\":\"dBm\",\"dev_cla\":\"signal_strength\"," MQTT_DEVICE_BLOCK "}"),
        deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion);
    case DISCOVERY_UNIT_ERRORS:
      return (size_t)mqttSnprintf(buf, bufLen,
        MQTT_FMT("{\"name\":\"Unit errors\",\"stat_t\":\"splitflap/%s/telemetry\",\"avty_t\":\"splitflap/%s/availability\",\"uniq_id\":\"%s_unit_errors\",\"val_tpl\":\"{{ value_json.unitErrors }}\"," MQTT_DEVICE_BLOCK "}"),
        deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion);
  }
  if (bufLen > 0) buf[0] = '\0';
  return 0;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `pio test -e native -f test_mqtt_helpers`
Expected: PASS (22 tests). Then full suite: `pio test -e native` → all PASS.

- [ ] **Step 5: Commit**

```bash
git add firmware/v1/ESPMaster/MqttHelpers.h firmware/v1/ESPMaster/test/test_mqtt_helpers/test_main.cpp
git commit -m "feat(firmware/v1/ESPMaster): MQTT topic/telemetry/HA-discovery builders (#121)"
```

---

### Task 4: Build config — library pin, MQTT env, credentials template

**Files:**
- Modify: `firmware/v1/ESPMaster/platformio.ini`
- Create: `firmware/v1/ESPMaster/MqttCredentials.h.example`
- Modify: `.gitignore`
- Modify: `firmware/v1/ESPMaster/ESPMaster.ino` (define block + credentials include only)

**Interfaces:**
- Produces: `MQTT_ENABLE` / `MQTT_TELEMETRY_INTERVAL_S` / `MQTT_MAX_TEXT_LEN` defines; globals `mqttBrokerHost` (const char*), `mqttBrokerPort` (uint16_t), `mqttUsername`, `mqttPassword`, `mqttDeviceId` (const char*) — consumed by Task 6's `initMqtt()`. New PlatformIO env `espmaster_mqtt`.

- [ ] **Step 1: Add the library pin to `[espmaster_common]` lib_deps** in `platformio.ini`:

```ini
lib_deps =
  https://github.com/dvarrel/ESPAsyncTCP.git#1.2.4
  https://github.com/dvarrel/ESPAsyncWebSrv.git#1.2.9
  https://github.com/alanswx/ESPAsyncWiFiManager.git#3b9a58a031d32da908e205d60c7c58eedc8bd6ee
  https://github.com/marvinroger/async-mqtt-client.git#v0.9.0
  EEPROM
```

(The library rides the ESPAsyncTCP stack already in lib_deps. With `MQTT_ENABLE` false its objects are dropped by `--gc-sections`; Task 8's size gate proves the default image is unchanged within noise.)

- [ ] **Step 2: Add the MQTT build env** at the bottom of `platformio.ini`, after `[env:espmaster]`:

```ini
; MQTT / Home Assistant variant (#121). Identical board/ldscript; flips the
; compile-time MQTT_ENABLE gate. CI builds this env so the default-off MQTT
; code path can't bit-rot. Requires MqttCredentials.h for a usable device
; (builds fine without — falls back to empty broker = MQTT disabled at boot).
[env:espmaster_mqtt]
extends = espmaster_common
board = esp01_1m
board_build.ldscript = eagle.flash.1m.ld
build_flags =
  ${espmaster_common.build_flags}
  -D MQTT_ENABLE=true
```

- [ ] **Step 3: Create `firmware/v1/ESPMaster/MqttCredentials.h.example`**:

```cpp
// Template for MqttCredentials.h — used when MQTT_ENABLE == true.
//
// Copy this file to MqttCredentials.h (which is gitignored) and fill in
// your broker details. The actual MqttCredentials.h is NOT tracked by git
// so your credentials never land in a commit.
//
// If you build with MQTT_ENABLE == false (the default) you don't need
// this file at all.
#pragma once

const char*    mqttBrokerHost = "192.168.1.10";  // broker IP or hostname; "" disables MQTT at boot
const uint16_t mqttBrokerPort = 1883;
const char*    mqttUsername   = "";              // "" = anonymous
const char*    mqttPassword   = "";
const char*    mqttDeviceId   = "";              // "" = fall back to mdnsName
```

- [ ] **Step 4: Gitignore the real credentials file.** In `.gitignore`, extend the generated-headers block:

```
firmware/**/BuildVersion.h
firmware/**/WebAssets.h
firmware/**/WifiCredentials.h
firmware/**/MqttCredentials.h
```

- [ ] **Step 5: Add the define block to `ESPMaster.ino`.** In the "Configurable Defines" section, directly below the `#define USE_MULTICAST` line:

```cpp
//MQTT / Home Assistant integration (#121). Default OFF — the whole feature
//is compiled out (zero flash, zero RAM, zero behavior change). Build the
//`espmaster_mqtt` PlatformIO env (which passes -D MQTT_ENABLE=true) and
//copy MqttCredentials.h.example to MqttCredentials.h to enable it.
#ifndef MQTT_ENABLE
#define MQTT_ENABLE         false
#endif
#define MQTT_TELEMETRY_INTERVAL_S 60   //Seconds between MQTT health telemetry publishes
#define MQTT_MAX_TEXT_LEN         256  //Inbound MQTT payload buffer cap (bytes)
```

- [ ] **Step 6: Add the credentials include to `ESPMaster.ino`.** Directly below the existing `WifiCredentials.h` `#if/#else/#endif` block (which ends around line 109), mirroring its pattern:

```cpp
//MQTT broker credentials live in a gitignored local header, same pattern as
//WifiCredentials.h above. Copy MqttCredentials.h.example to
//MqttCredentials.h. Missing file → empty broker host → initMqtt() logs and
//disables itself, the build still compiles (fresh checkout, CI).
#if MQTT_ENABLE == true
  #if __has_include("MqttCredentials.h")
    #include "MqttCredentials.h"
  #else
    #warning "MQTT_ENABLE is true but MqttCredentials.h is missing — copy MqttCredentials.h.example to fill it in."
    const char*    mqttBrokerHost = "";
    const uint16_t mqttBrokerPort = 1883;
    const char*    mqttUsername   = "";
    const char*    mqttPassword   = "";
    const char*    mqttDeviceId   = "";
  #endif
#endif
```

- [ ] **Step 7: Verify both envs build**

Run: `pio run -e espmaster && pio run -e espmaster_mqtt`
Expected: SUCCESS twice. Record the `Flash: used` and `RAM: used` numbers for both — they are the Task 8 baseline. (No MQTT code exists yet, so the two images should be nearly identical.)

- [ ] **Step 8: Commit**

```bash
git add firmware/v1/ESPMaster/platformio.ini firmware/v1/ESPMaster/MqttCredentials.h.example .gitignore firmware/v1/ESPMaster/ESPMaster.ino
git commit -m "feat(firmware/v1/ESPMaster): MQTT build env, pinned async-mqtt-client, credentials template (#121)"
```

---

### Task 5: Unit write-error tally (telemetry input)

**Files:**
- Modify: `firmware/v1/ESPMaster/ServiceFlapFunctions.ino`
- Modify: `firmware/v1/ESPMaster/ESPMaster.ino`
- Modify: `firmware/v1/ESPMaster/ESPMaster.h`

**Interfaces:**
- Produces: `extern int lastShowUnitWriteErrors` — count of units whose letter write failed on the Wire bus during the most recent `showMessage()` pass. Consumed by Task 6's telemetry.

This is Wire-bus code, not natively testable; the test cycle is "both envs build clean" (behavior verified on hardware in Task 8).

- [ ] **Step 1: Add the global to `ESPMaster.ino`**, directly below the `int displayState[UNITS_AMOUNT];` line (~line 165):

```cpp
//Units whose most recent letter write failed at the Wire level
//(endTransmission != 0) during the last showMessage() pass. Surfaced via
//MQTT health telemetry (#121); harmless standalone counter otherwise.
int lastShowUnitWriteErrors = 0;
```

- [ ] **Step 2: Add the extern to `ESPMaster.h`**, at the end of the file:

```cpp
// Wire-level write failures from the last showMessage() pass. Defined in
// ESPMaster.ino, updated by ServiceFlapFunctions.ino, published by the MQTT
// telemetry in ServiceMqttFunctions.ino (#121).
extern int lastShowUnitWriteErrors;
```

- [ ] **Step 3: Make `writeToUnit` report status.** In `ServiceFlapFunctions.ino`, change the signature and return (currently `void writeToUnit(...)` ending with a bare `Wire.endTransmission();`):

```cpp
//Write letter position and speed in rpm to single unit (by 0-based unit
//index). Returns Wire.endTransmission() status (0 = success) so callers
//can tally bus-level failures (#121).
int writeToUnit(int unitIndex, int letter, int flapSpeed) {
  int sendArray[2] = {letter, flapSpeed}; //Array with values to send to unit

  Wire.beginTransmission(toI2cAddress(unitIndex));

  //Write values to send to slave in buffer
  for (unsigned int index = 0; index < sizeof sendArray / sizeof sendArray[0]; index++) {
    SerialPrint(F("sendArray: "));
    SerialPrintln(sendArray[index]);

    Wire.write(sendArray[index]);
  }
  return Wire.endTransmission(); //send values to unit
}
```

- [ ] **Step 4: Tally in `showMessage`.** In `ServiceFlapFunctions.ino`, inside `showMessage`'s per-unit write loop, replace:

```cpp
    //only write to unit if char exists in letter array
    if (currentLetterPosition != -1) {
      writeToUnit(unitIndex, currentLetterPosition, flapSpeed);
      commandedLetters[unitIndex] = currentLetterPosition;
    }
```

with:

```cpp
    //only write to unit if char exists in letter array
    if (currentLetterPosition != -1) {
      if (writeToUnit(unitIndex, currentLetterPosition, flapSpeed) != 0) {
        writeErrors++;
      }
      commandedLetters[unitIndex] = currentLetterPosition;
    }
```

and declare/publish the counter around the loop — `int writeErrors = 0;` immediately before the `for (int unitIndex = 0; unitIndex < UNITS_AMOUNT; unitIndex++) {` write loop, and `lastShowUnitWriteErrors = writeErrors;` immediately after the loop's closing brace (before the exit `waitForDisplayToStop()` call). Do NOT tally inside `verifyAndResendLetters` — the counter means "last commanded pass", and re-sends would double-count.

- [ ] **Step 5: Verify build + native tests untouched**

Run: `pio run -e espmaster && pio test -e native`
Expected: build SUCCESS, all tests PASS.

- [ ] **Step 6: Commit**

```bash
git add firmware/v1/ESPMaster/ServiceFlapFunctions.ino firmware/v1/ESPMaster/ESPMaster.ino firmware/v1/ESPMaster/ESPMaster.h
git commit -m "feat(firmware/v1/ESPMaster): tally Wire-level write failures per show pass (#121)"
```

---

### Task 6: ServiceMqttFunctions.ino + lifecycle integration

**Files:**
- Create: `firmware/v1/ESPMaster/ServiceMqttFunctions.ino`
- Modify: `firmware/v1/ESPMaster/ESPMaster.h`
- Modify: `firmware/v1/ESPMaster/ESPMaster.ino`

**Interfaces:**
- Consumes: everything from Tasks 1–5, plus existing globals `mdnsName`, `espVersion`, `showText(String)`, `masterOtaUploadActive`.
- Produces (all no-op stubs when `MQTT_ENABLE == false`):
  - `void initMqtt()` — call once from setup, normal boot only
  - `void loopMqtt()` — call every loop pass
  - `bool mqttNotificationTick()` — true while a notification owns the display (and shows it)
  - `void mqttStopForOta()` — publish `offline` + disconnect when an upload starts
  - `void mqttResumeAfterOta()` — re-enable after a failed/stalled upload

- [ ] **Step 1: Add prototypes to `ESPMaster.h`** (end of file):

```cpp
// MQTT / Home Assistant integration (issue #121). Defined in
// ServiceMqttFunctions.ino; every one of these is a no-op stub when
// MQTT_ENABLE is false, so call sites need no #if guards.
void initMqtt();
void loopMqtt();
bool mqttNotificationTick();
void mqttStopForOta();
void mqttResumeAfterOta();
```

- [ ] **Step 2: Create `firmware/v1/ESPMaster/ServiceMqttFunctions.ino`**:

```cpp
//MQTT / Home Assistant integration (issue #121). Design doc:
//docs/superpowers/specs/2026-07-05-mqtt-ha-integration-design.md
//
//The whole implementation is gated on MQTT_ENABLE (default false in
//ESPMaster.ino) — the #else branch at the bottom provides no-op stubs so
//the call sites in ESPMaster.ino compile without their own guards, and
//--gc-sections drops AsyncMqttClient entirely from the default image.
//
//Threading model (non-negotiable, see design doc): AsyncMqttClient
//callbacks run in LWIP/sys context. They ONLY copy bytes and set flags.
//Everything that touches the display, I2C, Strings-at-scale, or publishes
//discovery happens in loopMqtt() on the main loop.

#if MQTT_ENABLE == true

#include <AsyncMqttClient.h>
#include "MqttHelpers.h"

static AsyncMqttClient mqttClient;

//Lifecycle state — all millis()-based, no tickers.
static bool mqttInitialised = false;
static bool mqttStoppedForOta = false;
static unsigned long mqttReconnectAtMs = 0;        //0 = no reconnect scheduled
static unsigned long mqttReconnectBackoffMs = 2000; //doubles per failure, 30 s cap
static unsigned long mqttNextTelemetryMs = 0;

//LWIP-context -> main-loop hand-off flags.
static volatile bool mqttJustConnected = false;
static volatile bool mqttMessagePending = false;
static char mqttRxBuffer[MQTT_MAX_TEXT_LEN + 1];

//Show-then-revert notification state (MqttHelpers.h).
static MqttNotification mqttNotification;

//Topics, resolved once in initMqtt(). AsyncMqttClient stores POINTERS for
//client id / credentials / will — these Strings must outlive the client,
//which is why they are globals and never reassigned after init.
static String mqttResolvedDeviceId;
static String mqttTopicSet;
static String mqttTopicAvailability;
static String mqttTopicTelemetry;

//LWIP context: flag only. Subscribe/discovery/availability happen in
//loopMqtt() so the 512-byte discovery buffers never live on the sys stack.
static void onMqttConnect(bool sessionPresent) {
  (void)sessionPresent;
  mqttJustConnected = true;
}

//LWIP context: schedule a retry; never call connect() from here.
static void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  (void)reason;
  mqttReconnectAtMs = millis() + mqttReconnectBackoffMs;
  mqttReconnectBackoffMs *= 2;
  if (mqttReconnectBackoffMs > 30000UL) mqttReconnectBackoffMs = 30000UL;
}

//LWIP context: copy the chunk into the fixed buffer (truncating past
//MQTT_MAX_TEXT_LEN — the display path truncates to the display width
//anyway) and flag the loop when the final chunk lands. Only one topic is
//subscribed, so no topic dispatch is needed.
static void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties,
                          size_t len, size_t index, size_t total) {
  (void)topic; (void)properties;
  for (size_t i = 0; i < len; i++) {
    size_t pos = index + i;
    if (pos >= MQTT_MAX_TEXT_LEN) break;
    mqttRxBuffer[pos] = payload[i];
  }
  if (index + len >= total) {
    size_t end = total > MQTT_MAX_TEXT_LEN ? MQTT_MAX_TEXT_LEN : total;
    mqttRxBuffer[end] = '\0';
    mqttMessagePending = true;
  }
}

//Publishes the four retained HA discovery configs. Main loop only — the
//buffers are stack-allocated here on the 4 KB cont stack.
static void publishMqttDiscovery() {
  char topicBuf[64];
  char payloadBuf[512];
  for (int entity = 0; entity < DISCOVERY_ENTITY_COUNT; entity++) {
    buildDiscoveryTopic(topicBuf, sizeof(topicBuf), entity, mqttResolvedDeviceId.c_str());
    buildDiscoveryPayload(payloadBuf, sizeof(payloadBuf), entity, mqttResolvedDeviceId.c_str(), espVersion);
    mqttClient.publish(topicBuf, 0, true, payloadBuf);
  }
}

//Called once from setup(), normal boots only (never quiet-OTA or recovery —
//setup() returns before reaching the call in those modes).
void initMqtt() {
  if (mqttBrokerHost[0] == '\0') {
    SerialPrintln(F("MQTT: no broker configured (MqttCredentials.h) — MQTT disabled"));
    return;
  }
  mqttResolvedDeviceId  = (mqttDeviceId[0] != '\0') ? String(mqttDeviceId) : String(mdnsName);
  mqttTopicSet          = mqttTopic(mqttResolvedDeviceId, "text/set");
  mqttTopicAvailability = mqttTopic(mqttResolvedDeviceId, "availability");
  mqttTopicTelemetry    = mqttTopic(mqttResolvedDeviceId, "telemetry");

  mqttClient.setServer(mqttBrokerHost, mqttBrokerPort);
  if (mqttUsername[0] != '\0') {
    mqttClient.setCredentials(mqttUsername, mqttPassword);
  }
  mqttClient.setClientId(mqttResolvedDeviceId.c_str());
  mqttClient.setKeepAlive(30);
  //Last Will: broker publishes retained "offline" if we vanish uncleanly.
  mqttClient.setWill(mqttTopicAvailability.c_str(), 1, true, "offline");
  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onMessage(onMqttMessage);

  mqttInitialised = true;
  mqttReconnectAtMs = millis();  //first connect attempt on the next loop pass
  SerialPrint(F("MQTT: initialised, device id: "));
  SerialPrintln(mqttResolvedDeviceId);
}

//Main-loop pump: reconnect schedule, post-connect setup, inbound messages,
//periodic telemetry. Broker down = silent retry with backoff; the display
//stays fully functional throughout.
void loopMqtt() {
  if (!mqttInitialised || mqttStoppedForOta) {
    return;
  }

  //Event-driven reconnect (design doc: 2 s -> 30 s exponential backoff),
  //only while WiFi is up. millis() comparison is wraparound-safe.
  if (!mqttClient.connected() && mqttReconnectAtMs != 0 &&
      (long)(millis() - mqttReconnectAtMs) >= 0 &&
      WiFi.status() == WL_CONNECTED) {
    mqttReconnectAtMs = 0;
    SerialPrintln(F("MQTT: connecting to broker..."));
    mqttClient.connect();
  }

  if (mqttJustConnected) {
    mqttJustConnected = false;
    mqttReconnectBackoffMs = 2000;  //healthy connection resets the backoff
    SerialPrintln(F("MQTT: connected"));
    mqttClient.subscribe(mqttTopicSet.c_str(), 0);
    mqttClient.publish(mqttTopicAvailability.c_str(), 1, true, "online");
    publishMqttDiscovery();
    mqttNextTelemetryMs = millis();  //first telemetry immediately
  }

  if (mqttMessagePending) {
    mqttMessagePending = false;
    String payload(mqttRxBuffer);
    String text;
    long dwellSeconds = MQTT_TEXT_DWELL_S;
    if (!parseMqttTextPayload(payload, text, dwellSeconds)) {
      //Malformed/plain payload → show it verbatim with the default dwell.
      text = payload;
      dwellSeconds = MQTT_TEXT_DWELL_S;
    }
    SerialPrint(F("MQTT: notification (dwell "));
    SerialPrint(dwellSeconds);
    SerialPrint(F(" s): "));
    SerialPrintln(text);
    notificationStart(mqttNotification, text, dwellSeconds, millis());
  }

  if (mqttClient.connected() && (long)(millis() - mqttNextTelemetryMs) >= 0) {
    mqttNextTelemetryMs = millis() + MQTT_TELEMETRY_INTERVAL_S * 1000UL;
    char buf[96];
    buildTelemetryPayload(buf, sizeof(buf), ESP.getFreeHeap(), WiFi.RSSI(), lastShowUnitWriteErrors);
    mqttClient.publish(mqttTopicTelemetry.c_str(), 0, false, buf);
  }
}

//Called from loop()'s once-per-second mode selection. Returns true while an
//MQTT notification owns the display (and pushes its text through the normal
//showText path — same cleaning/width handling as web-UI input). On expiry
//returns false; the caller's normal mode selection re-shows the previous
//clock/text content via showText's lastWrittenText comparison.
bool mqttNotificationTick() {
  if (!notificationTick(mqttNotification, millis())) {
    return false;
  }
  showText(mqttNotification.text);
  return true;
}

//Upload started (#116 freeze): publish a retained "offline" (a clean
//DISCONNECT discards the will, so HA would otherwise show a stale "online"
//through the whole flash) and drop the TCP connection so WiFi bandwidth
//and heap belong to the upload. Runs in async context — publish/disconnect
//only enqueue, which is safe there.
void mqttStopForOta() {
  if (!mqttInitialised || mqttStoppedForOta) {
    return;
  }
  mqttStoppedForOta = true;
  if (mqttClient.connected()) {
    mqttClient.publish(mqttTopicAvailability.c_str(), 1, true, "offline");
    mqttClient.disconnect();
  }
  SerialPrintln(F("MQTT: stopped for master OTA upload"));
}

//Upload failed or stalled (display thawed): resume MQTT. On the success
//path the device reboots instead, so this never runs there.
void mqttResumeAfterOta() {
  if (!mqttInitialised || !mqttStoppedForOta) {
    return;
  }
  mqttStoppedForOta = false;
  mqttReconnectBackoffMs = 2000;
  mqttReconnectAtMs = millis();
  SerialPrintln(F("MQTT: resuming after OTA upload ended without reboot"));
}

#else  //MQTT_ENABLE == false — no-op stubs so call sites stay guard-free.

void initMqtt() {}
void loopMqtt() {}
bool mqttNotificationTick() { return false; }
void mqttStopForOta() {}
void mqttResumeAfterOta() {}

#endif
```

- [ ] **Step 3: Wire up `setup()`.** In `ESPMaster.ino`, inside the `if (isWifiConfigured && !isPendingReboot)` block, add as the LAST statement of that block (after the web server setup/`begin`):

```cpp
    //MQTT / Home Assistant integration (#121). Normal boots only — the
    //quiet-OTA and recovery paths returned out of setup() long before here.
    initMqtt();
```

- [ ] **Step 4: Wire up `loop()`.** Two edits in `ESPMaster.ino`:

(a) Directly after the `if (firmwareFlashInProgress) { delay(1); return; }` block:

```cpp
  //MQTT pump (#121): reconnect schedule, inbound notifications, telemetry.
  //No-op when MQTT_ENABLE is false or the broker is unconfigured.
  loopMqtt();
```

(b) Replace the mode selection inside the once-per-second block:

```cpp
    //Mode Selection
    if (deviceMode == DEVICE_MODE_TEXT) {
      showText(inputText);
    }
    else if (deviceMode == DEVICE_MODE_CLOCK) {
      showText(formatDateTime(clockFormat));
    }
```

with:

```cpp
    //Mode Selection. An active MQTT notification (#121) temporarily owns
    //the display; when it expires the normal mode content re-flaps via
    //showText's lastWrittenText comparison (show-then-revert).
    if (!mqttNotificationTick()) {
      if (deviceMode == DEVICE_MODE_TEXT) {
        showText(inputText);
      }
      else if (deviceMode == DEVICE_MODE_CLOCK) {
        showText(formatDateTime(clockFormat));
      }
    }
```

- [ ] **Step 5: Wire up the OTA hooks.** Four edits in `ESPMaster.ino` (all inside `registerMasterFirmwareEndpoint()` and `loop()`; MQTT never initializes in recovery/OTA modes so the shared endpoint is safe):

(a) In the upload handler's `if (index == 0) {` block, directly after `masterOtaUploadActive = true;`:

```cpp
        mqttStopForOta();  //publish retained offline + disconnect (#121)
```

(b–d) At each of the three failure paths in the completion handler that currently do `masterOtaUploadActive = false;  //unfreeze the display (#116)` (the `otaRejected`, `Update.hasError()`, and `!Update.isFinished()` branches), add directly after that line:

```cpp
        mqttResumeAfterOta();
```

(e) In `loop()`'s stall-thaw branch (`SerialPrintln(F("OTA upload stalled >30 s — resuming normal operation")); masterOtaUploadActive = false;`), add directly after:

```cpp
      mqttResumeAfterOta();
```

- [ ] **Step 6: Build both envs + full native suite**

Run: `pio run -e espmaster && pio run -e espmaster_mqtt && pio test -e native`
Expected: both builds SUCCESS, all tests PASS. Compare `pio run -e espmaster` Flash/RAM numbers against the Task 4 baseline — the default env must be unchanged within linker noise (< ~100 bytes drift).

- [ ] **Step 7: Commit**

```bash
git add firmware/v1/ESPMaster/ServiceMqttFunctions.ino firmware/v1/ESPMaster/ESPMaster.h firmware/v1/ESPMaster/ESPMaster.ino
git commit -m "feat(firmware/v1/ESPMaster): MQTT client lifecycle, HA discovery, show-then-revert notifications (#121)"
```

---

### Task 7: CI build coverage + docs sync

**Files:**
- Modify: `.github/workflows/build.yml`
- Modify: `CLAUDE.md`

- [ ] **Step 1: Add the MQTT env to CI.** In `.github/workflows/build.yml`, in the `build` job after the existing `Build` step:

```yaml
      - name: Build ESPMaster (MQTT enabled)
        if: matrix.project == 'ESPMaster'
        working-directory: firmware/v1/${{ matrix.project }}
        run: pio run -e espmaster_mqtt
```

- [ ] **Step 2: Sync `CLAUDE.md`** (three small edits):
  - In the ESPMaster bullet under **Architecture**, add `ServiceMqttFunctions` to the sibling-`.ino` list and `MqttHelpers.h` to the header-only helpers list (one line: "pure MQTT payload/discovery logic, natively tested").
  - Under **Configuration Knobs Worth Knowing**, add: `- \`MQTT_ENABLE\` — MQTT / Home Assistant integration (#121). Default \`false\` (fully compiled out). Build the \`espmaster_mqtt\` env and copy \`MqttCredentials.h.example\` → \`MqttCredentials.h\` (gitignored) to enable. Notification-only semantics: HA text shows for a dwell (default 60 s, clamp 5–3600), then reverts; health telemetry every 60 s; zero EEPROM/RTC interaction.`
  - Under **Build / Flash / Test Workflow**, note the second env: `pio run -e espmaster_mqtt  # MQTT-enabled variant (#121)`.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/build.yml CLAUDE.md
git commit -m "ci+docs: build espmaster_mqtt env in CI, document MQTT integration (#121)"
```

---

### Task 8: Measured gates + hardware acceptance

No new code — this is the spec's mandatory verification. Steps 1–3 are automated; step 4 needs the physical display + a LAN broker and is performed by Lucas.

- [ ] **Step 1: Full automated suite**

```bash
pio test -e native && python -m pytest tests/ && pio run -e espmaster && pio run -e espmaster_mqtt
```
Expected: everything green.

- [ ] **Step 2: OTA staging gate.** From the `espmaster_mqtt` build output, take the sketch size (`Flash: used N bytes`). The 1 MB layout must still hold sketch + OTA staging: verify `N < (1044464 - N)` region headroom, i.e. the build's reported free space exceeds the sketch size (the same invariant `/firmware/master` checks via `ESP.getFreeSketchSpace()`). Also confirm the MQTT variant's flash delta vs the default env is within the spec's ~10–12 KB estimate; investigate anything wildly above.

- [ ] **Step 3: Heap gate.** Flash the `espmaster_mqtt` build to the 5-unit display via `flashing/ota-master.sh .pio/build/espmaster_mqtt/firmware.bin http://<display-ip>`. After boot, read free heap from the first MQTT telemetry packet (`mosquitto_sub -h <broker> -t 'splitflap/+/telemetry'`) and compare against the ~42 KB at-rest baseline. Regression must be small (~1 KB expected) and understood.

- [ ] **Step 4: Hardware acceptance checklist** (live HA + mosquitto on LAN):
  - Discovery: one device with Text entity + 3 sensors auto-appears in HA.
  - HA Text entity send → display shows it, reverts after 60 s (clock resumes ticking / previous text returns).
  - JSON payload `{"text":"TEST","dwell":10}` via `mosquitto_pub` → 10 s dwell.
  - Second message while first is showing → replaces it, deadline resets.
  - Stop mosquitto → display keeps working, silent reconnect when broker returns.
  - Pull display power → HA shows the device unavailable (LWT).
  - Telemetry sensors update every 60 s.
  - **OTA acceptance (non-negotiable):** a full web-OTA flash of the MQTT-on build returns SUCCESS via `flashing/ota-master.sh`, **twice in a row**.

- [ ] **Step 5: Close out** — push the branch, close #121 with a comment linking the acceptance results, update project memory (per-change workflow steps 8–9).

---

## Self-Review (performed at plan time)

- **Spec coverage:** inbound text ✓ (T1/T6), dwell + clamp ✓ (T1/T2), health telemetry ✓ (T3/T5/T6), availability + LWT ✓ (T6), discovery ✓ (T3/T6), MQTT_ENABLE-off = zero cost ✓ (T4/T6 stubs + gc-sections + size check), pinned lib ✓ (T4), credentials header ✓ (T4), no ArduinoJson ✓, LWIP copy-only ✓ (T6), OTA protection ✓ (T6 step 5), no EEPROM/RTC ✓ (no file touches), input cleaning via existing showText path ✓, size/heap gates ✓ (T8), native + hardware tests ✓ (T1–T3, T8).
- **Documented deviations from spec wording:** (1) no saved mode/text for revert — the declarative loop makes it unnecessary (T2 note); (2) discovery uses HA short-name keys to fit the 512-byte buffer; (3) telemetry `unitErrors` is defined concretely as Wire-level write failures from the last show pass (T5) — the spec's "units whose last I2C transaction failed" had no existing counter to reuse.
- **Type consistency:** `parseMqttTextPayload(const String&, String&, long&)` used identically in T1 tests and T6; `notificationStart/notificationTick` signatures match T2↔T6; `buildDiscoveryTopic/Payload(char*, size_t, int, const char*[, const char*])` match T3↔T6; `writeToUnit` returns `int` in T5 and T6 doesn't call it; `lastShowUnitWriteErrors` extern'd in T5, consumed in T6.
