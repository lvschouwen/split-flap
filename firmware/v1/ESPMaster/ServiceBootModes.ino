// Boot-mode + device-identity bring-up, extracted from ESPMaster.ino (#148).
// One translation unit: every type/include/global from ESPMaster.ino (which
// sorts earlier in the alphabetical .ino concatenation) is visible here.
// The RTC-cookie helpers (readBootStateRtc etc.) stay in ESPMaster.ino because
// their RtcBootState return type trips the auto-prototype-before-include gotcha;
// they precede this file in concat order, so calls below resolve fine.
// Prototypes for the functions here live in ESPMaster.h (called from setup()).

//Registers POST /firmware/master. Shared between main mode and recovery mode
//so the upload path (MD5 verify, size check, gated reboot) is identical.
//In recovery mode we skip the I2C unit-reboot hook since the bus may be
//unreachable — that's precisely why the device is in recovery.
void registerMasterFirmwareEndpoint() {
  //MD5 helper (SparkMD5, data/md5.js) registered alongside the upload
  //endpoint so every environment that can flash can also hash (#160):
  //?md5= is mandatory (#144) and browsers have no native MD5.
  webServer.on("/md5.js", HTTP_GET, [](AsyncWebServerRequest * request) {
    AsyncWebServerResponse *resp = request->beginResponse_P(200, "application/javascript", MD5_JS_GZ, MD5_JS_GZ_LEN);
    resp->addHeader("Content-Encoding", "gzip");
    //Same no-cache policy as every PROGMEM asset: an OTA that re-vendors
    //the library must not leave stale JS in the tab cache.
    resp->addHeader("Cache-Control", "no-cache");
    request->send(resp);
  });
  webServer.on("/firmware/master", HTTP_POST,
    [](AsyncWebServerRequest * request) {
      //Restore TX power on every exit path. Upload handler dropped it for
      //the write storm (#60). On the success path we reboot anyway, so
      //this is moot there, but restoring before we send the 200 gives the
      //response the best chance of reaching the client.
      if (otaTxPowerReduced) {
        WiFi.setOutputPower(DEFAULT_TX_POWER_DBM);
        otaTxPowerReduced = false;
      }
      if (otaRejected) {
        int status = otaRejectionStatus;
        String reason = otaRejectionReason;
        otaRejected = false;
        otaRejectionStatus = 0;
        otaRejectionReason = String();
        masterOtaUploadActive = false;  //unfreeze the display (#116)
        mqttResumeAfterOta();
        request->send(status, "text/plain", reason);
        return;
      }
      if (Update.hasError()) {
        String msg = String("Master OTA failed: ") + Update.getErrorString();
        SerialPrintln(msg);
        masterOtaUploadActive = false;  //unfreeze the display (#116)
        mqttResumeAfterOta();
        request->send(500, "text/plain", msg);
      } else if (!Update.isFinished()) {
        String msg = "Master OTA incomplete: Update.isFinished() == false after final chunk";
        SerialPrintln(msg);
        masterOtaUploadActive = false;  //unfreeze the display (#116)
        mqttResumeAfterOta();
        request->send(500, "text/plain", msg);
      } else {
        //Single source of truth: reboot only when the updater considers the
        //image committed AND no error was latched along the way.
        request->send(200, "text/plain", "Master firmware flashed; rebooting…");
        //Stash the flash-outcome cookie in RTC (#53/#118) using the uploaded
        //image's MD5. ?md5= is mandatory now (#144), and this success branch
        //is only reached when the upload handler accepted it, so the param is
        //guaranteed present: the next boot reports "ok" iff the running MD5
        //matches it — correct even when re-flashing the identical image.
        String expectedMd5 = request->getParam("md5")->value();
        expectedMd5.toLowerCase();
        setFlashCookieRtc(expectedMd5, COOKIE_KIND_EXPECTED_MD5);
        //Clear the previous verdict NOW (#118) — if the next boot can't
        //adjudicate (e.g. the cookie was invalidated by an RtcBootState
        //magic bump), /settings must show "" (unknown), not a stale
        //verdict from an earlier flash masquerading as this one's.
        saveLastFlashResult("");
        //A fresh firmware should start with a clean boot counter — otherwise
        //the very first post-flash boot could tip us back into recovery.
        clearBootCounterRtc();
        isPendingReboot = true;
      }
    },
    [](AsyncWebServerRequest * request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      masterOtaLastChunkMs = millis();
      if (index == 0) {
        //Freeze the display for the duration of the upload (issue #116).
        //The loop unfreezes on the completion handler's failure paths or
        //after 30 s without a chunk; on success the device reboots frozen.
        masterOtaUploadActive = true;
        mqttStopForOta();  //publish retained offline + disconnect (#121)
        otaRejected = false;
        otaRejectionStatus = 0;
        otaRejectionReason = String();

        //Reduce WiFi TX before we start writing flash (#60). Restored in
        //the request handler on every exit path.
        WiFi.setOutputPower(OTA_TX_POWER_DBM);
        otaTxPowerReduced = true;
        SerialPrint(F("WiFi TX reduced to "));
        SerialPrint(OTA_TX_POWER_DBM);
        SerialPrintln(F(" dBm for OTA upload"));

        uint32_t freeSpace = ESP.getFreeSketchSpace();
        uint32_t maxSketchSpace = (freeSpace - 0x1000) & 0xFFFFF000;
        size_t contentLen = request->contentLength();
        SerialPrint(F("Master OTA starting: ")); SerialPrintln(filename);
        SerialPrint(F("  freeSketchSpace = ")); SerialPrintln(freeSpace);
        SerialPrint(F("  maxSketchSpace  = ")); SerialPrintln(maxSketchSpace);
        SerialPrint(F("  contentLength   = ")); SerialPrintln(contentLen);

        if (contentLen > 0 && contentLen > maxSketchSpace) {
          otaRejected = true;
          otaRejectionStatus = 413;
          otaRejectionReason = String("Firmware too large: ") + contentLen +
                               " bytes > maxSketchSpace " + maxSketchSpace;
          SerialPrintln(otaRejectionReason);
          return;
        }

        //Flash-config mismatch (#92/#94): if the RUNNING image's flash-size
        //header claims more than the physical chip, Update.begin() will
        //refuse every upload with the cryptic "Flash config wrong". Detect
        //it first and tell the operator to USB-reflash the 1 MB build,
        //which is the only thing that breaks the deadlock (nothing over
        //the air can). Fires on legacy images built with a bigger header.
        uint32_t flashRealSize   = ESP.getFlashChipRealSize();
        uint32_t flashHeaderSize = ESP.getFlashChipSize();
        if (flashRealSize < flashHeaderSize) {
          otaRejected = true;
          otaRejectionStatus = 412;
          otaRejectionReason = String("Flash config mismatch: running firmware header claims ") +
                               flashHeaderSize + " bytes but chip is " + flashRealSize +
                               " — OTA is permanently rejected by this build; reflash once over USB with the current firmware build";
          SerialPrintln(otaRejectionReason);
          return;
        }

        Update.runAsync(true);
        if (!Update.begin(maxSketchSpace, U_FLASH)) {
          //Usual cause: stale updater state from a previous failed or
          //aborted upload — begin() refuses re-entry while its _size is
          //still set, without latching an error (#162). end(false) takes
          //the premature-end path (which does reset), then retry once so
          //one bad attempt doesn't poison this one.
          Update.end(false);
          Update.clearError();
          if (!Update.begin(maxSketchSpace, U_FLASH)) {
            otaRejected = true;
            otaRejectionStatus = 500;
            otaRejectionReason = String("Update.begin failed: ") + Update.getErrorString();
            SerialPrintln(otaRejectionReason);
            return;
          }
        }

        //MD5 is MANDATORY (#144). eboot's built-in checksum only catches the
        //staged image getting corrupted in flash — it does NOT catch a
        //truncated/partially-uploaded image landing on top of a trusted boot.
        //Reject any upload that doesn't carry a valid 32-hex ?md5= digest.
        if (!request->hasParam("md5")) {
          otaRejected = true;
          otaRejectionStatus = 400;
          otaRejectionReason = "md5 query param is required";
          SerialPrintln(otaRejectionReason);
          Update.end(false);
          return;
        }
        String md5 = request->getParam("md5")->value();
        md5.toLowerCase();
        if (md5.length() != 32) {
          otaRejected = true;
          otaRejectionStatus = 400;
          otaRejectionReason = "md5 query param must be a 32-char hex digest";
          SerialPrintln(otaRejectionReason);
          Update.end(false);
          return;
        }
        if (!Update.setMD5(md5.c_str())) {
          otaRejected = true;
          otaRejectionStatus = 400;
          otaRejectionReason = "Update.setMD5 rejected '" + md5 + "'";
          SerialPrintln(otaRejectionReason);
          Update.end(false);
          return;
        }
        SerialPrint(F("MD5 expected: ")); SerialPrintln(md5);
        //Record the caller-supplied intended version so we can detect a
        //silent revert on the next boot. Write unconditionally (empty if
        //the client didn't pass ?v=) so stale values from an earlier flash
        //don't linger and cause a false-positive "OTA REVERTED". See #52.
        String intended;
        if (request->hasParam("v")) {
          intended = request->getParam("v")->value();
        }
        saveIntendedVersion(intended);
        SerialPrint(F("Intended version recorded: \""));
        SerialPrint(intended);
        SerialPrintln(F("\""));
        SerialPrintln(F("Update.begin ok — streaming chunks"));
      }
      if (otaRejected) return;
      if (!Update.hasError() && len > 0) {
        size_t written = Update.write(data, len);
        if (written != len) {
          SerialPrint(F("Update.write short: wrote ")); SerialPrint(written);
          SerialPrint(F(" of ")); SerialPrint(len);
          SerialPrint(F(" — err: ")); SerialPrintln(Update.getErrorString());
        }
      }
      if (final) {
        SerialPrint(F("Final chunk: total ")); SerialPrint(index + len); SerialPrintln(F(" bytes"));
        if (Update.end(true)) {
          SerialPrint(F("Master OTA complete, "));
          SerialPrint(index + len);
          SerialPrintln(F(" bytes written"));
          //Units are deliberately NOT pushed into their bootloaders here
          //(issue #114). The old shotgun predated version detection and
          //re-flashed every unit on every master OTA even when the bundled
          //unit rev didn't change. The new master's boot-time
          //autoUpdateOutdatedUnits() compares each unit's reported rev
          //against its bundle and flashes exactly the mismatched ones.
        } else {
          SerialPrint(F("Update.end failed: "));
          SerialPrintln(Update.getErrorString());
          //The core's md5-mismatch branch of end() latches the error but
          //skips _reset(), leaving _size > 0 — the next begin() would then
          //refuse re-entry and report THIS attempt's error against the
          //retry (#162). A second end(false) takes the premature-end path,
          //which resets the size state while leaving the latched error
          //intact for the completion handler's 500 below.
          Update.end(false);
        }
      }
    }
  );
}

