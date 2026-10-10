#pragma once
// FollowerLineTest — how fast each bus line rises when it is let go, with
// only what is outside the chip pulling it up (#496). The numbers and what a
// reading means are FollowerBusDeath.h.
//
// What it is for: a row that stops answering with both lines free looks the
// same whether its cable is off or its units are deaf. How a line rises
// depends on what hangs on it — the pull-ups and the wire — so a reading
// taken at the death next to one from the working bus says whether what hangs
// on the line changed. Where this wall's pull-ups sit is not documented; the
// two readings are compared with each other, not with a number from a book.
//
// loop() context only, on an idle bus: SCL is pulsed with SDA high (nothing
// to a unit), then SDA with SCL high (a START and its STOP: an empty frame).

#include <stdint.h>

struct FollowerLineReading {
  uint16_t sda = 0;  // tenths of a microsecond; FOLLOWER_LINE_* otherwise
  uint16_t scl = 0;
};

FollowerLineReading followerLineTest();
