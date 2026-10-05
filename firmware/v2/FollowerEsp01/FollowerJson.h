#pragma once
// FollowerJson.h — the one JSON document this board serves itself: its
// identity (GET /settings, and the answer to POST /pair). Pure String
// assembly, natively tested by test_follower_json.

#include <Arduino.h>

#include "JsonEscape.h"  // appendJsonString

#define FOLLOWER_PLAT "esp01"

// `version` and `plat` are what ota-flash.sh reads. `sketchFree` is where an
// upload is stored (the handler takes one sector less); `flashMode` is the
// image header's SPI mode (0 QIO, 1 QOUT, 2 DIO, 3 DOUT), which a gzip image
// must match (#540). `flashId` is the SPI flash chip's JEDEC id as hex (low
// byte = vendor): the bootloader's copy has no per-vendor handling, so a row
// with another chip than the proven one should be known before a packed
// flash.
struct FollowerIdentity {
  const char* name = "";
  const char* rev = "";
  int width = 0;
  bool rescue = false;
  const char* masterId = "";    // "" = unpaired
  const char* masterHost = "";
  bool linked = false;          // the wall link is up
  uint32_t upSeconds = 0;
  uint32_t heapBytes = 0;
  uint32_t sketchBytes = 0;
  uint32_t sketchFreeBytes = 0;
  int flashMode = 0;
  uint32_t flashId = 0;
};

inline String followerIdentityJson(const FollowerIdentity& id) {
  String out;
  out.reserve(352);
  out += "{\"name\":";
  appendJsonString(out, String(id.name));
  out += ",\"version\":";
  appendJsonString(out, String(id.rev));
  out += ",\"plat\":\"" FOLLOWER_PLAT "\",\"width\":";
  out += id.width;
  out += ",\"rescue\":";
  out += id.rescue ? "true" : "false";
  out += ",\"master\":";
  appendJsonString(out, String(id.masterId));
  out += ",\"masterHost\":";
  appendJsonString(out, String(id.masterHost));
  out += ",\"linked\":";
  out += id.linked ? "true" : "false";
  out += ",\"up\":";
  out += String((unsigned long)id.upSeconds);
  out += ",\"heap\":";
  out += String((unsigned long)id.heapBytes);
  out += ",\"sketch\":";
  out += String((unsigned long)id.sketchBytes);
  out += ",\"sketchFree\":";
  out += String((unsigned long)id.sketchFreeBytes);
  out += ",\"flashMode\":";
  out += id.flashMode;
  out += ",\"flashId\":\"";
  out += String((unsigned long)id.flashId, HEX);
  out += "\"}";
  return out;
}
