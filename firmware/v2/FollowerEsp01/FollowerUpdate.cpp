// FollowerUpdate.cpp — the download half of a row firmware update over the
// wall link. Contract in FollowerUpdate.h, rules in FollowerUpdatePolicy.h.
// The flash session is set up the way POST /firmware/master sets it up
// (FollowerWeb.cpp): same reserve, same packed-image checks, same md5 rule.
#include "FollowerUpdate.h"

#include <Arduino.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <Updater.h>

#include "FollowerConfig.h"
#include "FollowerOtaImage.h"
#include "FollowerUpdatePolicy.h"
#include "FollowerWeb.h"
#include "FollowerWifi.h"

namespace {

bool active = false;

FollowerUpdateResult failed(wl_UpdateReason reason, uint32_t detail) {
  active = false;
  return {wl_UpdatePhase_UPDATE_FAILED, reason, detail};
}

// Ends a flash session that will not be installed. A failed end() latches
// its error without clearing the session; the second call does.
void abandonFlash() {
  Update.end(false);
  Update.end(false);
  Update.clearError();
  followerTxOtaCap(false);
}

FollowerUpdateResult fetch(const wl_Update& offer, WiFiClient& stream) {
  uint8_t buf[FOLLOWER_UPDATE_CHUNK];
  uint32_t detail = 0;

  // Judge the file before the freeze and before Update.begin erases flash.
  const uint32_t firstLen = followerUpdateFirstLen(offer.size);
  const size_t got = stream.readBytes(buf, firstLen);
  if (got < firstLen) return failed(wl_UpdateReason_UPDATE_STALLED, (uint32_t)got);
  uint32_t running = 0;
  if (!ESP.flashRead(0, &running, sizeof(running))) {
    return failed(wl_UpdateReason_UPDATE_IMAGE, (uint32_t)OtaImageCheck::FlashMode);
  }
  const wl_UpdateReason first =
      followerUpdateFirstBytes(buf, got, offer.packed, (const uint8_t*)&running, detail);
  if (first != wl_UpdateReason_UPDATE_REASON_NONE) return failed(first, detail);

  followerTxOtaCap(true);  // flash writes on a sagging rail are the worse failure
  const uint32_t maxSpace = followerUpdateMaxSpace(ESP.getFreeSketchSpace());
  // A plain image reserves the whole free space; a packed one only what it
  // needs, so it is stored clear of where it unpacks to (FollowerOtaImage.h).
  const uint32_t reserved = offer.packed ? otaGzipReserve(offer.size, maxSpace) : maxSpace;
  Update.runAsync(false);  // an upload leaves it on; here the writes must yield
  if (!Update.begin(reserved, U_FLASH)) {
    // Stale updater state from an abandoned upload.
    Update.end(false);
    Update.clearError();
    if (!Update.begin(reserved, U_FLASH)) {
      detail = Update.getError();
      abandonFlash();
      return failed(wl_UpdateReason_UPDATE_FLASH, detail);
    }
  }
  char md5[33];
  followerUpdateMd5Hex(offer.md5, md5);
  if (!Update.setMD5(md5)) {
    abandonFlash();
    return failed(wl_UpdateReason_UPDATE_BAD_OFFER, 0);
  }

  OtaGzipTail tail;
  uint32_t received = 0;
  size_t have = got;
  const uint32_t startedMs = millis();
  uint32_t lastByteMs = startedMs;
  while (true) {
    if (have > 0) {
      if (Update.write(buf, have) != have) {
        detail = Update.getError();
        abandonFlash();
        return failed(wl_UpdateReason_UPDATE_FLASH, detail);
      }
      if (offer.packed) tail.feed(buf, have);
      received += have;
      have = 0;
      lastByteMs = millis();
    }
    if (received >= offer.size) break;
    if (stream.available() > 0) {
      const uint32_t left = offer.size - received;
      const int n = stream.read(buf, left < sizeof(buf) ? left : sizeof(buf));
      if (n > 0) have = (size_t)n;
      continue;
    }
    if (!stream.connected() || followerUpdateGiveUp(millis(), lastByteMs, startedMs)) {
      abandonFlash();
      return failed(wl_UpdateReason_UPDATE_STALLED, received);
    }
    delay(1);  // lets the network deliver
  }

  if (offer.packed && !otaGzipUnpackFits(tail, appAreaBytes(), reserved)) {
    // Nothing is staged for the boot copier: end(false) drops the session.
    abandonFlash();
    return failed(wl_UpdateReason_UPDATE_IMAGE, FOLLOWER_UPDATE_DETAIL_UNPACK);
  }
  if (!Update.end(true)) {
    detail = Update.getError();  // md5 mismatch lands here
    abandonFlash();
    return failed(wl_UpdateReason_UPDATE_FLASH, detail);
  }
  return {wl_UpdatePhase_UPDATE_INSTALLED, wl_UpdateReason_UPDATE_REASON_NONE, 0};
}

}  // namespace

bool updateDownloadActive() { return active; }

FollowerUpdateResult updateDownloadAndInstall(const wl_Update& offer, const char* host) {
  // operator new resets this board when it cannot serve; ask first.
  if (ESP.getMaxFreeBlockSize() < FOLLOWER_UPDATE_HEAP_NEEDED) {
    return {wl_UpdatePhase_UPDATE_FAILED, wl_UpdateReason_UPDATE_NO_MEMORY,
            ESP.getMaxFreeBlockSize()};
  }
  active = true;
  SerialPrint(F("update: fetching "));
  SerialPrintln(offer.rev);

  WiFiClient conn;
  HTTPClient http;
  http.useHTTP10(true);  // a plain body of Content-Length bytes, never chunks
  http.setReuse(false);
  http.setTimeout(FOLLOWER_UPDATE_HTTP_TIMEOUT_MS);
  if (!http.begin(conn, String(host), followerUpdatePort(offer.http_port),
                  F(FOLLOWER_UPDATE_PATH))) {
    return failed(wl_UpdateReason_UPDATE_UNREACHABLE, 0);
  }
  const int code = http.GET();
  uint32_t detail = 0;
  const wl_UpdateReason answer = followerUpdateAnswer(code, http.getSize(), offer.size, detail);
  // The body is read from the client's own connection: begin() works on a
  // copy of `conn`, which itself never connects.
  WiFiClient* body = answer == wl_UpdateReason_UPDATE_REASON_NONE ? http.getStreamPtr() : nullptr;
  FollowerUpdateResult result =
      answer != wl_UpdateReason_UPDATE_REASON_NONE ? failed(answer, detail)
      : body == nullptr ? failed(wl_UpdateReason_UPDATE_UNREACHABLE, 0)
                        : fetch(offer, *body);
  http.end();
  SerialPrint(F("update: "));
  SerialPrintln(result.phase == wl_UpdatePhase_UPDATE_INSTALLED ? F("stored, restarting")
                                                                 : F("failed, image unchanged"));
  return result;
}
