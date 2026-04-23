#include "web_server.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>

#include "web_log.h"
#include "wifi_setup.h"

namespace {

AsyncWebServer g_server(80);

constexpr const char kIndexHtml[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Split-Flap Master</title>
  <style>
    body { font-family: system-ui, sans-serif; max-width: 40rem; margin: 2rem auto; padding: 0 1rem; color: #111; }
    h1 { font-size: 1.5rem; border-bottom: 1px solid #ccc; padding-bottom: 0.5rem; }
    h2 { font-size: 1.1rem; margin-top: 1.5rem; }
    table { border-collapse: collapse; width: 100%; }
    th, td { text-align: left; padding: 0.3rem 0.6rem; border-bottom: 1px solid #eee; }
    th { width: 12rem; color: #555; font-weight: normal; }
    form { margin-top: 0.5rem; }
    label { display: block; margin-top: 0.5rem; font-size: 0.9rem; color: #555; }
    input { font: inherit; padding: 0.4rem; width: 100%; box-sizing: border-box; }
    button { font: inherit; padding: 0.5rem 1rem; margin-top: 0.5rem; cursor: pointer; }
    pre { background: #111; color: #0f0; padding: 0.75rem; overflow: auto; max-height: 24rem; font-size: 0.85rem; }
  </style>
</head>
<body>
  <h1>Split-Flap Master (ESP32-S3)</h1>

  <h2>Status</h2>
  <table id="status"><tbody></tbody></table>

  <h2>WiFi</h2>
  <form method="POST" action="/wifi">
    <label>SSID <input name="ssid" required></label>
    <label>Password <input name="password" type="password"></label>
    <button>Save &amp; reboot</button>
  </form>

  <h2>Device name</h2>
  <form method="POST" action="/name">
    <label>Name <input name="name" required></label>
    <button>Save</button>
  </form>

  <h2>Reboot</h2>
  <form method="POST" action="/reboot"><button>Reboot now</button></form>

  <h2>Log</h2>
  <p><button type="button" id="refreshLog">Refresh</button></p>
  <pre id="log">(loading...)</pre>

  <script>
    async function loadStatus() {
      const r = await fetch('/settings');
      if (!r.ok) return;
      const s = await r.json();
      const tbody = document.querySelector('#status tbody');
      while (tbody.firstChild) tbody.removeChild(tbody.firstChild);
      for (const [k, v] of Object.entries(s)) {
        const tr = document.createElement('tr');
        const th = document.createElement('th'); th.textContent = k;
        const td = document.createElement('td'); td.textContent = String(v);
        tr.appendChild(th); tr.appendChild(td);
        tbody.appendChild(tr);
      }
    }
    async function loadLog() {
      const r = await fetch('/log');
      if (!r.ok) return;
      document.getElementById('log').textContent = await r.text();
    }
    document.getElementById('refreshLog').addEventListener('click', loadLog);
    loadStatus();
    loadLog();
    setInterval(loadStatus, 5000);
  </script>
</body>
</html>)HTML";

String escapeJson(const String& s) {
  String out;
  out.reserve(s.length() + 2);
  for (size_t i = 0; i < s.length(); i++) {
    const char c = s[i];
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

const char* resetReasonString() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "other_wdt";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    case ESP_RST_UNKNOWN:
    default:                return "unknown";
  }
}

String buildSettingsJson() {
  String j = "{";
  j += "\"deviceName\":\"";      j += escapeJson(wifiSetup::deviceName()); j += "\",";
  j += "\"mode\":\"";            j += (wifiSetup::isApMode() ? "ap" : "sta"); j += "\",";
  j += "\"ssid\":\"";            j += escapeJson(wifiSetup::ssid());       j += "\",";
  j += "\"ip\":\"";              j += wifiSetup::currentIP().toString();   j += "\",";
  j += "\"sketchMd5\":\"";       j += ESP.getSketchMD5();                  j += "\",";
  j += "\"lastResetReason\":\""; j += resetReasonString();                 j += "\",";
  j += "\"freeHeapBytes\":";     j += String(ESP.getFreeHeap());           j += ",";
  j += "\"uptimeMs\":";          j += String(millis());                    j += ",";
  j += "\"chipRevision\":";      j += String(ESP.getChipRevision());       j += ",";
  j += "\"cpuFreqMhz\":";        j += String(ESP.getCpuFreqMHz());
  j += "}";
  return j;
}

void setupRoutes() {
  g_server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html; charset=utf-8", kIndexHtml);
  });

  g_server.on("/settings", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "application/json", buildSettingsJson());
  });

  g_server.on("/log", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "text/plain; charset=utf-8", webLogRead());
  });

  g_server.on("/reboot", HTTP_POST, [](AsyncWebServerRequest* req) {
    req->send(200, "text/plain", "rebooting...\n");
    wifiSetup::markRebootPending();
  });

  g_server.on("/wifi", HTTP_POST, [](AsyncWebServerRequest* req) {
    const String ssid = req->hasParam("ssid", true)
                          ? req->getParam("ssid", true)->value() : String();
    const String pass = req->hasParam("password", true)
                          ? req->getParam("password", true)->value() : String();
    if (ssid.isEmpty()) {
      req->send(400, "text/plain", "ssid required\n");
      return;
    }
    wifiSetup::saveCredentials(ssid, pass);
    req->send(200, "text/plain", "saved — rebooting...\n");
  });

  g_server.on("/name", HTTP_POST, [](AsyncWebServerRequest* req) {
    const String name = req->hasParam("name", true)
                          ? req->getParam("name", true)->value() : String();
    if (name.isEmpty()) {
      req->send(400, "text/plain", "name required\n");
      return;
    }
    wifiSetup::saveDeviceName(name);
    req->send(200, "text/plain", "saved\n");
  });

  g_server.onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "text/plain", "not found\n");
  });
}

}  // namespace

namespace webServer {

void begin() {
  setupRoutes();
  g_server.begin();
  Serial.println("[http] listening on :80");
}

}  // namespace webServer
