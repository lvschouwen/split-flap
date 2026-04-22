// Async HTTP server for the S3. Exposes a minimal dashboard at `/`, plus
// endpoints for `/log`, `/settings`, `/reboot`, `/wifi`, `/name`.
//
// Built on ESP32Async/ESPAsyncWebServer + AsyncTCP (successor to dvarrel's
// ESP8266-focused forks). Independent of WiFi mode — works in both STA and
// AP (provisioning) modes, same routes on both.

#pragma once

namespace webServer {

// Starts the async server on port 80. Call after WiFi is up (either STA
// connected or AP active). Non-blocking; handlers run on their own task
// inside AsyncTCP.
void begin();

}  // namespace webServer
