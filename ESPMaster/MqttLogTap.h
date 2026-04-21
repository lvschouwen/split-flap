#pragma once

#include <Print.h>

// Live log streaming to MQTT (issue #50, task #10). Every SerialPrint* call
// is routed through this tap in parallel with the pre-connect ring buffer
// (WebLog.h). When MQTT is connected, the tap accumulates characters until
// a newline and publishes the completed line to <base>/log. When not
// connected, the tap short-circuits — the ring buffer is the fallback and
// gets flushed once on every reconnect (see mqttFlushLogRing()).
//
// Split into its own header so HelpersSerialHandling.h can reference it
// without dragging in AsyncMqttClient — implementation lives in
// ServiceMqttFunctions.ino where AsyncMqttClient is already included.
class MqttLogTap : public Print {
public:
  size_t write(uint8_t b) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};

extern MqttLogTap mqttLogTap;
