// System/diagnostics read endpoints — split from WebEndpoints.cpp (#338);
// async-context rules in WebEndpoints.cpp's header. Everything here is
// read-only against snapshots/services except /log/flash/clear, which stages
// for netTask's flashLogTick (handlers never write flash).

#include "WebEndpoints.h"
#include "WebEndpointsInternal.h"

#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

#include <atomic>
#include <memory>

#include <esp_app_desc.h>   // #416: running-image ELF sha for dump dating
#include <esp_core_dump.h>  // #319: /coredump crash diagnostics
#include <esp_partition.h>

#include "ApiIndex.h"
#include "BuildVersion.h"
#include "ClusterDigest.h"  // clusterStatusJson (/status aggregate)
#include "ClusterLeader.h"
#include "FactorySlot.h"
#include "FlashLog.h"
#include "OtaService.h"
#include "SettingsJson.h"  // appendJsonString
#include "SplitFlapProtocol.h"
#include "SystemStats.h"
#include "SystemStatsPolicy.h"
#include "Tasks.h"
#include "UnitBus.h"
#include "WearPolicy.h"
#include "WebLog.h"

#include "HelpersSerialHandling.h"

// #431: /coredump/erase handler stages here; webSystemCoredumpEraseTick()
// (netTask, via webEndpointsLoop) performs the actual flash erase.
static std::atomic<bool> coredumpErasePending{false};
// Open /coredump/raw chunked streams (async task; decremented onDisconnect).
// The erase tick defers while one is open, or a slow download would get a
// silent 0xFF tail read from under the ~1 s 64 KB erase.
static std::atomic<int> coredumpRawStreamsActive{0};

