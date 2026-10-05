#pragma once
// NetLiveness — the probes behind NetLivenessPolicy.h (#501) and the state a
// reboot decision needs across a restart. The policy step itself runs in
// WifiService's tick (netTask), next to the link watchdog it complements and
// the restart it shares.
#include <Arduino.h>

#include "NetLivenessPolicy.h"

// clusterTask, every pass: runs the two probes when one is due (self-throttled
// to NET_LIVENESS_PROBE_INTERVAL_MS; each step of a probe waits at most
// 1.5 s). The one task that already makes blocking outbound calls.
void netLivenessProbeTick();

// netTask: the latest results, Unknown when stale or never taken.
NetProbe netLivenessGateway(uint32_t nowMs);
NetProbe netLivenessSelf(uint32_t nowMs);

// Liveness reboots in a row, kept across a software restart (RTC memory; a
// power cycle starts at 0).
uint8_t netLivenessStrikes();
void netLivenessAddStrike();
void netLivenessClearStrikes();

// /settings "netLiveness".
String netLivenessJson();
