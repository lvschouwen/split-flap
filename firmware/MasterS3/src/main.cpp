// MasterS3 — ESP32-S3 firmware.
//
// Source of truth for pins and addresses: PCB/MASTER_V2/PINOUT.md and
// PCB/MASTER_V2/SCHEMATIC.md. Do not drift from those without updating
// the PCB docs first.
//
// Phase 2 scaffold (#63 / #65): LittleFS + Preferences + USB-CDC boot
// banner with config sanity print.
//
// Phase 3b (#67): S3↔H2 UART link alive. 921600 bps 8N1 on UART1
// (TX=IO17, RX=IO18). Sends PING every 1 s; matches PONG by seq and logs
// RTT; detects link down within 3 s.
//
// H2 control pins (per PINOUT.md §"S3 firmware-visible constraints"):
//   IO11 H2_RESET_N  — idle INPUT (high-Z). External pullup on H2 EN
//                      holds it high. Switched OUTPUT low only to reset.
//   IO12 H2_IRQ      — INPUT, external 10 kΩ pullup on S3 side. No internal
//                      pullup. Falling edge = H2 has async data (Zigbee/BLE).
//   IO13 H2_BOOT_SEL — idle INPUT (high-Z). External pullup on H2 IO9.
//                      Switched OUTPUT low only during H2 OTA entry.
//
// NOTE: no direct S3 GPIO LED indicator here. S3 indicator LEDs (heartbeat,
// WiFi, I2C act, UART act, row-select, OTA, fault) are all driven via the
// TLC5947 on SPI (IO4=SCK, IO5=SIN, IO6=XLAT, IO7=BLANK). TLC5947 driver
// is a separate phase — link activity must not touch IO4-7 directly.

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>

#include "config.h"
#include "cobs_crc.h"
#include "frame_reader.h"
#include "protocol.h"

namespace pins {
// H2 interlink — see PCB/MASTER_V2/PINOUT.md §"Inter-MCU signal summary".
constexpr uint8_t h2_rst       = 11;  // Output only during reset; idle INPUT.
constexpr uint8_t h2_irq       = 12;  // Input, external 10 kΩ pullup.
constexpr uint8_t h2_boot_sel  = 13;  // Output only during OTA entry; idle INPUT.
constexpr uint8_t h2_uart_tx   = 17;  // S3 UART1 TX → H2 IO4
constexpr uint8_t h2_uart_rx   = 18;  // S3 UART1 RX ← H2 IO5
}  // namespace pins

