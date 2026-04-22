// In-RAM ring buffer capturing whatever the firmware prints. Exposed at
// GET /log over HTTP so the device can be debugged through the web UI
// without a USB-serial connection. Ported from firmware/ESPMaster/WebLog.h;
// differences from the ESP8266 version:
//   - Uses portMUX_TYPE spinlock (multicore-safe on ESP32-S3) instead of
//     noInterrupts()/interrupts(). Log writes may happen on either core.
//   - Size defaults to 8 KB because the S3 has plenty of SRAM (vs 4 KB on
//     the ESP-01 where every byte counted).

#pragma once

#include <Arduino.h>
#include <Print.h>

#ifndef WEBLOG_SIZE
#define WEBLOG_SIZE 8192
#endif

class WebLogPrinter : public Print {
 public:
  size_t write(uint8_t b) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};

extern WebLogPrinter webLogPrinter;

void webLogAppend(const char* data, size_t len);
String webLogRead();
