#pragma once
// LanOrigin.h — which hosts and browser origins count as "inside the LAN",
// and the CSRF rule built on it. One definition for every board that serves
// HTTP (Master, ESP-01 follower, Rescue): this is a security boundary, so a
// fix must reach all of them. Each tree keeps only its own CORS path
// allowlist. Natively tested by test_cluster_digest (Master),
// test_follower_json (FollowerEsp01) and test_rescue_cors (Rescue).

#include <Arduino.h>

// A dotted-quad literal in 10/8, 127/8, 172.16/12 or 192.168/16.
inline bool lanPrivateIpv4(const String& host) {
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

// A host that can only resolve inside the LAN: a private-IPv4 literal, a
// `.local` mDNS name, or localhost.
inline bool lanHostIsLocal(const String& host) {
  if (host.length() == 0) return false;
  if (host.equalsIgnoreCase("localhost")) return true;
  String lower = host;
  lower.toLowerCase();
  if (lower.endsWith(".local") && host.length() > 6) return true;
  return lanPrivateIpv4(host);
}

// A browser Origin that can only be another pane inside the LAN. http-only:
// the boards serve plain http, so any https or public origin is by
// definition not one of them.
inline bool lanOriginAllowed(const String& origin) {
  if (!origin.startsWith("http://")) return false;
  String host = origin.substring(7);
  int cut = host.indexOf(':');
  if (cut < 0) cut = host.indexOf('/');
  if (cut >= 0) host = host.substring(0, cut);
  return lanHostIsLocal(host);
}

// CSRF gate (#313). A CORS header only decorates a response — it never
// blocks the request, so any web page a LAN user opened could drive a
// mutating form-POST (multipart triggers no preflight) at a board. Browsers
// attach `Origin` to every POST, so: a POST carrying an Origin that is NOT a
// LAN pane is cross-site forgery and is refused before the handler runs.
// Server-to-server traffic and curl send no Origin and pass; a board's own
// LAN web UI sends a LAN origin and passes. Method-based, so every mutating
// POST — present and future — is covered without a path allowlist to drift.
inline bool lanCsrfRejectPost(bool isPost, bool hasOrigin,
                              const String& origin) {
  return isPost && hasOrigin && !lanOriginAllowed(origin);
}
