#pragma once

#include <Arduino.h>
#include <Print.h>

// In-RAM ring buffer capturing the tail of every SerialPrint* call on the
// master. Used as the pre-connect staging buffer for the MQTT log sink:
// anything printed before WiFi/MQTT come up is held here and flushed to
// the `log` topic on MQTT connect (see ServiceMqttFunctions). Once MQTT
// is connected, new lines are published directly and the ring only re-
// accumulates if MQTT drops.
//
// Previously (#50 precursor) exposed via `GET /log` on the web UI at a
// 4 KB capacity; replaced by MQTT to free ~3.5 KB of static RAM for the
// MQTT client's buffers.
//
// Capture happens regardless of SERIAL_ENABLE. Disable the whole feature
// with `-D WEBLOG_DISABLE` if RAM pressure ever forces the issue.

#ifndef WEBLOG_SIZE
#define WEBLOG_SIZE 512
#endif

// `webLogPrinter` is a Print-derived sink so every type Serial can format
// (int, float, String, IPAddress, Printable, etc.) works out of the box
// without a pile of template overloads in the SerialPrint helpers.
class WebLogPrinter : public Print {
public:
  size_t write(uint8_t b) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};

extern WebLogPrinter webLogPrinter;

void webLogAppend(const char* data, size_t len);
String webLogRead();
