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
  // Calibration + provisioning (#204, slice B). unitAddress targets the op;
  // value is the opcode-disambiguated payload (offset steps / jog steps /
  // new address). Reflash arrives with slice C (#205).
  WriteOffset,
  Jog,
  Home,
  RebootToBootloader,
  Identify,
  SetAddress,
  ClearAddress,
  ResetOdometer,
  // Feature gates (#409): persist the unit's UNIT_GATE_* byte. `value`
  // carries the new byte; the exec verifies it by reading GET_LIFETIME back.
  SetGates,
  // On-demand unit self-test (#265): displayTask starts the unit's
  // diagnostic revolution and polls the result; measurements publish into
  // the snapshot's SelfTestSlot.
  SelfTest,
  ResetUnits,
  Stop,
  // Bulk unit reflash over twiboot (#205, slice C) — a long-running job
  // displayTask executes inline; progress rides the snapshot.
  ReflashUnits,
};

enum class DisplayAlignment : uint8_t { Left = 0, Center, Right };

// What put the current text on the wall (#403). The wall has five producers
// and no record of which one drove it: ask the display what it shows and it
// answers with the text, ask why and nothing in the system knows.
//
// This names the ACTOR, not the mechanism — a #219 show-then-revert transient
// from a browser is Web, an MQTT notification is Mqtt. The distinction that
// matters most to an owner is Leader vs the rest: on a follower, "I put this
// here" and "the leader put this here" are otherwise indistinguishable.
//
// uint8_t keeps the queue slot a fixed-size POD. No history, no audit log:
// one field for the CURRENT text, plus the millis() it landed.
enum class DisplaySource : uint8_t {
  Unknown = 0,  // nothing has driven the display since boot, or it is blank
  Web,          // a browser talking to this box
  Mqtt,         // MQTT / Home Assistant
  Clock,        // this box's own clock ticker
  Leader,       // the cluster leader handed this row its content
};

// Wire vocabulary for /settings and /events. The console names the producer
// from these strings — renaming one is a breaking UI change.
inline const char* displaySourceName(DisplaySource s) {
  switch (s) {
    case DisplaySource::Web:    return "web";
    case DisplaySource::Mqtt:   return "mqtt";
    case DisplaySource::Clock:  return "clock";
    case DisplaySource::Leader: return "leader";
    default:                    return "none";
  }
}

// The display can never show more than the hardware ceiling; longer text is
// truncated at build time so the queue slot stays fixed-size.
#define DISPLAY_CMD_TEXT_LEN UNITS_AMOUNT

// The display-domain projection of a text: exactly what makeShowTextCommand
// puts on the wire. Every String that enters ClockPolicy's dedup
// comparisons (retained inputText, lastQueued) MUST pass through this, or a
// longer-than-display message can never equal any snapshot text and the
// ticker's in-flight marker sticks forever (#192 review H1).
inline String truncateForDisplay(const String& text) {
  if (text.length() <= DISPLAY_CMD_TEXT_LEN) return text;
  return text.substring(0, DISPLAY_CMD_TEXT_LEN);
}

struct DisplayCommand {
  DisplayOpcode opcode = DisplayOpcode::None;
  DisplayAlignment alignment = DisplayAlignment::Left;
  uint8_t speed = 0;  // web-scale 1..100 (v1 slider contract)
  // Maintenance fields (#204): seq correlates the queued op with the
  // snapshot's MaintResult (the /unit/op-result contract); unitAddress is
  // the wire target; value carries offset steps, jog steps, or the new
  // address — the opcode disambiguates. Built only through the makers.
  uint32_t seq = 0;
  uint8_t unitAddress = 0;
  int16_t value = 0;
  // Who is driving (#403). Meaningful on ShowText; the re-show opcodes below
  // deliberately leave it Unknown because they restore text the snapshot
  // already attributes — see makeResetUnitsCommand.
  DisplaySource source = DisplaySource::Unknown;
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
    case DisplayOpcode::ShowText:           return "ShowText";
    case DisplayOpcode::Probe:              return "Probe";
    case DisplayOpcode::WriteOffset:        return "WriteOffset";
    case DisplayOpcode::Jog:                return "Jog";
    case DisplayOpcode::Home:               return "Home";
    case DisplayOpcode::RebootToBootloader: return "RebootToBootloader";
    case DisplayOpcode::Identify:           return "Identify";
    case DisplayOpcode::SetAddress:         return "SetAddress";
    case DisplayOpcode::ClearAddress:       return "ClearAddress";
    case DisplayOpcode::ResetOdometer:      return "ResetOdometer";
    case DisplayOpcode::SetGates:           return "SetGates";
    case DisplayOpcode::SelfTest:           return "SelfTest";
    case DisplayOpcode::ResetUnits:         return "ResetUnits";
    case DisplayOpcode::Stop:               return "Stop";
    case DisplayOpcode::ReflashUnits:       return "ReflashUnits";
    default:                                return "None";
  }
}

// `source` has no default on purpose (#403): a new producer must state who it
// is rather than inherit an anonymous one. An unattributed wall is the bug
// this field exists to kill.
inline DisplayCommand makeShowTextCommand(const String& text,
                                          const String& alignment, int speed,
                                          DisplaySource source) {
  DisplayCommand cmd;
  cmd.opcode = DisplayOpcode::ShowText;
  cmd.source = source;
  cmd.alignment = displayAlignmentFromString(alignment);
  cmd.speed = (uint8_t)(speed < 1 ? 1 : (speed > 100 ? 100 : speed));

  String cut = truncateForDisplay(text);
  memcpy(cmd.text, cut.c_str(), cut.length());
  cmd.text[cut.length()] = '\0';
  return cmd;
}

