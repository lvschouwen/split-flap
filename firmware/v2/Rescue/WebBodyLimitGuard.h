// WebBodyLimitGuard.h (#347) — target-only glue that enforces WebBodyLimit.h
// on an AsyncWebServer. Split from the pure policy so WebBodyLimit.h stays
// natively testable (this file pulls in ESPAsyncWebServer and is never
// compiled by the native env).
//
// The parser buffers an oversized non-file body into a heap String BEFORE any
// route handler or the CORS/CSRF middleware runs, so the reject must happen at
// handler-match time: canHandle() sees the header-parsed URL, multipart flag
// and Content-Length. isRequestHandlerTrivial()==true makes the parser
// DISCARD the body rather than buffer it — the 413 is sent at end-of-body
// with no heap String ever growing. attachBodyLimitGuard() MUST be called
// before any server.on(...) so the guard wins the first-match-wins scan.
//
// COPIED HEADER — byte-identical across Master / Rescue / FollowerEsp01 (all
// on esp32async/ESPAsyncWebServer, #356). Fix bugs in every tree.
#pragma once

#include <ESPAsyncWebServer.h>

#include "WebBodyLimit.h"

class WebBodyLimitGuard : public AsyncWebHandler {
 public:
  bool canHandle(AsyncWebServerRequest* request) const override {
    return bodyLimitExceeded(request->url().c_str(), request->multipart(),
                             request->contentLength());
  }
  bool isRequestHandlerTrivial() const override { return true; }
  void handleRequest(AsyncWebServerRequest* request) override {
    // Raw 413 with no per-platform CORS headers. On Master the server CORS
    // middleware still decorates it; on FollowerEsp01/Rescue (no middleware)
    // it is bare — acceptable because the guard only trips on >2 KB bodies,
    // and no browser-facing follower/rescue route sends a body that large
    // (its local-UI callers — e.g. the wall's Leave button — are GETs and
    // tiny query-param POSTs; leader-wire traffic is server-to-server with no
    // Origin). So a CORS-less 413 has no realistic victim here.
    request->send(413);
  }
};

// Register the guard as the first handler. The instance is function-local
// static so it outlives the server without a global; a single server per app
// makes one instance sufficient.
inline void attachBodyLimitGuard(AsyncWebServer& server) {
  static WebBodyLimitGuard guard;
  server.addHandler(&guard);
}
