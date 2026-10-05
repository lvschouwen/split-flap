// FollowerEscalation.cpp — RTC-memory, heap and restart glue for the
// escalation policy (#503). Rules and limits in FollowerEscalation.h.

#include "FollowerEscalation.h"

#include <Arduino.h>

#include "FollowerBus.h"      // followerBusRecovery, reflashProgress
#include "FollowerBusRecovery.h"
#include "FollowerConfig.h"
#include "FollowerRescue.h"   // rescueActive, its RTC words
#include "FollowerResetLog.h" // its RTC words
#include "FollowerWeb.h"      // isPendingReboot

// RTC user memory is shared by word offset; a new tenant must not land on an
// old one, and the whole area is 512 bytes.
static_assert(FOLLOWER_RESETLOG_RTC_OFFSET >= FOLLOWER_RESCUE_RTC_OFFSET + 3,
              "reset log overlaps the rescue record");
static_assert(FOLLOWER_ESCALATION_RTC_OFFSET * 4 >=
                  FOLLOWER_RESETLOG_RTC_OFFSET * 4 + sizeof(FollowerResetLogBlob),
              "escalation record overlaps the reset log");
static_assert(FOLLOWER_ESCALATION_RTC_OFFSET * 4 + sizeof(EscalationRecord) <= 512,
              "escalation record runs past RTC user memory");

static EscalationRecord escalationRecord;
static EscalationState escalationState;

static void escalationRecordStore() {
  ESP.rtcUserMemoryWrite(FOLLOWER_ESCALATION_RTC_OFFSET,
                         (uint32_t*)&escalationRecord,
                         sizeof(escalationRecord));
}

void escalationBootInit() {
  ESP.rtcUserMemoryRead(FOLLOWER_ESCALATION_RTC_OFFSET,
                        (uint32_t*)&escalationRecord, sizeof(escalationRecord));
  if (!escalationRecordValid(escalationRecord)) {
    escalationRecord = EscalationRecord{};
    return;
  }
  if (escalationCount(escalationRecord) > 0 &&
      escalationRecord.minutesSince == 0) {
    SerialPrint(F("escalation: this boot follows a self-restart for "));
    SerialPrintln(escalationCauseName(escalationLastCause(escalationRecord)));
  }
}

const EscalationRecord& escalationRecordGet() { return escalationRecord; }

void escalationTick() {
  static uint32_t nextStepMs = 0;
  static uint32_t nextMinuteMs = 60000UL;
  static bool suppressedLogged = false;
  uint32_t now = millis();
  if ((int32_t)(now - nextMinuteMs) >= 0) {
    nextMinuteMs = now + 60000UL;
    escalationRecordMinute(escalationRecord);
    escalationRecordStore();
  }
  if ((int32_t)(now - nextStepMs) < 0) return;
  nextStepMs = now + 1000;

  const BusRecoveryState& bus = followerBusRecovery();
  EscalationInput in;
  in.nowMs = now;
  in.busDead = bus.dead;
  in.busDeadSinceMs = bus.deadSinceMs;
  // Latched: a row that has had a unit answer has a bus worth restarting for.
  static bool unitsSeen = false;
  if (detectedUnitCount > 0) unitsSeen = true;
  in.unitsSeenThisBoot = unitsSeen;
  in.largestFreeBlock = ESP.getMaxFreeBlockSize();
  EscalationVerdict v = escalationStep(escalationState, in);
  if (v.logLowHeap) {
    SerialPrint(F("heap: largest free block "));
    SerialPrint(in.largestFreeBlock);
    SerialPrintln(F(" B for a minute — restart if it stays"));
  }
  if (v.cause == EscalationCause::None) {
    suppressedLogged = false;
    return;
  }
  // Not on the way out, and not in the rescue beacon (it never touches the
  // bus and has its own exit). A reflash or an OTA upload cannot be cut: the
  // first blocks loop() for its whole length, the second returns from loop()
  // before this tick is reached.
  if (isPendingReboot || rescueActive()) return;
  if (!escalationAllowed(escalationRecord)) {
    if (!suppressedLogged) {
      suppressedLogged = true;
      SerialPrint(F("escalation: "));
      SerialPrint(escalationCauseName((uint8_t)v.cause));
      SerialPrint(F(" — no restart, the last one was "));
      SerialPrint(escalationRecord.minutesSince);
      SerialPrintln(F(" min of uptime ago"));
    }
    return;
  }
  SerialPrint(F("escalation: "));
  SerialPrint(escalationCauseName((uint8_t)v.cause));
  SerialPrintln(F(" did not heal — restarting this board"));
  escalationRecordTaken(escalationRecord, v.cause);
  escalationRecordStore();
  isPendingReboot = true;
}