inline DisplayCommand makeProbeCommand() {
  DisplayCommand cmd;
  cmd.opcode = DisplayOpcode::Probe;
  return cmd;
}

// --- maintenance makers (#204) — the only constructors for these ops ---------

inline DisplayCommand makeMaintCommand(DisplayOpcode op, uint32_t seq,
                                       uint8_t unitAddress, int16_t value) {
  DisplayCommand cmd;
  cmd.opcode = op;
  cmd.seq = seq;
  cmd.unitAddress = unitAddress;
  cmd.value = value;
  return cmd;
}

inline DisplayCommand makeWriteOffsetCommand(uint32_t seq, uint8_t addr,
                                             int16_t offsetSteps) {
  return makeMaintCommand(DisplayOpcode::WriteOffset, seq, addr, offsetSteps);
}

// Jog steps ride the wire as one signed byte; clamp like v1's jogUnit().
inline DisplayCommand makeJogCommand(uint32_t seq, uint8_t addr, int steps) {
  if (steps > 127) steps = 127;
  if (steps < -127) steps = -127;
  return makeMaintCommand(DisplayOpcode::Jog, seq, addr, (int16_t)steps);
}

inline DisplayCommand makeHomeCommand(uint32_t seq, uint8_t addr) {
  return makeMaintCommand(DisplayOpcode::Home, seq, addr, 0);
}

inline DisplayCommand makeRebootToBootloaderCommand(uint32_t seq,
                                                    uint8_t addr) {
  return makeMaintCommand(DisplayOpcode::RebootToBootloader, seq, addr, 0);
}

inline DisplayCommand makeIdentifyCommand(uint32_t seq, uint8_t addr) {
  return makeMaintCommand(DisplayOpcode::Identify, seq, addr, 0);
}

inline DisplayCommand makeSetAddressCommand(uint32_t seq, uint8_t addr,
                                            uint8_t newAddress) {
  return makeMaintCommand(DisplayOpcode::SetAddress, seq, addr,
                          (int16_t)newAddress);
}

inline DisplayCommand makeClearAddressCommand(uint32_t seq, uint8_t addr) {
  return makeMaintCommand(DisplayOpcode::ClearAddress, seq, addr, 0);
}

// Physical-rebuild bookkeeping only (#231): zeroes the unit's revolution
// odometer + its EEPROM ring after a flap swap or motor replacement.
inline DisplayCommand makeResetOdometerCommand(uint32_t seq, uint8_t addr) {
  return makeMaintCommand(DisplayOpcode::ResetOdometer, seq, addr, 0);
}

// Feature gates (#409): the write half of #406's gate byte, which is what
// lets #407's motion changes ship dormant and be switched on over the wire
// instead of costing a second fleet reflash.
inline DisplayCommand makeSetGatesCommand(uint32_t seq, uint8_t addr,
                                          uint8_t gates) {
  return makeMaintCommand(DisplayOpcode::SetGates, seq, addr, (int16_t)gates);
}

// On-demand diagnostic revolution (#265): ~15 s of unit motion displayTask
// waits out inline (commands queue behind it, like every long op).
inline DisplayCommand makeSelfTestCommand(uint32_t seq, uint8_t addr) {
  return makeMaintCommand(DisplayOpcode::SelfTest, seq, addr, 0);
}

// The re-show text/alignment/speed are baked at enqueue time (senders bake
// params): a ShowText queued between ResetUnits and its execution can't
// leak into the post-reset display, and the blank-out + re-show frames
// honor the settings of that moment.
inline DisplayCommand makeResetUnitsCommand(uint32_t seq,
                                            const String& currentText,
                                            const String& alignment,
                                            int speed) {
  // Unknown source is deliberate (#403): this re-shows text the snapshot
  // already attributes, so displayApplyCommand leaves both the text and the
  // source alone. Minting a source here would let a maintenance op claim
  // authorship of a message the leader or a browser put on the wall.
  DisplayCommand cmd =
      makeShowTextCommand(currentText, alignment, speed, DisplaySource::Unknown);
  cmd.opcode = DisplayOpcode::ResetUnits;
  cmd.seq = seq;
  return cmd;
}

inline DisplayCommand makeStopCommand(uint32_t seq) {
  return makeMaintCommand(DisplayOpcode::Stop, seq, 0, 0);
}

// Same bake-at-enqueue rule as ResetUnits (#205): the reflash job's
// end-of-run re-show uses the content of the moment the operator clicked —
// reflashed units home to blank, so the job puts the display back itself.
// `addr` 0 = the whole fleet (the historical behaviour); 1..126 targets one
// unit (#412). 0 is the general-call address and never a unit's, so it is a
// free sentinel — no extra field needed on the queue-copied POD.
inline DisplayCommand makeReflashUnitsCommand(uint32_t seq,
                                              const String& currentText,
                                              const String& alignment,
                                              int speed, uint8_t addr) {
  // Same Unknown-source rule as ResetUnits (#403): the end-of-run re-show
  // restores attributed text, it does not author it.
  DisplayCommand cmd =
      makeShowTextCommand(currentText, alignment, speed, DisplaySource::Unknown);
  cmd.opcode = DisplayOpcode::ReflashUnits;
  cmd.seq = seq;
  cmd.unitAddress = addr;
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
  } else if (cmd.unitAddress != 0 || cmd.seq != 0) {
    // Maintenance ops: correlation seq + wire target + payload.
    line += " seq=";
    line += (unsigned long)cmd.seq;
    if (cmd.unitAddress != 0) {
      line += " addr=";
      line += (int)cmd.unitAddress;
    }
    if (cmd.value != 0) {
      line += " value=";
      line += (int)cmd.value;
    }
  }
  return line;
}