void webSystemRegister(AsyncWebServer& server) {
  server.on("/health", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "text/plain", "Healthy");
  });

  server.on("/log", HTTP_GET, [](AsyncWebServerRequest* request) {
    // Don't SerialPrintln here; every log request would otherwise stamp
    // itself into the buffer on every poll and drown out real activity.
    request->send(200, "text/plain", webLogRead());
  });

  // --- persistent flash log (#206) ------------------------------------------
  // Serves the LittleFS log files directly (chunked by the async layer;
  // esp_littlefs serializes fs access internally, and the flush path
  // open→append→closes so no write handle is ever shared). Known-benign
  // race: between the exists() check and the response's real open(), a
  // rotation or drained clear can remove the file — AsyncFileResponse then
  // degrades to a 404, so a request racing a rotate occasionally 404s;
  // retry. ?prev=1 = the rotated file. Content up to the last ~5 s flush.
  server.on("/log/flash", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (!flashLogAvailable()) {
      request->send(503, "text/plain",
                    F("Flash log unavailable (storage mount failed)"));
      return;
    }
    const char* path = request->hasParam("prev") ? flashLogPreviousPath()
                                                 : flashLogCurrentPath();
    if (!LittleFS.exists(path)) {
      request->send(404, "text/plain", F("No flash log yet"));
      return;
    }
    request->send(LittleFS, path, "text/plain");
  });

  // Clear is staged and drained by netTask's flashLogTick — handlers never
  // write flash (async rule).
  server.on("/log/flash/clear", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!flashLogAvailable()) {
      request->send(503, "text/plain",
                    F("Flash log unavailable (storage mount failed)"));
      return;
    }
    flashLogRequestClear();
    request->send(202, "text/plain", F("Flash log clear queued"));
  });

  // --- coredump (#319/#431) — remote crash diagnostics ---------------------
  // The SUMMARY (task name + code addresses + backtrace PCs) carries NO
  // secrets, so an unauthenticated GET is fine — same posture as /settings,
  // /log/flash. The RAW image is a task-stack dump that CAN transiently hold
  // HMAC key material / WiFi-cred fragments (a key mid-sign on clusterTask's
  // stack); #431 ships it anyway — accepted risk for this internal LAN-only
  // deployment, where a dump that cannot be pulled costs more than the
  // exposure (the surface stays CSRF/CORS-closed like every other route).
  // The partition is written by the panic handler and erased only via the
  // staged /coredump/erase drain below (netTask, like every flash write).
  server.on("/coredump/summary", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (esp_core_dump_image_check() != ESP_OK) {
      request->send(404, "application/json", F("{\"present\":false}"));
      return;
    }
    // Zero-init: if a partially-corrupted-but-parseable dump leaves a field
    // untouched, we echo a deterministic 0, not stale heap bytes.
    auto* summary =
        (esp_core_dump_summary_t*)calloc(1, sizeof(esp_core_dump_summary_t));
    if (summary == nullptr) {
      request->send(503, "application/json", F("{\"error\":\"oom\"}"));
      return;
    }
    if (esp_core_dump_get_summary(summary) != ESP_OK) {
      free(summary);
      request->send(500, "application/json",
                    F("{\"present\":true,\"error\":\"summary-failed\"}"));
      return;
    }
    char task[17];
    memcpy(task, summary->exc_task, 16);
    task[16] = '\0';
    // #416: `rev` is the RUNNING image's compile-time GIT_REV — it says
    // nothing about which build crashed (a fossil dump survives every OTA).
    // The truncated ELF SHA pair is what actually dates a dump: `stale`
    // is only claimed when the dump carries a sha and it differs.
    char dumpSha[APP_ELF_SHA256_SZ];
    memcpy(dumpSha, summary->app_elf_sha256, sizeof(dumpSha));
    dumpSha[sizeof(dumpSha) - 1] = '\0';
    char runSha[APP_ELF_SHA256_SZ];
    esp_app_get_elf_sha256(runSha, sizeof(runSha));
    bool stale =
        dumpSha[0] != '\0' && strncmp(dumpSha, runSha, strlen(dumpSha)) != 0;
    char hex[11];
    String out;
    out.reserve(768);
    out += F("{\"present\":true,\"rev\":\"" GIT_REV "\",\"elfSha256\":");
    appendJsonString(out, String(dumpSha));
    out += F(",\"runningElfSha256\":");
    appendJsonString(out, String(runSha));
    out += F(",\"stale\":");
    out += stale ? F("true") : F("false");
    out += F(",\"task\":");
    appendJsonString(out, String(task));
    snprintf(hex, sizeof(hex), "0x%08x", (unsigned)summary->exc_pc);
    out += F(",\"pc\":\"");
    out += hex;
    out += F("\",\"excCause\":");
    out += (unsigned)summary->ex_info.exc_cause;
    snprintf(hex, sizeof(hex), "0x%08x", (unsigned)summary->ex_info.exc_vaddr);
    out += F(",\"excVaddr\":\"");
    out += hex;
    out += F("\",\"coreDumpVersion\":");
    out += (unsigned)summary->core_dump_version;
    out += F(",\"backtraceCorrupted\":");
    out += summary->exc_bt_info.corrupted ? F("true") : F("false");
    out += F(",\"backtrace\":[");
    uint32_t depth = summary->exc_bt_info.depth;
    if (depth > 16) depth = 16;
    for (uint32_t i = 0; i < depth; i++) {
      if (i) out += ',';
      snprintf(hex, sizeof(hex), "0x%08x", (unsigned)summary->exc_bt_info.bt[i]);
      out += '"';
      out += hex;
      out += '"';
    }
    out += F("]}");
    free(summary);
    request->send(200, "application/json", out);
  });

  // Raw ELF coredump for offline `esp-coredump info_corefile` — the fallback
  // when a summary backtrace reads corrupted. Streamed straight off the
  // coredump partition (flash READ only, async-safe). Always registered
  // since #431 (see the posture note above).
  server.on("/coredump/raw", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (esp_core_dump_image_check() != ESP_OK) {
      request->send(404, "text/plain", F("no coredump"));
      return;
    }
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) {
      request->send(500, "text/plain", F("coredump image unavailable"));
      return;
    }
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
    if (part == nullptr) {
      request->send(500, "text/plain", F("no coredump partition"));
      return;
    }
    // image_get() returns an ABSOLUTE flash address — translate to a
    // partition-relative offset for esp_partition_read.
    size_t offset = addr - part->address;
    AsyncWebServerResponse* response = request->beginChunkedResponse(
        "application/octet-stream",
        [part, offset, size](uint8_t* buffer, size_t maxLen,
                             size_t index) -> size_t {
          if (index >= size) return 0;
          size_t remaining = size - index;
          size_t toRead = remaining < maxLen ? remaining : maxLen;
          if (esp_partition_read(part, offset + index, buffer, toRead) !=
              ESP_OK) {
            return 0;  // abort the stream
          }
          return toRead;
        });
    response->addHeader("Content-Disposition",
                        "attachment; filename=\"coredump-" GIT_REV ".elf\"");
    coredumpRawStreamsActive.fetch_add(1, std::memory_order_relaxed);
    request->onDisconnect([]() {
      coredumpRawStreamsActive.fetch_sub(1, std::memory_order_relaxed);
    });
    request->send(response);
  });

  // #431: purge the partition so the NEXT dump is unambiguous. Before this
  // route a dump could only ever be replaced by the next panic, and the
  // fossils muddied every later forensics pass (both wall masters carried
  // one on 2026-08-05). Handlers never write flash: stage the request and
  // let netTask's drain run the erase — same pattern as /log/flash/clear.
  server.on("/coredump/erase", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (esp_core_dump_image_check() != ESP_OK) {
      request->send(404, "text/plain", F("no coredump"));
      return;
    }
    coredumpErasePending.store(true, std::memory_order_relaxed);
    request->send(202, "text/plain",
                  F("Coredump erase queued — /coredump/summary reports "
                    "present:false once it lands"));
  });

  // System tab (#245): current vitals + ~10 min sparkline history in one
  // JSON. History is server-side (netTask's sample ring) so a freshly
  // opened tab has depth immediately; the browser polls at 2 s.
  server.on("/system/stats", HTTP_GET, [](AsyncWebServerRequest* request) {
    std::unique_ptr<char[]> buf(new char[SYSTEM_STATS_JSON_CAP]);
    size_t n = systemStatsJson(buf.get(), SYSTEM_STATS_JSON_CAP);
    if (n == 0 || n >= SYSTEM_STATS_JSON_CAP) {
      request->send(500, "text/plain", F("stats unavailable"));
      return;
    }
    request->send(200, "application/json", buf.get());
  });

  // Self-documenting route + terse-key legend index for the headless
  // (curl-only) operator (#307). Static data, heap-rendered per request.
  server.on("/api", HTTP_GET, [](AsyncWebServerRequest* request) {
    std::unique_ptr<char[]> buf(new char[API_JSON_CAP]);
    size_t n = buildApiJson(buf.get(), API_JSON_CAP);
    if (n == 0 || n >= API_JSON_CAP) {
      request->send(500, "text/plain", F("api index unavailable"));
      return;
    }
    request->send(200, "application/json", buf.get());
  });

  // Static hardware/partition inventory (#307). Reuses existing accessors; no
  // new sampling. bootCount comes from the NVS counter main.cpp bumps.
  server.on("/system/info", HTTP_GET, [](AsyncWebServerRequest* request) {
    String out;
    out.reserve(1024);
    out += "{\"chip\":\"";
    out += ESP.getChipModel();
    out += "\",\"chipRev\":";
    out += String(ESP.getChipRevision());
    out += ",\"cores\":";
    out += String(ESP.getChipCores());
    out += ",\"cpuMHz\":";
    out += String(ESP.getCpuFreqMHz());
    out += ",\"flashKB\":";
    out += String(ESP.getFlashChipSize() / 1024);
    out += ",\"psramKB\":";
    out += String(ESP.getPsramSize() / 1024);
    out += ",\"rev\":\"";
    out += GIT_REV;
    out += "\",\"bundledUnitRev\":\"";
    out += BUNDLED_UNIT_REV;
    out += "\",\"sketchMd5\":\"";
    out += ESP.getSketchMD5();
    out += "\",\"bootCount\":";
    out += String(liveStore != nullptr ? liveStore->getInt("bootCount", 0) : 0);
    out += ",\"resetReason\":\"";
    out += webResetReasonString();
    out += "\",\"factoryPresent\":";
    out += factorySlotPresent() ? "true" : "false";
    out += ",\"rescueValid\":";  // factory slot holds a bootable rescue image
    out += factorySlotImageValid() ? "true" : "false";
    out += ",\"partitions\":[";
    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    bool first = true;
    while (it != nullptr) {
      const esp_partition_t* p = esp_partition_get(it);
      if (!first) out += ',';
      first = false;
      out += "{\"label\":\"";
      out += p->label;
      out += "\",\"type\":";
      out += String(p->type);
      out += ",\"subtype\":";
      out += String(p->subtype);
      out += ",\"offset\":";
      out += String((unsigned long)p->address);
      out += ",\"size\":";
      out += String((unsigned long)p->size);
      out += '}';
      it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);
    out += "]}";
    request->send(200, "application/json", out);
  });

  // One-shot aggregate for a single curl (#307): settings + stats.now + units
  // + cluster + ota, composed from the existing serializers. History stays at
  // /system/stats to keep this bounded.
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest* request) {
    std::unique_ptr<char[]> nowBuf(new char[SYSTEM_STATS_JSON_CAP]);
    size_t nowN = systemStatsNowJson(nowBuf.get(), SYSTEM_STATS_JSON_CAP);
    if (nowN >= SYSTEM_STATS_JSON_CAP) nowBuf[0] = '\0';

    DisplaySnapshot snap = displaySnapshotGet();
    std::unique_ptr<char[]> unitsBuf(new char[UNIT_HEALTH_JSON_CAP]);
    size_t unitsN =
        buildUnitHealthJson(unitsBuf.get(), UNIT_HEALTH_JSON_CAP, snap.units,
                            snap.displayWidth, snap.faultyUnitCount,
                            SFP_I2C_ADDRESS_BASE, millis());
    if (unitsN == 0 || unitsN >= UNIT_HEALTH_JSON_CAP) {
      snprintf(unitsBuf.get(), UNIT_HEALTH_JSON_CAP,
               "{\"width\":%d,\"faulty\":%d,\"units\":[]}", snap.displayWidth,
               snap.faultyUnitCount);
    }

    String out;
    out.reserve(6144);
    out += "{\"settings\":";
    out += buildCurrentSettingsJson();
    out += ",\"stats\":{\"now\":";
    out += nowBuf.get();
    out += "},\"units\":";
    out += unitsBuf.get();
    out += ",\"cluster\":";
    out += clusterStatusJson(clusterLeaderStatusGet());
    out += ",\"ota\":";
    out += otaDebugJson();
    out += "}";
    request->send(200, "application/json", out);
  });
}

// #431: drained from webEndpointsLoop — netTask is the sole flash writer,
// so the erase (a few sector erases on the 64 KB coredump partition) never
// runs in async-handler context.
void webSystemCoredumpEraseTick() {
  if (!coredumpErasePending.load(std::memory_order_relaxed)) return;
  // A raw stream opened after this check (or a summary read mid-handler)
  // can still overlap the erase by a hair — each partition op is
  // driver-locked, so the worst case is one corrupt/`0xFF`-tailed response,
  // accepted for a two-humans-in-one-second window.
  if (coredumpRawStreamsActive.load(std::memory_order_relaxed) > 0) return;
  coredumpErasePending.store(false, std::memory_order_relaxed);
  esp_err_t err = esp_core_dump_image_erase();
  SerialPrintf("coredump: erase -> %s\n", esp_err_to_name(err));
}
