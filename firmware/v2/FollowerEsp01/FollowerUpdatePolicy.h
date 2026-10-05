#pragma once
// FollowerUpdatePolicy.h — when this row takes the image its master offers
// over the wall link (wl.Update, #564), and what it requires of the download
// before and after anything is written to flash. Pure; natively tested by
// test_follower_update. The download itself is FollowerUpdate.cpp; the image
// checks are the ones POST /firmware/master makes (FollowerOtaImage.h).

#include <stdint.h>
#include <string.h>

#include "FollowerOtaImage.h"
#include "wall_link.pb.h"

// Where the master serves the row image.
#define FOLLOWER_UPDATE_PATH "/firmware/row"
// The master must answer the GET within this.
#define FOLLOWER_UPDATE_HTTP_TIMEOUT_MS 5000
// No byte for this long ends the download.
#define FOLLOWER_UPDATE_STALL_MS 10000UL
// However it trickles, the row is not held longer than this.
#define FOLLOWER_UPDATE_TOTAL_MS 180000UL
// Read buffer, on loop()'s stack.
#define FOLLOWER_UPDATE_CHUNK 256
// Free memory the download needs: the updater's sector buffer, the HTTP
// client and a second connection.
#define FOLLOWER_UPDATE_HEAP_NEEDED 9000

// UpdateState.detail for UPDATE_IMAGE beyond the OtaImageCheck values.
#define FOLLOWER_UPDATE_DETAIL_UNPACK 100  // would not unpack clear of its stored copy
#define FOLLOWER_UPDATE_DETAIL_KIND 101    // packed flag and file disagree

// What Update.begin can be given: the free space less the sector the core
// keeps, in whole sectors. `freeSketchSpace` = ESP.getFreeSketchSpace().
inline uint32_t followerUpdateMaxSpace(uint32_t freeSketchSpace) {
  if (freeSketchSpace < OTA_FLASH_SECTOR) return 0;
  return (freeSketchSpace - OTA_FLASH_SECTOR) & ~(OTA_FLASH_SECTOR - 1);
}

// May this offer be downloaded? UPDATE_REASON_NONE = yes. Rescue mode takes
// the rev it already runs: only an installed image ends it.
inline wl_UpdateReason followerUpdateAdmit(const wl_Update& u, const char* runningRev,
                                           bool rescue, bool unitsBusy, uint32_t maxSpace) {
  bool md5Set = false;
  for (size_t i = 0; i < sizeof(u.md5); i++) md5Set = md5Set || u.md5[i] != 0;
  if (u.rev[0] == 0 || u.size == 0 || !md5Set) return wl_UpdateReason_UPDATE_BAD_OFFER;
  if (!rescue && strcmp(u.rev, runningRev) == 0) return wl_UpdateReason_UPDATE_CURRENT;
  // The install ends in a restart, which would strand a unit mid-flash in
  // its bootloader.
  if (unitsBusy) return wl_UpdateReason_UPDATE_UNITS_BUSY;
  if (u.size > maxSpace) return wl_UpdateReason_UPDATE_TOO_LARGE;
  return wl_UpdateReason_UPDATE_REASON_NONE;
}

inline void followerUpdateMd5Hex(const uint8_t md5[16], char out[33]) {
  static const char digits[] = "0123456789abcdef";
  for (int i = 0; i < 16; i++) {
    out[2 * i] = digits[md5[i] >> 4];
    out[2 * i + 1] = digits[md5[i] & 0x0F];
  }
  out[32] = 0;
}

inline uint16_t followerUpdatePort(uint32_t offered) {
  return offered == 0 || offered > 65535 ? 80 : (uint16_t)offered;
}

// The master's answer to the GET, judged before any flash is erased.
// `contentLength` < 0 = the answer carried none.
inline wl_UpdateReason followerUpdateAnswer(int httpCode, int contentLength, uint32_t offeredSize,
                                            uint32_t& detail) {
  detail = 0;
  if (httpCode <= 0) return wl_UpdateReason_UPDATE_UNREACHABLE;
  if (httpCode != 200) {
    detail = (uint32_t)httpCode;
    return wl_UpdateReason_UPDATE_HTTP_STATUS;
  }
  if (contentLength < 0 || (uint32_t)contentLength != offeredSize) {
    detail = contentLength < 0 ? 0 : (uint32_t)contentLength;
    return wl_UpdateReason_UPDATE_SIZE_DIFFERS;
  }
  return wl_UpdateReason_UPDATE_REASON_NONE;
}

// How much of the file must be in hand before it can be judged.
inline uint32_t followerUpdateFirstLen(uint32_t size) {
  const uint32_t need = (uint32_t)(OTA_GZIP_IMAGE_OFFSET + OTA_IMAGE_HEADER_LEN);
  return size < need ? size : need;
}

// The first bytes of the file, judged before any flash is erased. `running` =
// the 4-byte header of the image at flash address 0.
inline wl_UpdateReason followerUpdateFirstBytes(const uint8_t* data, size_t len, bool packed,
                                                const uint8_t* running, uint32_t& detail) {
  detail = 0;
  OtaImageCheck check = OtaImageCheck::Ok;
  if (otaIsGzip(data, len)) {
    if (!packed) {
      detail = FOLLOWER_UPDATE_DETAIL_KIND;
      return wl_UpdateReason_UPDATE_IMAGE;
    }
    check = otaGzipCheck(data, len, running);
  } else if (len < 1 || data[0] != OTA_IMAGE_MAGIC) {
    check = OtaImageCheck::NotAnImage;
  } else if (packed) {
    detail = FOLLOWER_UPDATE_DETAIL_KIND;
    return wl_UpdateReason_UPDATE_IMAGE;
  }
  if (check == OtaImageCheck::Ok) return wl_UpdateReason_UPDATE_REASON_NONE;
  detail = (uint32_t)check;
  return wl_UpdateReason_UPDATE_IMAGE;
}

// millis()-wrap-safe: no byte since `lastByteMs`, or downloading since
// `startedMs`, for too long.
inline bool followerUpdateGiveUp(uint32_t nowMs, uint32_t lastByteMs, uint32_t startedMs) {
  return (uint32_t)(nowMs - lastByteMs) > FOLLOWER_UPDATE_STALL_MS ||
         (uint32_t)(nowMs - startedMs) > FOLLOWER_UPDATE_TOTAL_MS;
}
