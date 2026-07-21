#pragma once
// RescueCors.h — CSRF origin gate for the rescue app's mutating POSTs
// (#349). copied: trimmed from firmware/v2/FollowerEsp01/FollowerCors.h
// (origin + CSRF-reject logic only; the path-allow gate is dropped — rescue
// serves no cross-origin surface, it only refuses cross-site POSTs). Copy
// policy: fix bugs in both trees. Natively tested (test/test_rescue_cors).
//
// Allowed origins are the ones that can only exist inside the LAN — private
// IPv4 literals, .local names and localhost, all http-only. That covers the
// captive-portal AP client (http://192.168.4.1) and a LAN browser when
// rescue is STA-joined; no-Origin clients (curl, ota-flash.sh, the page's
// own same-origin fetch) pass untouched. A mutating POST carrying any other
// Origin is cross-site forgery against a flash-capable endpoint — refuse it.

#include <Arduino.h>

inline bool rescueCorsPrivateIpv4(const String& host) {
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

inline bool rescueCorsOriginAllowed(const String& origin) {
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
  return rescueCorsPrivateIpv4(host);
}

inline bool rescueCsrfRejectPost(bool isPost, bool hasOrigin,
                                 const String& origin) {
  return isPost && hasOrigin && !rescueCorsOriginAllowed(origin);
}
