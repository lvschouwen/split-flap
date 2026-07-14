#pragma once
// FollowerCors.h — the #294 rung-3 CORS gates, COPY of the v2 master's
// ClusterDigest.h origin/path logic (copy policy: fix bugs in both trees),
// natively tested by test_follower_json. Reflected back only for origins
// that can only exist inside the LAN — private IPv4 literals, .local names
// and localhost, all http-only. The path gate opens exactly the per-member
// management surface the S3's wall panel fans out to; /firmware/* and
// /cluster/* stay closed. The per-response glue lives in FollowerWeb.cpp
// (the ESP8266 async fork has no server middleware).

#include <Arduino.h>

inline bool followerCorsPrivateIpv4(const String& host) {
  int octets[4];
  int value = 0, digits = 0, index = 0;
  for (unsigned int i = 0; i <= host.length(); i++) {
    char c = i < host.length() ? host[i] : '.';
    if (c == '.') {
      if (digits == 0 || digits > 3 || index >= 4) return false;
      octets[index++] = value;
      value = 0;
      digits = 0;
    } else if (c >= '0' && c <= '9') {
      value = value * 10 + (c - '0');
      if (value > 255) return false;
      digits++;
    } else {
      return false;
    }
  }
  if (index != 4) return false;
  if (octets[0] == 10 || octets[0] == 127) return true;
  if (octets[0] == 192 && octets[1] == 168) return true;
  if (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) return true;
  return false;
}

inline bool followerCorsOriginAllowed(const String& origin) {
  if (!origin.startsWith("http://")) return false;
  String host = origin.substring(7);
  int cut = host.indexOf(':');
  if (cut < 0) cut = host.indexOf('/');
  if (cut >= 0) host = host.substring(0, cut);
  if (host.length() == 0) return false;
  if (host.equalsIgnoreCase("localhost")) return true;
  String lower = host;
  lower.toLowerCase();
  if (lower.endsWith(".local") && host.length() > 6) return true;
  return followerCorsPrivateIpv4(host);
}

// This firmware's served slice of the v2 surface ("/" and the log/stat
// reads don't exist here; /reboot does — the panel's reboot button).
inline bool followerCorsPathAllowed(const String& path) {
  if (path == "/settings" || path == "/units/health" ||
      path == "/units/health/refresh" || path == "/reboot") {
    return true;
  }
  return path.startsWith("/unit/");
}
