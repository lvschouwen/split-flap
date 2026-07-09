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

// Inbound MQTT message as copied out of LWIP context (v1's copy+flag rule,
// now structural): the future MQTT slice's callbacks post here and return;
// only mqttTask does real work. POD, same reasoning as DisplayCommand.
//
// Sizing: subscribed topics are "<deviceName>/..." — deviceName caps at 24
// (DeviceIdentity) and v1's longest command suffix is well under the
// remaining 23. Payloads: inbound is HA text/mode/dwell commands — text is
// display-width bounded, modes/dwells are tokens; 200 dwarfs all of them
// (outbound discovery JSON is bigger but never enters this inbox).
// PRODUCER CONTRACT (MQTT slice): bound-check/truncate BEFORE memcpy into
// these buffers, and keep them NUL-terminated — mqttInboxPost() copies raw.
struct MqttInboxMessage {
  char topic[48] = {0};
  char payload[200] = {0};
};

// Creates the queues, the snapshot mutex, and all four tasks. Call once
// from setup(), after webEndpointsInit() (netTask drains the web staging
// and needs its mutex to exist).
void tasksInit(MasterSettings& settings, SettingsStore& store);

// Non-blocking enqueue into the display task; false = queue full (callers
// report, never wait — network context must not block on the display).
bool displayEnqueue(const DisplayCommand& cmd);
bool displayQueueFull();

// Mutex-guarded copy of the display task's published state. Safe from any
// task; consumers render from the copy, never from live state.
DisplaySnapshot displaySnapshotGet();

// Posts one inbound MQTT message; false = inbox full. Safe from LWIP
// callbacks (non-blocking).
bool mqttInboxPost(const MqttInboxMessage& msg);

// One heartbeat line: free heap + per-task stack high-water marks —
// empirical validation of the stack sizing table on real hardware.
void tasksHeartbeatReport();
