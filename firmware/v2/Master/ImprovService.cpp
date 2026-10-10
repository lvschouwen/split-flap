#include "ImprovService.h"

#include <ArduinoJson.h>

#include "BuildVersion.h"
#include "HelpersSerialHandling.h"
#include "ImprovPolicy.h"
#include "UartLog.h"
#include "WifiService.h"

static const int UART_PORT = 1;
static Stream* const ports[IMPROV_PORTS] = {&Serial, &Serial0};
static ImprovRx rx[IMPROV_PORTS];
static std::string boardName;

// Which port is owed the result of the credentials it brought, across the
// restart that saves them (ImprovPolicy.h). netTask only.
RTC_NOINIT_ATTR static ImprovPendingRecord pending;
static int owedPort = -1;  // read from `pending` once, at init

// One scan at a time, answered to the port that asked.
static int scanPort = -1;
static uint32_t scanAskedMs = 0;
static const uint32_t SCAN_ANSWER_MS = 20000;

// Bytes taken from one port in one tick: a request is under 300.
static const int READ_BUDGET = 256;

void improvServiceInit(const String& deviceName) {
  boardName = deviceName.c_str();
  owedPort = improvPendingDecode(pending);
}

static ImprovFacts currentFacts() {
  ImprovFacts facts;
  WifiPhase phase = wifiServicePhase();
  String url = wifiServiceUrl();
  if (url.length() > 0) {
    facts.link = ImprovLink::Online;
    facts.url = url.c_str();
  } else if (phase == WifiPhase::Portal) {
    facts.link = ImprovLink::Ready;
  } else {
    facts.link = ImprovLink::Joining;
  }
  facts.canScan = facts.link != ImprovLink::Joining;
  facts.name = boardName;
  facts.rev = GIT_REV;
  return facts;
}

// How long an answer waits for room on the UART port before it is dropped.
static const uint32_t UART_ROOM_WAIT_MS = 300;

// A packet must start a line of its own (ImprovPolicy.h), so it is written
// whole under the lock every log line is written under. Every logger on both
// cores waits on that lock: room on the UART port is waited for before it is
// taken, never under it, and an answer that still has none is dropped (the
// installer asks again).
static void writePackets(int port, const ImprovOut& out) {
  if (out.packets.empty()) return;
  if (port == UART_PORT) {
    size_t total = 0;
    for (const ImprovPacket& packet : out.packets) total += packet.size();
    uint32_t since = millis();
    while (!uartLogHasRoom(total)) {
      if (millis() - since >= UART_ROOM_WAIT_MS) return;
      vTaskDelay(1);
    }
  }
  SerialPrintLock lock;
  for (const ImprovPacket& packet : out.packets) {
    ports[port]->write(packet.data(), packet.size());
  }
}

static void act(int port, const ImprovOut& out) {
  if (out.credentials) {
    SerialPrintln(F("WiFi credentials submitted over USB"));
    wifiStagePortalConfig(String(out.ssid.c_str()), String(out.password.c_str()));
    improvPendingEncode(pending, (uint8_t)port);
  }
  writePackets(port, out);
  if (!out.scan) return;
  if (scanPort < 0) {
    wifiStageScan();
    scanPort = port;
    scanAskedMs = millis();
  } else if (scanPort != port) {
    ImprovOut none;
    improvScanReply({}, none);
    writePackets(port, none);
  }
}

static void pumpScan() {
  if (scanPort < 0) return;
  String json = wifiScanResultJson();  // "" until the scan is done
  std::vector<ImprovNetwork> networks;
  if (json.length() == 0) {
    if (millis() - scanAskedMs < SCAN_ANSWER_MS) return;
  } else {
    JsonDocument doc;
    if (deserializeJson(doc, json) == DeserializationError::Ok) {
      for (JsonObjectConst network : doc["networks"].as<JsonArrayConst>()) {
        ImprovNetwork n;
        n.ssid = network["ssid"] | "";
        n.rssi = network["rssi"] | -127;
        n.secure = network["secure"] | true;
        networks.push_back(n);
      }
    }
  }
  ImprovOut out;
  improvScanReply(networks, out);
  writePackets(scanPort, out);
  scanPort = -1;
}

void improvServiceTick() {
  for (int port = 0; port < IMPROV_PORTS; port++) {
    if (ports[port]->available() <= 0) continue;
    ImprovFacts facts = currentFacts();
    ImprovOut out;
    for (int n = 0; n < READ_BUDGET && ports[port]->available() > 0; n++) {
      int byte = ports[port]->read();
      if (byte < 0) break;
      improvFeed(rx[port], (uint8_t)byte, facts, out);
    }
    act(port, out);
  }

  pumpScan();

  if (owedPort >= 0) {
    ImprovOut out;
    if (improvOutcome(currentFacts(), wifiServicePhase() == WifiPhase::Portal, out)) {
      writePackets(owedPort, out);
      improvPendingClear(pending);
      owedPort = -1;
    }
  }
}
