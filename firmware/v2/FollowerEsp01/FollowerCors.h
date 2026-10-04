#pragma once
// FollowerCors.h — the #294 rung-3 CORS path allowlist of the ESP-01
// follower, natively tested by test_follower_json. Which origins count as
// LAN panes, and the CSRF rule, are the shared LanOrigin.h; this is only the
// per-member management surface the S3's wall panel fans out to. The
// per-response glue lives in FollowerWeb.cpp (the ESP8266 async fork has no
// server middleware, so each mutating handler calls lanCsrfRejectPost).

#include <Arduino.h>

#include "LanOrigin.h"

// The slice of this firmware's surface a wall pane on another board may call:
// the settings/health reads, the unit ops, /reboot and (#304) /reflash-units.
// /log is served but pulled by the leader server-to-server, so it is not
// opened here. /firmware/* stays CLOSED, as on the S3: the
// ESP-01's firmware is pushed by the S3 relay (stored image streamed
// server-to-server via clusterTask, #304 2a), never a browser cross-origin
// POST — so no CORS exception is needed here.
inline bool followerCorsPathAllowed(const String& path) {
  if (path == "/settings" || path == "/units/health" ||
      path == "/units/health/refresh" || path == "/reboot" ||
      path == "/reflash-units") {
    return true;
  }
  return path.startsWith("/unit/");
}
