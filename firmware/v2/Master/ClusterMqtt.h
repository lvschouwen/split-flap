#pragma once
// ClusterMqtt.h — pure HA/MQTT payload logic for the cluster leader's
// surfacing (#277, epic #270): the cluster-degraded binary sensor, its
// per-member availability + rollout attributes (the #276 rollout state's
// deferred HA exposure lands here), and the wall text/state. v2-only ON
// PURPOSE — MqttHelpers.h stays the v1-tracking copy, so cluster entities
// never join its discovery enum; MqttService.cpp publishes these beside
// the shared set. Natively tested by test_cluster_mqtt.
//
// Lifecycle rule (owned by MqttService.cpp): the discovery config is
// published only while leading and blanked on the leading→standalone
// transition, so standalone boards never grow a meaningless HA entity.

#include <Arduino.h>

#include "ClusterLeader.h"
#include "MqttHelpers.h"   // MQTT_FMT/mqttSnprintf + MQTT_DEVICE_BLOCK
#include "SettingsJson.h"  // appendJsonString

// ON = the wall needs attention: any member unjoined or degraded, a
// member the fleet rollout gave up on, or a running image that failed its
// verify pass (convergence is off until reboot). A rollout merely in
// progress is normal convergence, not a problem.
inline bool clusterDegraded(const ClusterLeaderStatus& st) {
  if (!st.enabled) return false;
  if (st.rolloutImageFailed) return true;
  for (int i = 0; i < st.memberCount; i++) {
    if (!st.members[i].joined || st.members[i].degraded ||
        st.members[i].updateBlocked) {
      return true;
    }
  }
  return false;
}

// json_attributes payload for the cluster_degraded sensor. String-built:
// hosts are user config and revs arrive in join reply bodies — both need
// real JSON escaping; size is bounded by CLUSTER_MAX_MEMBERS.
inline String buildClusterAttrsJson(const ClusterLeaderStatus& st) {
  String out;
  out.reserve(192 + st.memberCount * 130);
  out += "{\"rows\":";
  out += st.gridRows;
  out += ",\"capacity\":";
  out += st.gridCapacity;
  out += ",\"members\":[";
  for (int i = 0; i < st.memberCount; i++) {
    const ClusterLeaderMemberStatus& m = st.members[i];
    if (i > 0) out += ',';
    out += "{\"host\":";
    appendJsonString(out, m.host);
    out += ",\"self\":";
    out += m.host.length() == 0 ? "true" : "false";
    out += ",\"row\":";
    out += m.row;
    out += ",\"col\":";
    out += m.col;
    out += ",\"width\":";
    out += m.width;
    out += ",\"joined\":";
    out += m.joined ? "true" : "false";
    out += ",\"degraded\":";
    out += m.degraded ? "true" : "false";
    out += ",\"updating\":";
    out += m.updating ? "true" : "false";
    out += ",\"updateBlocked\":";
    out += m.updateBlocked ? "true" : "false";
    out += ",\"rev\":";
    appendJsonString(out, m.rev);
    out += '}';
  }
  out += "],\"rollout\":{\"phase\":";
  appendJsonString(out, st.rolloutPhase);
  out += ",\"host\":";
  appendJsonString(out, st.rolloutHost);
  out += ",\"sent\":";
  out += (unsigned long)st.rolloutSent;
  out += ",\"total\":";
  out += (unsigned long)st.rolloutTotal;
  out += "},\"imageVerifyFailed\":";
  out += st.rolloutImageFailed ? "true" : "false";
  out += '}';
  return out;
}

inline size_t buildClusterDegradedDiscoveryTopic(char* buf, size_t bufLen,
                                                 const char* deviceId) {
  return (size_t)mqttSnprintf(
      buf, bufLen,
      MQTT_FMT("homeassistant/binary_sensor/%s_cluster_degraded/config"),
      deviceId);
}

// Same shape rules as MqttHelpers.h's entities (shared device block, the
// documented HA short keys, 512-byte truncation guard in the caller).
inline size_t buildClusterDegradedDiscovery(char* buf, size_t bufLen,
                                            const char* deviceId,
                                            const char* fwVersion) {
  return (size_t)mqttSnprintf(
      buf, bufLen,
      MQTT_FMT("{\"name\":\"Cluster degraded\","
               "\"stat_t\":\"splitflap/%s/cluster_degraded\","
               "\"json_attr_t\":\"splitflap/%s/cluster/attrs\","
               "\"avty_t\":\"splitflap/%s/availability\","
               "\"uniq_id\":\"%s_cluster_degraded\","
               "\"dev_cla\":\"problem\",\"ent_cat\":\"diagnostic\","
               "\"pl_on\":\"ON\",\"pl_off\":\"OFF\"," MQTT_DEVICE_BLOCK "}"),
      deviceId, deviceId, deviceId, deviceId, deviceId, deviceId, fwVersion);
}

// text/state while leading: the wall's logical content — every grid row,
// '\n'-joined (HA renders it as the multi-line wall) and truncated to
// HA's 255-char sensor-state limit.
inline String clusterWallStateText(const String* rows, int rowCount) {
  String out;
  for (int i = 0; i < rowCount; i++) {
    if (i > 0) out += '\n';
    out += rows[i];
  }
  if (out.length() > 255) out = out.substring(0, 255);
  return out;
}
