// MasterH2 — ESP32-H2 radio coprocessor firmware.
//
// Phase 2 scaffold (#63 / #65): LittleFS + Preferences + LED heartbeat +
// config sanity print over USB-CDC.
//
// Phase 3b (#67): S3↔H2 UART link — responder side. 921600 bps 8N1 on
// UART1 (RX=IO4 from S3, TX=IO5 to S3). On a valid PING frame, emits a
// PONG echoing the seq. The heartbeat LED keeps blinking at ~1 Hz on a
// separate time slice so the main loop being alive stays independently
// visible from UART traffic.
//
// IRQ line (IO10) is left high-impedance (open-drain, driven only when
// async data is pending) — wired up for real when Zigbee/BLE features land.

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>

#include "config.h"
#include "cobs_crc.h"
#include "frame_reader.h"
#include "protocol.h"

// Pins per PCB/MASTER_V2/PINOUT.md. LED_H2_HB is on H2 IO11 (1 kΩ series to
// LED_4, active high). The Wokwi diagram (diagram.json) is wired to match.
// Do not switch to IO8 on the devkit "onboard LED" — on the real PCB IO8
// is a strap pin with no copper escape, so driving it lights nothing.
constexpr uint8_t kLedPin       = 11;
constexpr uint8_t kS3UartRx     = 4;   // H2 IO4 = UART1 RX (from S3 IO17)
constexpr uint8_t kS3UartTx     = 5;   // H2 IO5 = UART1 TX (to S3 IO18)
constexpr uint32_t kUartBaud    = 921600;
constexpr size_t kReaderBufCap  = 256;

namespace {

uint8_t g_reader_scratch[kReaderBufCap];
uint8_t g_reader_payload[kReaderBufCap];
splitflap::frame::Reader g_reader(g_reader_scratch, sizeof(g_reader_scratch),
                                  g_reader_payload, sizeof(g_reader_payload));

uint32_t g_pings_seen = 0;

void initLittleFs() {
  if (LittleFS.begin(true)) {
    Serial.printf("[H2] LittleFS OK (%u/%u bytes used)\n",
                  static_cast<unsigned>(LittleFS.usedBytes()),
                  static_cast<unsigned>(LittleFS.totalBytes()));
  } else {
    Serial.println("[H2] LittleFS FAIL");
  }
}

uint32_t bumpBootCounter() {
  Preferences prefs;
  if (!prefs.begin("smoke", false)) {
    Serial.println("[H2] Preferences FAIL");
    return 0;
  }
  uint32_t count = prefs.getUInt("boots", 0) + 1;
  prefs.putUInt("boots", count);
  prefs.end();
  return count;
}

void sendPong(uint16_t seq) {
  uint8_t msg[splitflap::proto::kHeaderSize];
  const size_t m = splitflap::proto::encodePong(seq, msg, sizeof(msg));
  if (m == 0) return;

  uint8_t wire[splitflap::frame::encodeBound(sizeof(msg))];
  const size_t n = splitflap::frame::encode(msg, m, wire, sizeof(wire));
  if (n == 0) return;

  Serial1.write(wire, n);
}

void handleFrame() {
  splitflap::proto::Parsed parsed{};
  if (!splitflap::proto::parse(g_reader.payload(), g_reader.payloadLen(),
                               &parsed)) {
    Serial.println("[H2] protocol parse rejected a frame");
    return;
  }

  switch (parsed.type) {
    case splitflap::proto::MsgType::Ping:
      ++g_pings_seen;
      sendPong(parsed.seq);
      Serial.printf("[H2] PING seq=%u -> PONG (pings_seen=%lu)\n",
                    parsed.seq, static_cast<unsigned long>(g_pings_seen));
      break;
    case splitflap::proto::MsgType::Pong:
      Serial.printf("[H2] unexpected PONG from S3 seq=%u\n", parsed.seq);
      break;
    case splitflap::proto::MsgType::Log:
      Serial.printf("[H2] S3-log: %.*s\n",
                    static_cast<int>(parsed.body_len),
                    reinterpret_cast<const char*>(parsed.body));
      break;
  }
}

void pumpUart() {
  while (Serial1.available()) {
    const int c = Serial1.read();
    if (c < 0) break;
    const auto ev = g_reader.feed(static_cast<uint8_t>(c));
    if (ev.kind == splitflap::frame::Reader::Event::Kind::Frame) {
      handleFrame();
    } else if (ev.kind == splitflap::frame::Reader::Event::Kind::Dropped) {
      Serial.printf("[H2] frame dropped err=%u\n",
                    static_cast<unsigned>(ev.err));
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(kLedPin, OUTPUT);
  delay(500);

  Serial.println("[H2] alive");
  Serial.printf("[H2] config: kMaxRows=%u, kMaxUnitsPerRow=%u, kMaxUnits=%u\n",
                splitflap::kMaxRows, splitflap::kMaxUnitsPerRow,
                splitflap::kMaxUnits);

  initLittleFs();
  Serial.printf("[H2] Preferences OK (boot count = %u)\n",
                static_cast<unsigned>(bumpBootCounter()));

  Serial1.begin(kUartBaud, SERIAL_8N1, kS3UartRx, kS3UartTx);
  Serial.printf("[H2] S3 UART up: %lu 8N1 on rx=%u tx=%u\n",
                static_cast<unsigned long>(kUartBaud), kS3UartRx, kS3UartTx);
}

void loop() {
  pumpUart();

  static uint32_t last_blink_ms = 0;
  static bool led_on = false;
  const uint32_t now = millis();
  if (now - last_blink_ms >= 500) {
    last_blink_ms = now;
    led_on = !led_on;
    digitalWrite(kLedPin, led_on ? HIGH : LOW);
  }
}
