// FollowerWifi.cpp — WiFi/identity/SNTP/mDNS glue (#298). Contract in
// FollowerWifi.h; the portal flow is v1's ServiceWifiFunctions.ino trimmed.

// Deliberately first, in this order (v1 rule — conflicts with other libs).
#include <DNSServer.h>
#include <ESPAsyncWiFiManager.h>

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <Updater.h>
#include <time.h>

#include "BuildVersion.h"
#include "FollowerBus.h"     // busRowMoving
#include "FollowerConfig.h"
#include "FollowerJson.h"  // FOLLOWER_PLAT
#include "FollowerWeb.h"   // isPendingReboot
#include "FollowerRescue.h"  // rescueActive
#include "FollowerWifi.h"
#include "WifiTxPolicy.h"

String effectiveDeviceName;
bool isWifiConfigured = false;

static DNSServer dnsServer;

// --- TX power ladder (#508) -----------------------------------------------------
static WifiTxState txState;
static int8_t txAppliedIndex = -1;  // -1 = nothing applied yet
static bool txOtaCapped = false;

// `force` re-sends an unchanged level: the ESP8266 cannot read the level
// back, so it is re-asserted wherever the radio may have been re-initialised.
static void txApply(uint8_t index, bool log, bool force = false) {
  if ((int8_t)index == txAppliedIndex && !force) return;
  WiFi.setOutputPower(wifiTxLevelRaw(index) / 4.0f);
  if (log && (int8_t)index != txAppliedIndex) {
    int dbm10 = wifiTxLevelDbm10(index);
    SerialPrint(F("wifi: TX power "));
    if (txAppliedIndex >= 0) {
      int was10 = wifiTxLevelDbm10((uint8_t)txAppliedIndex);
      SerialPrint(was10 / 10);
      SerialPrint('.');
      SerialPrint(was10 % 10);
      SerialPrint(F(" -> "));
    }
    SerialPrint(dbm10 / 10);
    SerialPrint('.');
    SerialPrint(dbm10 % 10);
    SerialPrintln(F(" dBm"));
  }
  txAppliedIndex = (int8_t)index;
}

int followerTxPowerDbm10() {
  return txAppliedIndex < 0 ? 0 : wifiTxLevelDbm10((uint8_t)txAppliedIndex);
}

// Join and portal: no link, no units moving yet.
static void txBringUpStep(WifiTxPhase phase, bool force = false) {
  WifiTxInput in;
  in.phase = phase;
  txApply(wifiTxPolicyStep(txState, in, millis()), true, force);
}

void followerTxOtaCap(bool on) {
  txOtaCapped = on;
  // Capped at the join ceiling, below the 10 dBm this guard used before the
  // ladder: a row that climbed past it for a weak link may stall the upload,
  // which the 30 s thaw and the leader's retry recover; a sagging rail during
  // flash writes is the worse failure.
  uint8_t index = txState.index;
  if (on && index > WIFI_TX_JOIN_CEILING_INDEX) {
    index = WIFI_TX_JOIN_CEILING_INDEX;
  }
  txApply(index, false);
}

void followerTxTick() {
  static uint32_t nextMs = 0;
  static bool reasserted = false;
  uint32_t now = millis();
  if ((int32_t)(now - nextMs) < 0) return;
  nextMs = now + 1000;
  if (txOtaCapped) return;
  if (!reasserted) {  // the join or the portal may have re-initialised the PHY
    reasserted = true;
    txApply(txState.index, false, true);
  }

  WifiTxInput in;
  in.phase = WifiTxPhase::Online;
  in.linkUp = WiFi.status() == WL_CONNECTED;
  in.rssiDbm = in.linkUp ? WiFi.RSSI() : 0;
  // RSSI() reads a non-negative sentinel when the link fell between the two
  // calls; fed to the ladder it would count as a perfect signal.
  if (in.linkUp && in.rssiDbm >= 0) return;

  // An up-step needs an idle row, and asking the row costs one I2C read per
  // unit — so ask only when a step is actually due.
  WifiTxState trial = txState;
  in.unitsIdle = true;
  uint8_t index = wifiTxPolicyStep(trial, in, now);
  if (index > txState.index && !rescueActive() && busRowMoving()) {
    in.unitsIdle = false;
    index = wifiTxPolicyStep(txState, in, now);
  } else {
    txState = trial;
  }
  // The row poll can yield to the upload handler, which then owns the level.
  if (txOtaCapped) return;
  txApply(index, true);
}

