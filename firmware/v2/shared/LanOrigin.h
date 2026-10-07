#pragma once
// LanOrigin.h — which hosts and browser origins count as "inside the LAN",
// and the CSRF rule built on it. One definition for every board that serves
// HTTP (Master, ESP-01 follower, Rescue): this is a security boundary, so a
// fix must reach all of them. Natively tested by test_follower_json (FollowerEsp01) and
// test_rescue_cors (Rescue).

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

// Host and port as a browser compares them: lower case, the default port
// left out.
inline String lanHostPort(const String& hostPort) {
  String out = hostPort;
  out.toLowerCase();
  if (out.endsWith(":80")) out.remove(out.length() - 3);
  return out;
}

// The Origin names the very host and port the request was sent to: the page
// that sends it was served by this board.
inline bool lanSameOrigin(const String& origin, const String& host) {
  if (!origin.startsWith("http://") || host.length() == 0) return false;
  return lanHostPort(origin.substring(7)) == lanHostPort(host);
}

// CSRF gate (#313). A CORS header only decorates a response — it never
// blocks the request, so any page the owner's browser has open could drive a
// changing request (a multipart POST triggers no preflight) at a board.
// Browsers attach `Origin` to every such request, so: one whose Origin is
// not this board's own page is refused before the handler runs. The origin
// must also be a LAN one, or a public name pointed at the board's address
// would count as its own page. Traffic between boards and curl send no
// Origin and pass. Method-based, so every changing route — present and
// future — is covered without a path allowlist to drift.
inline bool lanCsrfReject(bool changes, bool hasOrigin, const String& origin,
                          const String& host) {
  if (!changes || !hasOrigin) return false;
  return !(lanOriginAllowed(origin) && lanSameOrigin(origin, host));
}
