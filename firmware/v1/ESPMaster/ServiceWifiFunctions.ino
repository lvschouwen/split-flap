//WiFi bring-up (#126). Credentials normally live in exactly ONE place: the
//ESP8266 SDK's flash config sector. The setup portal (or any persistent
//WiFi.begin(ssid, pass)) writes them there and a bare WiFi.begin() reads
//them back — surviving reboots and sketch OTAs. A factory-fresh device is
//provisioned by joining the "<deviceName>-setup" portal once. The optional
//WifiCredentials.h migration seed is documented at its include site in
//ESPMaster.ino.

//Polls for WL_CONNECTED after a WiFi.begin() variant has been issued.
static bool waitForWifiConnected(int timeoutSeconds) {
  for (int elapsedSeconds = 0; elapsedSeconds < timeoutSeconds; elapsedSeconds++) {
    if (WiFi.status() == WL_CONNECTED) {
      SerialPrint(F("connected. IP Address: "));
      SerialPrintln(WiFi.localIP());
      return true;
    }
    SerialPrint('.');
    delay(1000);
  }

  SerialPrintln(F(" timed out"));
  return false;
}

//Bounded attempt to join the network whose credentials the SDK has
//persisted. Shared by every boot path — recovery and quiet-OTA mode rely
//on it to reappear on the familiar LAN IP without ever opening a portal.
//Returns true when connected; no stored credentials -> immediate false.
bool tryJoinKnownWifi(int timeoutSeconds) {
  WiFi.mode(WIFI_STA);
  WiFi.hostname(effectiveDeviceName.c_str());
  WiFi.setAutoReconnect(true);

  if (WiFi.SSID().length() == 0) {
    SerialPrintln(F("No WiFi credentials persisted in SDK flash"));
    return false;
  }

  SerialPrint(F("Joining known WiFi \""));
  SerialPrint(WiFi.SSID());
  SerialPrint(F("\" "));
  WiFi.begin();  //no args = reconnect with the SDK-persisted credentials

  return waitForWifiConnected(timeoutSeconds);
}

//Normal-boot WiFi init: known credentials first, then the migration seed
//if one was compiled in, then the captive portal. The portal never runs in
//recovery/quiet-OTA boots (they call tryJoinKnownWifi() directly) so those
//modes stay minimal upload-only environments.
void initWiFi() {
  if (tryJoinKnownWifi(30)) {
    isWifiConfigured = true;
    return;
  }

#if WIFI_SEED_AVAILABLE
  //Migration seed: pre-#126 firmware supplied its compiled credentials
  //with persistence OFF (this core's default), i.e. RAM only — an
  //OTA-upgraded device can land here with an empty or stale SDK sector.
  //Persist the seeded credentials this time; wrap in persistent(true)/
  //(false) exactly like the portal's own save path does.
  if (strlen(wifiDirectSsid) > 0) {
    SerialPrint(F("Trying build-seeded WiFi credentials (persisting) "));
    WiFi.persistent(true);
    WiFi.begin(wifiDirectSsid, wifiDirectPassword);
    WiFi.persistent(false);
    if (waitForWifiConnected(30)) {
      isWifiConfigured = true;
      return;
    }
  }
#endif

  SerialPrintln(F("Starting WiFi setup portal..."));

  //Waiting in the portal is a deliberate parked state, not a crash loop —
  //clear the RTC boot counter when the AP comes up so a long portal wait
  //(or several portal-timeout reboot cycles) never tips the device into
  //recovery mode.
  wifiManager.setAPCallback([](AsyncWiFiManager*) {
    clearBootCounterRtc();
    SerialPrintln("WiFi setup portal up: " + effectiveDeviceName + AP_SUFFIX_SETUP);
  });
  wifiManager.setSaveConfigCallback([]() {
    //Reboot after the portal saves new credentials so the async server
    //can rebind cleanly to the STA interface. Historical note: tzapu's
    //sync portal had a similar gotcha (issues/1579).
    SerialPrintln(F("New WiFi configuration saved. Will need to reboot device to let webserver work..."));
    isPendingReboot = true;
  });
  wifiManager.setConfigPortalTimeout(300);
  wifiManager.setConnectTimeout(30);

  //startConfigPortal (not autoConnect): tryJoinKnownWifi() above already
  //spent its bounded window on the stored credentials, so go straight to
  //the AP instead of retrying them a second time.
  if (wifiManager.startConfigPortal((effectiveDeviceName + AP_SUFFIX_SETUP).c_str())) {
    SerialPrint(F("Successfully Connected to WiFi. IP Address: "));
    SerialPrintln(WiFi.localIP());

    isWifiConfigured = true;
    return;
  }

  //Nobody configured us within the portal window. Reboot and start over:
  //the next boot retries the stored credentials (the router may just have
  //been down) and falls back to a fresh portal. The AP callback cleared
  //the boot counter, so this retry cycle cannot trip recovery mode.
  SerialPrintln(F("Setup portal timed out with no configuration — rebooting to retry"));
  isPendingReboot = true;
}

#if USE_MULTICAST == true
//Deferred worker for the MQTT broker auto-detect endpoint (#129). Armed by
//POST /mqtt/discover; runs the blocking mDNS queries here in loop() context
//(~1 s each, ~2 s worst case when the fallback query also runs — bounded,
//user-triggered, re-entry blocked by the pending flag).
//_mqtt._tcp answers advertise a broker directly; when there are none, a
//_home-assistant._tcp answer points at the HA host (Mosquitto add-on case —
//suggestedBrokerPort() maps that to 1883). Candidate/JSON rules live in
//MdnsDiscovery.h, natively tested.
#define MQTT_DISCOVER_MAX_CANDIDATES 4

void runPendingMqttDiscovery() {
  if (!mqttDiscoverPending) return;

  MdnsBrokerCandidate candidates[MQTT_DISCOVER_MAX_CANDIDATES];
  size_t candidateCount = 0;

  bool fromHomeAssistant = false;
  uint32_t answerCount = MDNS.queryService("mqtt", "tcp");
  if (answerCount == 0) {
    answerCount = MDNS.queryService("home-assistant", "tcp");
    fromHomeAssistant = true;
  }

  for (uint32_t i = 0; i < answerCount && candidateCount < MQTT_DISCOVER_MAX_CANDIDATES; i++) {
    MdnsBrokerCandidate& candidate = candidates[candidateCount];
    candidate.name = normalizeMdnsHostname(MDNS.hostname(i));
    IPAddress answerIp = MDNS.IP(i);
    candidate.ip = (answerIp.isSet() ? answerIp.toString() : String());
    candidate.advertisedPort = MDNS.port(i);
    candidate.fromHomeAssistant = fromHomeAssistant;
    candidateCount++;
  }
  if (answerCount > MQTT_DISCOVER_MAX_CANDIDATES) {
    SerialPrintln("MQTT discovery: capping " + String(answerCount) + " answers to " + String(MQTT_DISCOVER_MAX_CANDIDATES));
  }

  mqttDiscoverResultJson = buildDiscoverJson(candidates, candidateCount);
  mqttDiscoverPending = false;
  SerialPrintln("MQTT discovery finished: " + String(candidateCount) + " candidate(s)");
}
#endif