static bool waitForWifiConnected(int timeoutSeconds) {
  for (int elapsed = 0; elapsed < timeoutSeconds; elapsed++) {
    txBringUpStep(WifiTxPhase::Joining);
    if (WiFi.status() == WL_CONNECTED) {
      SerialPrint(F("connected. IP Address: "));
      SerialPrintln(WiFi.localIP());
      return true;
    }
    SerialPrint('.');
    delay(1000);
  }
  SerialPrintln(F(" timed out"));
  return false;
}

static bool tryJoinKnownWifi(int timeoutSeconds) {
  WiFi.mode(WIFI_STA);  // the radio is off until here (core 3.x boot default)
  txBringUpStep(WifiTxPhase::Joining);  // before any join burst
  WiFi.hostname(effectiveDeviceName.c_str());
  WiFi.setAutoReconnect(true);
  if (WiFi.SSID().length() == 0) {
    SerialPrintln(F("No WiFi credentials persisted in SDK flash"));
    return false;
  }
  SerialPrint(F("Joining known WiFi "));
  WiFi.begin();  // no args = the SDK-persisted credentials
  return waitForWifiConnected(timeoutSeconds);
}

void wifiInit(AsyncWebServer& server) {
  effectiveDeviceName = "split-flap-" + String(ESP.getChipId(), HEX);
  SerialPrint(F("Device identity: "));
  SerialPrintln(effectiveDeviceName);

  if (tryJoinKnownWifi(30)) {
    isWifiConfigured = true;
    return;
  }

  SerialPrintln(F("Starting WiFi setup portal..."));
  txBringUpStep(WifiTxPhase::Portal);
  // Function-local static: the manager registers handlers on the shared
  // server, so it must outlive this call (v1 keeps its instance global).
  static AsyncWiFiManager wifiManager(&server, &dnsServer);
  wifiManager.setSaveConfigCallback([]() {
    // Reboot after the portal saves so the async server rebinds cleanly to
    // the STA interface (v1 rule).
    isPendingReboot = true;
  });
  wifiManager.setConfigPortalTimeout(300);
  wifiManager.setConnectTimeout(30);
  if (wifiManager.startConfigPortal(
          (effectiveDeviceName + "-setup").c_str())) {
    isWifiConfigured = true;
    return;
  }
  // Portal window elapsed unconfigured: reboot and retry (the router may
  // just have been down).
  SerialPrintln(F("Setup portal timed out — rebooting to retry"));
  isPendingReboot = true;
}

void wifiServicesInit(int rowWidth) {
  // Epoch-only SNTP: commitAt flips stay in unison with the wall; unsynced
  // renders fall back to immediate (FollowerPolicy rule). No blocking wait
  // — the row is useful before sync.
  configTime(0, 0, "pool.ntp.org");

  if (MDNS.begin(effectiveDeviceName.c_str())) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("splitflap", "tcp", 80);
    MDNS.addServiceTxt("splitflap", "tcp", "name",
                       effectiveDeviceName.c_str());
    MDNS.addServiceTxt("splitflap", "tcp", "rev", GIT_REV);
    MDNS.addServiceTxt("splitflap", "tcp", "width",
                       String(rowWidth).c_str());
    // #297: the plat tag is what keeps the S3 leader's firmware rollout
    // away from this board and tags it in the discovery scan.
    MDNS.addServiceTxt("splitflap", "tcp", "plat", FOLLOWER_PLAT);
    SerialPrintln(F("mDNS responder started"));
  } else {
    SerialPrintln(F("Error setting up mDNS responder"));
  }
}

#define FOLLOWER_RADIO_RECONNECT_HOLD_MS 60000UL

bool followerRadioBusy() {
  static uint32_t downSinceMs = 0;
  static bool down = false;
  if (Update.isRunning()) return true;
  if (WiFi.status() == WL_CONNECTED) {
    down = false;
    return false;
  }
  uint32_t now = millis();
  if (!down) {
    down = true;
    downSinceMs = now;
  }
  return now - downSinceMs < FOLLOWER_RADIO_RECONNECT_HOLD_MS;
}
