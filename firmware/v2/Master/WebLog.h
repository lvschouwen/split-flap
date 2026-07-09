#pragma once

#include <Arduino.h>
#include <Print.h>

// In-RAM ring buffer capturing the tail of every SerialPrint* call on the
// master, exposed to the web UI via `GET /log`. v2 copy of the v1 helper
// (#133) — v1 stays stable for OTA maintenance, so this is a copy, not a
// shared include.
//
// Disable the whole feature with `-D WEBLOG_DISABLE` if RAM pressure ever
// forces the issue (it shouldn't on the S3).

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
void webLogReset();