//Shared SoftAP fallback for recovery / quiet-OTA (#126). persistent(false)
//BEFORE disconnect(): on the ESP8266 core a persistent disconnect() zeroes
//the stored station config — the primary credential store.
void startFallbackSoftAp(const String& apSuffix) {
  WiFi.persistent(false);
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  WiFi.softAP((effectiveDeviceName + apSuffix).c_str());
  SerialPrint(F("Fallback SoftAP IP: "));
  SerialPrintln(WiFi.softAPIP().toString());
}

//Shared upload form for the minimal recovery/quiet-OTA pages. A plain HTML
//form can't flash any more — ?md5= is mandatory on /firmware/master (#144) —
//so this snippet hashes the file via /md5.js and uploads with XHR, attaching
//the digest plus ?v= when the filename carries a build-stamped rev (#160).
//KEEP IN SYNC: the filename regex duplicates script.js's
//firmwareVersionParam() (these pages can't load script.js; note the extra
//C-string escaping: \\. here is \. in the JS).
static const char MINIMAL_UPLOAD_FORM[] PROGMEM =
  "<form id='fwForm'>"
  "<p><input type='file' id='fwFile' accept='.bin' required/></p>"
  "<p><button type='submit' id='fwBtn'>Flash firmware</button></p>"
  "<p id='fwStatus'></p>"
  "</form>"
  "<script src='/md5.js'></script>"
  "<script>"
  "document.getElementById('fwForm').addEventListener('submit',function(e){"
  "e.preventDefault();"
  "var fi=document.getElementById('fwFile');var f=fi.files[0];if(!f)return;"
  "var btn=document.getElementById('fwBtn'),st=document.getElementById('fwStatus');"
  "btn.disabled=true;fi.disabled=true;st.textContent='Computing MD5...';"
  "var r=new FileReader();"
  "r.onerror=function(){btn.disabled=false;fi.disabled=false;st.textContent='Could not read the selected file.';};"
  "r.onload=function(){"
  "var m=f.name.match(/^firmware-([0-9a-f]{7,40}(?:-dirty)?)(?:-[0-9]+m[0-9]*m?)?\\.bin$/i);"
  "var url='/firmware/master?md5='+SparkMD5.ArrayBuffer.hash(r.result)+(m?'&v='+encodeURIComponent(m[1]):'');"
  "st.textContent='Uploading...';"
  "var fd=new FormData();fd.append('firmware',f);"
  "var x=new XMLHttpRequest();x.open('POST',url);"
  "x.onreadystatechange=function(){if(x.readyState!==4)return;btn.disabled=false;fi.disabled=false;"
  "st.textContent=x.status===200?x.responseText:"
  "(x.status===0?'Upload failed - lost connection.':'HTTP '+x.status+': '+x.responseText);};"
  "x.send(fd);};"
  "r.readAsArrayBuffer(f);});"
  "</script>";

