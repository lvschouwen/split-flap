#pragma once
// DisplayCommand.h — the network→display IPC command (#187), pure logic.
//
// Commands are PODs so a FreeRTOS queue can copy them by value: fixed char
// buffer, no String members. Senders bake in every parameter (speed,
// alignment) at build time — the display task never reaches across cores
// into shared settings. The queue itself lives in Tasks.cpp; this header
// stays FreeRTOS-free so the native test env needs no RTOS fake.

#include <Arduino.h>

enum class DisplayOpcode : uint8_t {
  None = 0,  // default-constructed command; the worker rejects it
  ShowText,
  Probe,
  // Calibration/reflash opcodes arrive with the I2C slice.
};

enum class DisplayAlignment : uint8_t { Left = 0, Center, Right };

// The display can never show more than the hardware ceiling; longer text is
// truncated at build time so the queue slot stays fixed-size.
#define DISPLAY_CMD_TEXT_LEN UNITS_AMOUNT

struct DisplayCommand {
  DisplayOpcode opcode = DisplayOpcode::None;
  DisplayAlignment alignment = DisplayAlignment::Left;
  uint8_t speed = 0;  // web-scale 1..100 (v1 slider contract)
  char text[DISPLAY_CMD_TEXT_LEN + 1] = {0};
};

// Alignment settings strings are validated at the web boundary
// (isValidAlignmentValue); anything else deliberately degrades to Left
// rather than rejecting — a command must always be executable.
inline DisplayAlignment displayAlignmentFromString(const String& v) {
  if (v == "center") return DisplayAlignment::Center;
  if (v == "right") return DisplayAlignment::Right;
  return DisplayAlignment::Left;
}

inline const char* displayAlignmentName(DisplayAlignment a) {
  switch (a) {
    case DisplayAlignment::Center: return "center";
    case DisplayAlignment::Right:  return "right";
    default:                       return "left";
  }
}

inline const char* displayOpcodeName(DisplayOpcode op) {
  switch (op) {
    case DisplayOpcode::ShowText: return "ShowText";
    case DisplayOpcode::Probe:    return "Probe";
    default:                      return "None";
  }
}

inline DisplayCommand makeShowTextCommand(const String& text,
                                          const String& alignment, int speed) {
  DisplayCommand cmd;
  cmd.opcode = DisplayOpcode::ShowText;
  cmd.alignment = displayAlignmentFromString(alignment);
  cmd.speed = (uint8_t)(speed < 1 ? 1 : (speed > 100 ? 100 : speed));

  size_t n = text.length();
  if (n > DISPLAY_CMD_TEXT_LEN) n = DISPLAY_CMD_TEXT_LEN;
  memcpy(cmd.text, text.c_str(), n);
  cmd.text[n] = '\0';
  return cmd;
}

inline DisplayCommand makeProbeCommand() {
  DisplayCommand cmd;
  cmd.opcode = DisplayOpcode::Probe;
  return cmd;
}

// One log line per executed command — the stub worker's visible output on
// the bare devkit's USB-CDC console.
inline String describeDisplayCommand(const DisplayCommand& cmd) {
  String line = displayOpcodeName(cmd.opcode);
  if (cmd.opcode == DisplayOpcode::ShowText) {
    line += " \"";
    line += cmd.text;
    line += "\" align=";
    line += displayAlignmentName(cmd.alignment);
    line += " speed=";
    line += (int)cmd.speed;
  }
  return line;
}
