// Sole includer of the generated WebAssets.h (#338 rule, kept after the web
// UI was removed): the generated arrays have internal linkage, so a second
// include would duplicate the whole bundled unit image into another TU.
//
// This used to be WebContent.cpp, which served the browser's HTML, CSS, JS,
// favicon, captive-portal page, timezone table and SSE stream. All of that
// went with the UI; the device serves an HTTP API and no pages. What was
// left underneath was this: displayTask reaching the bundled unit firmware
// (#205) through two accessors, so the file is now named for that job.

#include "ContentState.h"

#include "WebAssets.h"

// On the S3, PROGMEM is flash-mapped and directly readable, so the pointer
// works as a plain byte buffer.
const uint8_t* unitFirmwareBin() { return UNIT_FIRMWARE_BIN; }
size_t unitFirmwareBinLen() { return UNIT_FIRMWARE_BIN_LEN; }