//Minimal recovery mode: serves a single upload form and the OTA endpoint.
//No I2C traffic, no persistence — just enough to let the user reflash a
//working image without dragging out the USB cable. We first try the
//SDK-persisted WiFi so the device reappears on its familiar LAN IP and the
//remote flasher doesn't have to switch SSIDs (#53); SoftAP is the fallback.
//Deliberately NO setup portal here (#126) — recovery stays a minimal
//upload-only environment.
void enterRecoveryMode() {
  SerialPrintln(F("Recovery: attempting known WiFi first..."));
  if (tryJoinKnownWifi(30)) {
    isWifiConfigured = true;
    SerialPrint(F("Recovery on LAN IP: "));
    SerialPrintln(WiFi.localIP().toString());
  } else {
    SerialPrintln(F("Recovery: WiFi unavailable — falling back to SoftAP"));
    startFallbackSoftAp(AP_SUFFIX_RECOVERY);
  }

  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
    String html =
      "<!doctype html><html><head><title>Split-Flap Recovery</title>"
      "<meta name='viewport' content='width=device-width, initial-scale=1'/>"
      "<style>body{font-family:sans-serif;max-width:480px;margin:2em auto;padding:1em}"
      "button{padding:.5em 1em}</style></head><body>"
      "<h1>Split-Flap Recovery</h1>"
      "<p>Device entered recovery after repeated failed boots. "
      "Upload a known-good <code>firmware.bin</code> to recover.</p>";
    html += FPSTR(MINIMAL_UPLOAD_FORM);
    html += "</body></html>";
    request->send(200, "text/html", html);
  });
  //EEPROM is already up (initialiseFileSystem() runs at the top of setup(),
  //before the recovery dispatch) — required before /firmware/master is
  //served: the upload handler persists intendedVersion and stages
  //lastFlashResult="", and without EEPROM.begin() both silently no-op (#119).
  registerMasterFirmwareEndpoint();
  webServer.begin();
  SerialPrintln(F("Recovery web server ready"));
}

