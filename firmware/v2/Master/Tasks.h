#pragma once
// Tasks.h — the v2 master's FreeRTOS task skeleton (#187).
//
// Full decomposition per the 2026-07-09 spec: every domain task exists from
// day one, stub-bodied until its slice arrives, so later slices land into a
// pre-built home instead of accreting into loop().
//
//   core 1 (display domain)   displayTask  prio 3  exclusive I2C owner
//                             clockTask    prio 1  1 Hz mode ticker
//   core 0 (network domain)   netTask      prio 1  web/settings housekeeping
//                             mqttTask     prio 1  MQTT client lifecycle
//
// WiFi/LWIP are already pinned to core 0 by the framework; AsyncTCP joins
// them via -DCONFIG_ASYNC_TCP_RUNNING_CORE=0 (platformio.ini). Arduino's
// loopTask survives only as the observability heartbeat.
//
// Memory policy: all stacks and queues here are STATICALLY allocated in
// internal SRAM (an RTOS requirement for stacks and the deterministic
// choice for queues); KB-sized elastic buffers go through largeAlloc()
// instead. Cross-task traffic is queues + snapshot copies only — commands
// carry all their parameters, tasks never reach into shared settings.

#include "DisplayCommand.h"
#include "DisplayIpc.h"
#include "Settings.h"
#include "SettingsStore.h"

// Inbound MQTT message as copied out of callback context (v1's copy+flag
// rule, now structural): MqttService's onMessage posts here; only mqttTask
// does real work. POD, same reasoning as DisplayCommand.
//
// Sizing (static_asserted in MqttService.cpp): the longest command topic is
// "splitflap/<24-char name>/alignment/set" = 48 chars + NUL; text/set
// payloads run to MQTT_MAX_TEXT_LEN (256) + NUL — the v1 wire truncation
// point (the display path truncates to width anyway). Outbound discovery
// JSON is bigger but never enters this inbox.
// PRODUCER CONTRACT: bound-check/truncate BEFORE memcpy into these buffers,
// and keep them NUL-terminated — mqttInboxPost() copies raw.
struct MqttInboxMessage {
  char topic[64] = {0};
  char payload[260] = {0};
};

// Creates the queues, the snapshot mutex, and all four tasks. Call once
// from setup(), after webEndpointsInit() (netTask drains the web staging
// and needs its mutex to exist).
void tasksInit(MasterSettings& settings, SettingsStore& store);

// #289 dummy mode: push a changed unit-count override to displayTask (0 =
// auto). Takes effect at the next probe/health fold — the settings drain
// queues a Probe right after calling this.
void tasksSetUnitCountOverride(int count);

// #331 headless: push a changed deviceRole to displayTask. A headless role
// forces displayWidth 0 (no display, no phantom row) at the next fold — the
// settings drain queues a Probe right after calling this.
void tasksSetDeviceRole(const String& role);

// #412 boot auto-install brake, pushed by the settings drain on netTask. True
// (the default) keeps the historical behaviour: a master booting with
// off-bundle units converges them unattended. False skips it so an operator
// can drive the campaign one unit at a time and inspect each result.
void tasksSetReflashOnBoot(bool enabled);

// Non-blocking enqueue into the display task; false = queue full (callers
// report, never wait — network context must not block on the display).
bool displayEnqueue(const DisplayCommand& cmd);
bool displayQueueFull();

// Monotonic sequence for maintenance ops (#204): stamp into the command at
// enqueue, hand to the client, correlate via the snapshot's MaintResult
// (GET /unit/op-result). Starts at 1 — seq 0 means "nothing executed yet".
uint32_t displayNextMaintSeq();

// Mutex-guarded copy of the display task's published state. Safe from any
// task; consumers render from the copy, never from live state.
DisplaySnapshot displaySnapshotGet();

// Posts one inbound MQTT message; false = inbox full. Safe from LWIP
// callbacks (non-blocking).
bool mqttInboxPost(const MqttInboxMessage& msg);

// One heartbeat line: free heap + per-task stack high-water marks —
// empirical validation of the stack sizing table on real hardware.
void tasksHeartbeatReport();
