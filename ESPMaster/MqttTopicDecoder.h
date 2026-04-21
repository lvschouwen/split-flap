#pragma once

// Pure-logic MQTT cmd/* topic parser + payload validators. Lives in a
// header so the host-side test env (ArduinoFake + Unity) can exercise it
// without pulling AsyncMqttClient. Issue #50, task #11.
//
// The parser is intentionally conservative: any malformed topic path or
// out-of-range unit address decodes to `NONE`, and the dispatcher treats
// NONE as "silently ignore". That way a buggy HA automation or a stray
// mosquitto_pub can't reach an internal code path we didn't intend to
// expose.

#include <Arduino.h>

#ifndef MQTT_MESSAGE_MAX_LEN
// Matches the web UI's <input maxlength="75"> so MQTT and HTTP can't
// disagree about how long a displayable message can be. The rest of the
// display pipeline (processSentenceToLines -> showMessage) tolerates
// longer input, but "tolerates" isn't the same as "should accept" —
// cap at the boundary that's been tested since #32-era calibration work.
#define MQTT_MESSAGE_MAX_LEN 75
#endif

struct MqttCmdDecoded {
  enum Kind : uint8_t {
    NONE = 0,
    STOP,            // cmd/stop
    HOME_ALL,        // cmd/home_all
    MESSAGE,         // cmd/message
    UNIT_HOME,       // cmd/unit/<N>/home
    UNIT_REBOOT      // cmd/unit/<N>/reboot
    // Note: cmd/unit/<N>/bootloader is deliberately NOT represented. It's
    // destructive (puts the unit into twiboot mode) and is only legitimate
    // inside the master-orchestrated reflash-all flow, not as a standalone
    // MQTT command.
  };
  Kind kind;
  int  unitAddress;  // valid only for UNIT_HOME / UNIT_REBOOT; I2C range 1..127
};

// Validate a cmd/message payload before it's copied into the display
// pipeline. Returns true iff the payload is safe to apply.
//
// Rules:
//   - empty payload is allowed (equivalent to clearing the message)
//   - length must not exceed MQTT_MESSAGE_MAX_LEN
//   - reject C0 control characters (< 0x20) except horizontal tab
//   - reject DEL (0x7F)
//   - allow extended ASCII (0x80+) — the letters[] lookup treats unknown
//     glyphs as blanks, so they can't hurt the display, and tightening
//     further would break the $/&/# wire encoding the web UI ships for
//     ä/ö/ü.
inline bool mqttValidateMessagePayload(const char* data, size_t len) {
  if (len == 0) return true;
  if (len > MQTT_MESSAGE_MAX_LEN) return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)data[i];
    if (c < 0x20 && c != '\t') return false;
    if (c == 0x7F) return false;
  }
  return true;
}

// Parse an incoming cmd topic into a decoded action. `topic` is the full
// topic string the broker delivered; `cmdPrefix` is the cached
// "<base>/cmd/" prefix built from mdnsName (caller supplies both so the
// pure function stays free of global state — easier to test).
//
// Returns NONE for anything that doesn't match an allowed action.
inline MqttCmdDecoded mqttDecodeCmdTopic(const String& topic, const String& cmdPrefix) {
  MqttCmdDecoded r;
  r.kind = MqttCmdDecoded::NONE;
  r.unitAddress = 0;

  if (cmdPrefix.length() == 0) return r;
  if (!topic.startsWith(cmdPrefix)) return r;

  String suffix = topic.substring(cmdPrefix.length());

  if (suffix == "stop")     { r.kind = MqttCmdDecoded::STOP;     return r; }
  if (suffix == "home_all") { r.kind = MqttCmdDecoded::HOME_ALL; return r; }
  if (suffix == "message")  { r.kind = MqttCmdDecoded::MESSAGE;  return r; }

  //Per-unit: "unit/<N>/<action>". Reject anything that doesn't parse
  //cleanly as two slash-separated segments with <N> being a positive
  //decimal integer in the I2C address range.
  const char* kUnitPrefix = "unit/";
  const size_t kUnitPrefixLen = 5;
  if (suffix.length() > kUnitPrefixLen && suffix.startsWith(kUnitPrefix)) {
    int firstSlash = suffix.indexOf('/', (int)kUnitPrefixLen);
    if (firstSlash < 0) return r;  //no action segment

    String addrPart = suffix.substring(kUnitPrefixLen, firstSlash);
    String action   = suffix.substring(firstSlash + 1);

    if (addrPart.length() == 0 || action.length() == 0) return r;

    //Strict decimal-only to avoid String::toInt()'s tolerance for leading
    //whitespace and trailing garbage ("  5abc" -> 5 via toInt()).
    for (size_t i = 0; i < addrPart.length(); i++) {
      char c = addrPart[i];
      if (c < '0' || c > '9') return r;
    }

    long addr = addrPart.toInt();
    if (addr < 1 || addr > 127) return r;  //valid I2C 7-bit range

    if (action == "home")   { r.kind = MqttCmdDecoded::UNIT_HOME;   r.unitAddress = (int)addr; return r; }
    if (action == "reboot") { r.kind = MqttCmdDecoded::UNIT_REBOOT; r.unitAddress = (int)addr; return r; }
    //Any other action (including "bootloader") intentionally falls through
    //to NONE — see the enum comment above.
  }

  return r;
}