//Minimal quiet-flash environment (issue #117). Joins WiFi and serves ONLY
//the upload form, /firmware/master, /settings (so ota-master.sh's verdict
//polling works unchanged) and an exit endpoint. No I2C probe, no NTP wait,
//no display writes — the supply stays quiet for the whole upload and the
//eboot copy that follows. Units keep showing whatever they last displayed.
void enterOtaMode() {
  SerialPrintln(F("OTA mode: joining known WiFi..."));
  if (tryJoinKnownWifi(30)) {
    isWifiConfigured = true;
    SerialPrint(F("OTA mode on LAN IP: "));
    SerialPrintln(WiFi.localIP().toString());
  } else {
    SerialPrintln(F("OTA mode: WiFi unavailable — falling back to SoftAP"));
    startFallbackSoftAp(AP_SUFFIX_OTA);
  }

  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
    String html =
      "<!doctype html><html><head><title>Split-Flap OTA Mode</title>"
      "<meta name='viewport' content='width=device-width, initial-scale=1'/>"
      "<style>body{font-family:sans-serif;max-width:480px;margin:2em auto;padding:1em}"
      "button{padding:.5em 1em}</style></head><body>"
      "<h1>Split-Flap OTA Mode</h1>"
      "<p>Quiet flash environment — no display or unit activity. "
      "Upload a <code>firmware.bin</code>, or exit to resume normal operation.</p>";
    html += FPSTR(MINIMAL_UPLOAD_FORM);
    html +=
      "<form method='post' action='/firmware/ota-exit'>"
      "<p><button type='submit'>Exit OTA mode</button></p>"
      "</form></body></html>";
    request->send(200, "text/html", html);
  });
  webServer.on("/firmware/ota-exit", HTTP_POST, [](AsyncWebServerRequest * request) {
    SerialPrintln(F("OTA mode exit requested"));
    request->send(200, "text/plain", "Leaving OTA mode; rebooting normally…");
    isPendingReboot = true;  //bootMode already cleared on entry — normal boot
  });
  webServer.on("/settings", HTTP_GET, [](AsyncWebServerRequest * request) {
    String json = getCurrentSettingValues();
    request->send(200, "application/json", json);
  });
  //EEPROM is already up (initialiseFileSystem() runs at the top of setup(),
  //before the OTA-mode dispatch) — required before /firmware/master is
  //served: the upload handler persists intendedVersion and stages
  //lastFlashResult="" (#119). A quiet-mode flash without EEPROM up used to
  //leave a stale intendedVersion behind.
  registerMasterFirmwareEndpoint();
  webServer.begin();
  SerialPrintln(F("OTA-mode web server ready"));
}

//Resolve the per-device network identity (#125). Runs right after
//initialiseFileSystem() at the top of setup() — before the quiet-OTA/
//recovery dispatch (their SoftAP SSIDs need it) and before initWiFi()
//(hostname). The blob is already migrated by then, so the magic/version
//guard is belt-and-suspenders against a corrupt or foreign blob only.
void resolveDeviceIdentity() {
  uint8_t ver = EEPROM.read(OFF_VERSION);
  bool eepromValid = (readSettingMagic() == SETTINGS_MAGIC) &&
                     (ver >= 5) && (ver <= SETTINGS_VERSION);
  String stored = eepromValid ? readSettingString(OFF_DEVICE_NAME, LEN_DEVICE_NAME) : String("");
  effectiveDeviceName = resolveDeviceName(eepromValid, stored, mdnsName, ESP.getChipId());
  SerialPrint(F("Device identity: "));
  SerialPrint(effectiveDeviceName);
  SerialPrintln(stored.length() ? F(" (from EEPROM)") : F(" (chip-id default)"));
}

