#pragma once
// SntpReply.h — the answer to a time request (#559/#566). The row boards
// point their built-in SNTP client at the master, so that a row agrees with
// the clock the master names flip instants on, whatever the internet says.
// Pure, natively tested by test_sntp_reply; the UDP socket is the link
// task's (WallLink.cpp).
//
// The packet is RFC 4330's 48 bytes. A master whose own clock is not synced
// does not answer at all (the caller's decision): a row without time flips on
// arrival, which is what the master asks for then anyway.
//
// No existing server fits: the ESP32 ones are whole firmwares around a GPS
// receiver or ESPHome components, and lwIP's SNTP app is a client only.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SNTP_PORT 123
#define SNTP_PACKET_LEN 48
// This master takes its time from an internet server (stratum 1 to 3).
#define SNTP_STRATUM 4
// Seconds from 1900-01-01 (the NTP era) to 1970-01-01 (Unix time).
#define SNTP_UNIX_OFFSET_S 2208988800ULL

inline void sntpPutTime(uint8_t* out, uint64_t epochUs) {
  const uint32_t seconds = (uint32_t)(epochUs / 1000000ULL + SNTP_UNIX_OFFSET_S);
  const uint32_t fraction = (uint32_t)(((epochUs % 1000000ULL) << 32) / 1000000ULL);
  for (int i = 0; i < 4; i++) {
    out[i] = (uint8_t)(seconds >> (24 - 8 * i));
    out[4 + i] = (uint8_t)(fraction >> (24 - 8 * i));
  }
}

// `rxEpochUs`: this board's clock when the request arrived; `txEpochUs`: now,
// as the reply leaves. False when the packet is not a client's request: it
// gets no answer.
inline bool sntpBuildReply(const uint8_t* request, size_t length, uint64_t rxEpochUs,
                           uint64_t txEpochUs, uint8_t reply[SNTP_PACKET_LEN]) {
  if (length < SNTP_PACKET_LEN) return false;
  const uint8_t version = (request[0] >> 3) & 0x07;
  const uint8_t mode = request[0] & 0x07;
  if (mode != 3 || version < 1 || version > 4) return false;
  memset(reply, 0, SNTP_PACKET_LEN);
  reply[0] = (uint8_t)((version << 3) | 4);  // no leap warning, server
  reply[1] = SNTP_STRATUM;
  reply[2] = request[2];                     // poll, echoed
  reply[3] = (uint8_t)-10;                   // precision: about a millisecond
  reply[7] = 0x40;                           // root delay 1/4 s (16.16)
  reply[11] = 0x40;                          // root dispersion 1/4 s
  memcpy(reply + 12, "WALL", 4);             // reference id: opaque at this stratum
  sntpPutTime(reply + 16, rxEpochUs);        // reference
  memcpy(reply + 24, request + 40, 8);       // originate = the client's transmit
  sntpPutTime(reply + 32, rxEpochUs);        // receive
  sntpPutTime(reply + 40, txEpochUs);        // transmit
  return true;
}