namespace {

constexpr uint32_t kUartBaud        = 921600;
constexpr uint32_t kPingIntervalMs  = 1000;
constexpr uint32_t kLinkTimeoutMs   = 3000;
constexpr size_t   kReaderBufCap    = 256;

uint8_t g_reader_scratch[kReaderBufCap];
uint8_t g_reader_payload[kReaderBufCap];
splitflap::frame::Reader g_reader(g_reader_scratch, sizeof(g_reader_scratch),
                                  g_reader_payload, sizeof(g_reader_payload));

uint16_t g_next_seq          = 1;
uint16_t g_last_ping_seq     = 0;
uint32_t g_last_ping_tx_ms   = 0;
uint32_t g_last_ping_sent_ms = 0;
uint32_t g_last_pong_rx_ms   = 0;
bool     g_link_up           = false;

void initLittleFs() {
  if (LittleFS.begin(true)) {
    Serial.printf("[S3] LittleFS OK (%u/%u bytes used)\n",
                  static_cast<unsigned>(LittleFS.usedBytes()),
                  static_cast<unsigned>(LittleFS.totalBytes()));
  } else {
    Serial.println("[S3] LittleFS FAIL — mount + format both refused");
  }
}

uint32_t bumpBootCounter() {
  Preferences prefs;
  if (!prefs.begin("smoke", false)) {
    Serial.println("[S3] Preferences FAIL — could not open 'smoke' namespace");
    return 0;
  }
  uint32_t count = prefs.getUInt("boots", 0) + 1;
  prefs.putUInt("boots", count);
  prefs.end();
  return count;
}

void initH2Pins() {
  // RESET_N and BOOT_SEL stay INPUT (high-Z) during normal operation.
  // External pullups on the H2 side keep the signals high. Firmware
  // switches either pin to OUTPUT LOW only during reset / OTA entry,
  // then returns it to INPUT.
  pinMode(pins::h2_rst, INPUT);
  pinMode(pins::h2_boot_sel, INPUT);
  // IRQ input — no internal pullup; external 10 kΩ on the S3 side.
  pinMode(pins::h2_irq, INPUT);
}

void sendPing() {
  uint8_t msg[splitflap::proto::kHeaderSize];
  const size_t m = splitflap::proto::encodePing(g_next_seq, msg, sizeof(msg));
  if (m == 0) return;

  uint8_t wire[splitflap::frame::encodeBound(sizeof(msg))];
  const size_t n = splitflap::frame::encode(msg, m, wire, sizeof(wire));
  if (n == 0) return;

  Serial1.write(wire, n);
  g_last_ping_seq     = g_next_seq++;
  g_last_ping_tx_ms   = millis();
  g_last_ping_sent_ms = g_last_ping_tx_ms;
}

void handleFrame() {
  splitflap::proto::Parsed parsed{};
  if (!splitflap::proto::parse(g_reader.payload(), g_reader.payloadLen(),
                               &parsed)) {
    Serial.println("[S3] protocol parse rejected a frame");
    return;
  }

  const uint32_t now = millis();
  switch (parsed.type) {
    case splitflap::proto::MsgType::Pong: {
      g_last_pong_rx_ms = now;
      if (parsed.seq == g_last_ping_seq) {
        const uint32_t rtt = now - g_last_ping_tx_ms;
        Serial.printf("[S3] PONG seq=%u rtt=%u ms\n",
                      parsed.seq, static_cast<unsigned>(rtt));
      } else {
        Serial.printf("[S3] PONG seq=%u (stale — expected %u)\n",
                      parsed.seq, g_last_ping_seq);
      }
      if (!g_link_up) {
        g_link_up = true;
        Serial.println("[S3] link UP");
      }
      break;
    }
    case splitflap::proto::MsgType::Log: {
      Serial.printf("[S3] H2-log: %.*s\n",
                    static_cast<int>(parsed.body_len),
                    reinterpret_cast<const char*>(parsed.body));
      break;
    }
    case splitflap::proto::MsgType::Ping:
      Serial.printf("[S3] unexpected PING from H2 seq=%u\n", parsed.seq);
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
      Serial.printf("[S3] frame dropped err=%u\n",
                    static_cast<unsigned>(ev.err));
    }
  }
}

void serviceLink() {
  const uint32_t now = millis();
  if (now - g_last_ping_sent_ms >= kPingIntervalMs) {
    sendPing();
  }
  if (g_link_up && (now - g_last_pong_rx_ms >= kLinkTimeoutMs)) {
    g_link_up = false;
    Serial.println("[S3] link DOWN");
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("[S3] alive");
  Serial.printf("[S3] config: kMaxRows=%u, kMaxUnitsPerRow=%u, kMaxUnits=%u\n",
                splitflap::kMaxRows, splitflap::kMaxUnitsPerRow,
                splitflap::kMaxUnits);

  initLittleFs();
  Serial.printf("[S3] Preferences OK (boot count = %u)\n",
                static_cast<unsigned>(bumpBootCounter()));

  initH2Pins();
  Serial1.begin(kUartBaud, SERIAL_8N1, pins::h2_uart_rx, pins::h2_uart_tx);
  Serial.printf("[S3] H2 UART up: %lu 8N1 on rx=%u tx=%u\n",
                static_cast<unsigned long>(kUartBaud),
                pins::h2_uart_rx, pins::h2_uart_tx);
}

void loop() {
  pumpUart();
  serviceLink();
}
