// `devLog` is a Print-derived sink that writes every byte to BOTH the USB-
// CDC Serial and the in-RAM web-log ring. Anything formatted via
// devLog.print / printf / println shows up on the USB console AND in the
// /log endpoint. Use it everywhere in this firmware where the ESP8266 code
// would have used Serial.

#pragma once

#include <Arduino.h>
#include <Print.h>

#include "web_log.h"

class DualLogger : public Print {
 public:
  size_t write(uint8_t b) override {
    Serial.write(b);
    webLogPrinter.write(b);
    return 1;
  }
  size_t write(const uint8_t* buffer, size_t size) override {
    Serial.write(buffer, size);
    webLogPrinter.write(buffer, size);
    return size;
  }
};

extern DualLogger devLog;
