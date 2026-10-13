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

// An IPv6 literal as a URL writes it between brackets, without them: hex
// groups of one to four digits, one "::" at most, an optional zone after
// '%'. Fills the 16 bytes. No embedded IPv4 tail.
inline bool lanParseIpv6(const String& text, uint8_t out[16]) {
  String t = text;
  const int zone = t.indexOf('%');
  if (zone >= 0) t = t.substring(0, zone);
  if (t.length() == 0) return false;
  uint16_t groups[8];
  int count = 0, gap = -1;
  unsigned int i = 0;
  const unsigned int n = t.length();
  if (t.startsWith("::")) {
    gap = 0;
    i = 2;
  } else if (t[0] == ':') {
    return false;
  }
  while (i < n) {
    int value = 0, digits = 0;
    while (i < n && digits < 4) {
      const char c = t[i];
      int d;
      if (c >= '0' && c <= '9') d = c - '0';
      else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
      else break;
      value = value * 16 + d;
      digits++;
      i++;
    }
    if (digits == 0 || count >= 8) return false;
    groups[count++] = (uint16_t)value;
    if (i == n) break;
    if (t[i] != ':') return false;
    i++;
    if (i < n && t[i] == ':') {
      if (gap >= 0) return false;
      gap = count;
      i++;
      if (i == n) break;
    } else if (i == n) {
      return false;  // a trailing single colon
    }
  }
  if (gap < 0 && count != 8) return false;
  if (gap >= 0 && count >= 8) return false;
  int o = 0;
  for (int g = 0; g < count; g++) {
    if (g == gap) {
      for (int z = 0; z < 8 - count; z++) {
        out[o++] = 0;
        out[o++] = 0;
      }
    }
    out[o++] = (uint8_t)(groups[g] >> 8);
    out[o++] = (uint8_t)groups[g];
  }
  if (gap == count) {
    while (o < 16) out[o++] = 0;
  }
  return o == 16;
}

// Link-local (fe80::/10), unique-local (fc00::/7) or loopback (::1).
inline bool lanPrivateIpv6(const uint8_t a[16]) {
  if (a[0] == 0xfe && (a[1] & 0xc0) == 0x80) return true;
  if ((a[0] & 0xfe) == 0xfc) return true;
  for (int i = 0; i < 15; i++) {
    if (a[i] != 0) return false;
  }
  return a[15] == 1;
}

// An IPv6 literal that is one of this board's own addresses by shape: a
// private one, or a global unicast one (2000::/3) the network handed out
// (#612). An address literal cannot be a name pointed at the board, which
// is what the LAN rule keeps out; for a global address the network's
// firewall is the boundary, as README says.
inline bool lanIpv6Local(const String& literal) {
  uint8_t a[16];
  if (!lanParseIpv6(literal, a)) return false;
  return lanPrivateIpv6(a) || (a[0] & 0xe0) == 0x20;
}

// A host that can only resolve inside the LAN: a private-IPv4 literal, an
// IPv6 literal of the board's own (in brackets, as a URL carries it), a
// `.local` mDNS name, or localhost.
inline bool lanHostIsLocal(const String& host) {
  if (host.length() == 0) return false;
  if (host.equalsIgnoreCase("localhost")) return true;
  if (host[0] == '[') {
    return host.endsWith("]") && lanIpv6Local(host.substring(1, host.length() - 1));
  }
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
  // An IPv6 literal keeps its brackets, and its colons are its own.
  int cut = host.startsWith("[") ? host.indexOf(']') + 1 : host.indexOf(':');
  if (cut <= 0) cut = host.indexOf('/');
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
