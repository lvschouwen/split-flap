// FollowerRescue.cpp — RTC-memory glue for the boot-rescue beacon (#343).
// Policy + contract in FollowerRescue.h.

#include <Arduino.h>

#include "FollowerConfig.h"
#include "FollowerRescue.h"

static bool beaconActive = false;
static bool healthyMarked = false;

void rescueBootInit() {
  FollowerRescueBlob blob{};
  ESP.rtcUserMemoryRead(FOLLOWER_RESCUE_RTC_OFFSET, (uint32_t*)&blob,
                        sizeof(blob));
  uint32_t prev = followerRescueDecode(blob);
  beaconActive = followerRescueBeaconAtBoot(prev);
  followerRescueEncode(blob, followerRescueNextCounter(prev));
  ESP.rtcUserMemoryWrite(FOLLOWER_RESCUE_RTC_OFFSET, (uint32_t*)&blob,
                         sizeof(blob));
  if (beaconActive) {
    SerialPrint(F("RESCUE BEACON: "));
    SerialPrint(prev);
    SerialPrintln(F(" consecutive bad boots — I2C/render disabled, "
                    "waiting for a firmware push"));
  }
}

bool rescueActive() { return beaconActive; }

void rescueMarkHealthy() {
  if (healthyMarked) return;
  healthyMarked = true;
  FollowerRescueBlob blob{};
  followerRescueEncode(blob, 0);
  ESP.rtcUserMemoryWrite(FOLLOWER_RESCUE_RTC_OFFSET, (uint32_t*)&blob,
                         sizeof(blob));
}

void rescueHealthyTick() {
  // A beacon boot never self-forgives — only a completed firmware push
  // (whose reboot runs through rescueMarkHealthy) ends the rescue state,
  // so a transient-crash row still converges back to a proven image.
  if (beaconActive || healthyMarked) return;
  if (millis() >= FOLLOWER_RESCUE_HEALTHY_MS) rescueMarkHealthy();
}
