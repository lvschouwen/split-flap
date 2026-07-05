#pragma once

#include <Arduino.h>
#include <Print.h>

// In-RAM ring buffer capturing the tail of every SerialPrint* call on the
// master. Exposed to the web UI via `GET /log` so you can see what the
// firmware is doing without needing a USB-serial connection (which in turn
// would require SERIAL_ENABLE=true, which disables I2C).
//
// Capture happens regardless of SERIAL_ENABLE. Disable the whole feature
// with `-D WEBLOG_DISABLE` if RAM pressure ever forces the issue.

#ifndef WEBLOG_SIZE
#define WEBLOG_SIZE 4096
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
