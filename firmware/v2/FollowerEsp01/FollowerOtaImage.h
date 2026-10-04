#pragma once
// FollowerOtaImage.h — what POST /firmware/master checks about the image
// itself (#540). Pure; natively tested by test_follower_ota_image.
//
// This board has one app area: the upload is stored behind the running image
// and eboot copies it over that image at reboot, unpacking it if it is gzip.
// A gzip upload is what lets the image outgrow half the flash — and it is the
// one upload the ESP8266 core cannot check: Update skips its flash-size test
// for it and cannot patch the flash mode into it, so whatever header the
// image was built with reaches sector 0 as it is. A mode this board's flash
// cannot boot leaves it dead until a USB flash. So a gzip upload must show
// its image header (the build leaves the first bytes uncompressed —
// buildtools/fwbuild.py ota_gzip_image) and that header must carry the flash
// config this board is running. A plain image is the core's to check.

#include <Arduino.h>

// The file fwbuild.ota_gzip_image() writes: gzip header (10 bytes, no
// optional fields), one stored block header (5), then the image verbatim.
static const size_t OTA_GZIP_IMAGE_OFFSET = 15;
// Image header: magic, segment count, flash mode, flash size/frequency.
static const size_t OTA_IMAGE_HEADER_LEN = 4;
static const uint8_t OTA_IMAGE_MAGIC = 0xE9;

// Decided on the first byte alone: the web library hands over whatever part
// of the file shared a TCP segment with the part headers, which can be one
// byte. An image starts 0xE9, so 0x1F can only be gzip — and must take the
// gzip checks even when the chunk is too short to pass them.
inline bool otaIsGzip(const uint8_t* data, size_t len) {
  return len >= 1 && data[0] == 0x1F;
}

enum class OtaImageCheck : uint8_t {
  Ok = 0,
  ShortChunk,      // first chunk ends before the image header
  NotInspectable,  // gzip, but not with the header left uncompressed
  NotAnImage,      // the uncompressed bytes are not an ESP8266 image
  FlashMode,       // image would change this board's flash mode
  FlashSize,       // image is built for another flash size
};

// `running` = the 4-byte header of the image at flash address 0.
inline OtaImageCheck otaGzipCheck(const uint8_t* data, size_t len,
                                  const uint8_t* running) {
  if (len < OTA_GZIP_IMAGE_OFFSET + OTA_IMAGE_HEADER_LEN) {
    return OtaImageCheck::ShortChunk;
  }
  // Gzip, deflate, no FEXTRA/FNAME/FCOMMENT/FHCRC (each would move the
  // offset).
  if (data[0] != 0x1F || data[1] != 0x8B || data[2] != 8 || data[3] != 0) {
    return OtaImageCheck::NotInspectable;
  }
  // A stored, non-final block long enough to hold the header.
  uint16_t storedLen = (uint16_t)(data[11] | (data[12] << 8));
  uint16_t storedInv = (uint16_t)(data[13] | (data[14] << 8));
  if (data[10] != 0x00 || storedLen < OTA_IMAGE_HEADER_LEN ||
      (uint16_t)~storedLen != storedInv) {
    return OtaImageCheck::NotInspectable;
  }
  const uint8_t* image = data + OTA_GZIP_IMAGE_OFFSET;
  if (image[0] != OTA_IMAGE_MAGIC) return OtaImageCheck::NotAnImage;
  if (image[2] != running[2]) return OtaImageCheck::FlashMode;
  if ((image[3] >> 4) != (running[3] >> 4)) return OtaImageCheck::FlashSize;
  return OtaImageCheck::Ok;
}

inline const __FlashStringHelper* otaImageCheckReason(OtaImageCheck c) {
  switch (c) {
    case OtaImageCheck::ShortChunk:
      return F("gzip image: the first chunk arrived too short to inspect "
               "— nothing was flashed, retry the upload");
    case OtaImageCheck::NotInspectable:
      return F("gzip image must be the build's follower-<rev>-gz.bin (a "
               "plain gzip hides the image header)");
    case OtaImageCheck::NotAnImage:
      return F("gzip image does not contain an ESP8266 image");
    case OtaImageCheck::FlashMode:
      return F("gzip image has another flash mode than this board runs — "
               "upload the plain image");
    case OtaImageCheck::FlashSize:
      return F("gzip image is built for another flash size");
    default:
      return F("");
  }
}

// The last four bytes of a gzip file are the unpacked length, and eboot
// writes exactly that many bytes from flash address 0 upward. Chunks split
// anywhere, so the tail is carried across them.
struct OtaGzipTail {
  uint8_t last[4] = {0, 0, 0, 0};
  size_t seen = 0;

  void feed(const uint8_t* data, size_t len) {
    for (size_t i = (len > 4 ? len - 4 : 0); i < len; i++) {
      last[0] = last[1];
      last[1] = last[2];
      last[2] = last[3];
      last[3] = data[i];
    }
    seen += len;
  }
  uint32_t unpackedLen() const {
    return (uint32_t)last[0] | ((uint32_t)last[1] << 8) |
           ((uint32_t)last[2] << 16) | ((uint32_t)last[3] << 24);
  }
};

static const uint32_t OTA_FLASH_SECTOR = 0x1000;

inline uint32_t otaSectorCeil(uint32_t n) {
  return (n + OTA_FLASH_SECTOR - 1) & ~(OTA_FLASH_SECTOR - 1);
}

// How much to reserve for a gzip upload. Update stores an upload in the
// LAST `size` bytes of the app area, so reserving only what the request
// carries puts the packed copy as high as it can go, leaving the most room
// under it for the unpacked image. `contentLen` is the request's
// Content-Length (the file plus multipart framing, so never too small);
// 0 = unknown, and then the whole free space is reserved, as for a plain
// image. `maxSpace` is that whole free space, a sector multiple.
inline uint32_t otaGzipReserve(uint32_t contentLen, uint32_t maxSpace) {
  if (contentLen == 0) return maxSpace;
  uint32_t need = otaSectorCeil(contentLen);
  return need < maxSpace ? need : maxSpace;
}

// eboot unpacks from the stored copy to address 0 upward, erasing as it
// goes, and never checks whether it has reached its own input. An image
// that unpacks faster than its packed copy is consumed would overwrite
// input not read yet and leave the board without a bootable image. So the
// unpacked image must END below where the packed copy STARTS — then no
// ordering of reads and writes can matter. That also keeps it inside the
// app area (above it: the EEPROM sector, rf-cal, the SDK's WiFi config).
// `reserved` = what Update.begin was given.
inline bool otaGzipUnpackFits(const OtaGzipTail& tail, uint32_t appArea,
                              uint32_t reserved) {
  uint32_t n = tail.unpackedLen();
  if (tail.seen < 4 || n < OTA_IMAGE_HEADER_LEN) return false;
  if (reserved > appArea || n > appArea) return false;
  return otaSectorCeil(n) <= appArea - reserved;
}
