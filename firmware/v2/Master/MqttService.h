#pragma once

#include <Arduino.h>

#include "Settings.h"
#include "SettingsStore.h"
#include "Tasks.h"

// MQTT / Home Assistant service for the v2 master (#224) — the v1
// ServiceMqttFunctions.ino behavior (22 discovery entities, availability
// LWT, five command topics, show-then-revert notifications, 60 s telemetry)
// on espMqttClient, owned and ticked exclusively by mqttTask (core 0).
//
// Threading: mqttServiceInit() runs in setup() before tasksInit().
// mqttServiceTick()/mqttServiceHandleInbox() are mqttTask-only — the client
// runs without its internal task (UseInternalTask::NO), so every callback
// fires inside our loop() call in mqttTask context; inbound messages still
// route through the MqttInboxMessage queue (the structural copy+flag rule).
// Every cross-task entry point below stages an atomic flag that the tick
// consumes; none of them touch client state directly.

// Copies the broker identity into stable service-local Strings (the client
// stores raw pointers; the settings Strings can be reassigned by a web POST
// mid-connection — v1 #57 rule). Empty host = MQTT disabled, everything
// inert. bootCount feeds the retained diag/boots sensor.
void mqttServiceInit(MasterSettings& settings, SettingsStore& store,
                     const String& effectiveDeviceName, uint32_t bootCount);

// mqttTask only: apply one inbound message (drained from the inbox queue).
void mqttServiceHandleInbox(const MqttInboxMessage& msg);

// mqttTask only: client pump + reconnect/backoff + on-connect sequence +
// state publishers + telemetry + notification dwell.
void mqttServiceTick();

// Any task: broker session state for GET /settings.
bool mqttIsConnected();

// Any task: true while an MQTT notification owns the display — clockTask's
// gate (v1 gated loop()'s mode block via mqttNotificationTick()).
bool mqttNotificationActive();

// Any task: cancel a running notification (explicit mode switch or message
// send trumps it, v1 #130 rule). Staged; the tick applies it.
void mqttCancelNotification();

// Any task: arm the show-then-revert overlay for web transient text (#219)
// the caller just queued to the display. The clockTask gate flips
// immediately in the caller's task; the dwell deadline is stamped by the
// next tick. dwellSeconds <= 0 -> the 600 s default. Works broker-less
// (v1 rule: calibration transients exist without a broker).
void mqttStartNotificationDwell(long dwellSeconds);

// Any task: force-close the session before OTA flash writes begin (#116
// freeze). Abrupt on purpose — the broker fires the retained "offline" LWT,
// keeping HA availability truthful without an in-flight publish.
void mqttStopForOta();

// Any task: resume after an upload that ended without a reboot.
void mqttResumeAfterOta();

// Any task: device renamed (#125) — while this run still IS the old MQTT
// identity, blank the old retained discovery configs + state topics so the
// post-reboot identity doesn't orphan a device in Home Assistant.
void mqttRequestDiscoveryClear();
