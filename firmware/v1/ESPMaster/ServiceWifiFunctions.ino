//Initialize WiFi
void initWiFi() {
  int wifiConnectTimeoutSeconds = 180;

  WiFi.mode(WIFI_STA);

#if WIFI_USE_DIRECT == false
  SerialPrintln(F("Setting up WiFi AP Setup Mode"));

#if WIFI_STATIC_IP == true
  wifiManager.setSTAStaticIPConfig(wifiDeviceStaticIp, wifiRouterGateway, wifiSubnet, wifiPrimaryDns);
  SerialPrintln(F("WiFi Static IP Configured"));
#endif

  //ESPAsyncWiFiManager exposes a subset of tzapu's config API; the rest
  //(title/hostname/dark mode/menu/blocking flag) isn't configurable here.
  //WiFi.setAutoReconnect() is an ESP8266-core call, still honored.
  WiFi.hostname("Split-Flap");
  WiFi.setAutoReconnect(true);
  wifiManager.setConfigPortalTimeout(wifiConnectTimeoutSeconds);
  wifiManager.setConnectTimeout(120);
  wifiManager.setSaveConfigCallback([]() {
    //Reboot after the portal saves new credentials so the async server
    //can rebind cleanly to the STA interface. Historical note: tzapu's
    //sync portal had a similar gotcha (issues/1579).
    SerialPrintln(F("New WiFi configuration saved. Will need to reboot device to let webserver work..."));
    isPendingReboot = true;
  });

  SerialPrintln(F("Attempting to connect to WiFi... Will fallback to AP mode to allow configuring of WiFi if fails..."));
  if(wifiManager.autoConnect("Split-Flap-AP")) {
    SerialPrint(F("Successfully Connected to WiFi. IP Address: "));
    SerialPrintln(WiFi.localIP());

    isWifiConfigured = true;
  }
  
#else
  SerialPrintln(F("Setting up WiFi Direct"));

  if (strlen(wifiDirectSsid) > 0 && strlen(wifiDirectPassword) > 0) {
    int maxAttemptsCount = 0;
    
#if WIFI_STATIC_IP == true
    if (WiFi.config(wifiDeviceStaticIp, wifiRouterGateway, wifiSubnet, wifiPrimaryDns)) {
      SerialPrintln(F("WiFi Static IP Configuration Success"));
    }
    else {
      SerialPrintln(F("WiFi Static IP Configuration could not take place"));
    }
#endif
    
    WiFi.begin(wifiDirectSsid, wifiDirectPassword);
    SerialPrint(F("Connecting"));

    while (WiFi.status() != WL_CONNECTED && maxAttemptsCount != wifiConnectTimeoutSeconds) {
      if (maxAttemptsCount % 10 == 0) {
        SerialPrint('\n');
      }
      else {
        SerialPrint('.');
      }

      delay(1000);

      maxAttemptsCount++;      
    }

    //If we reached the max timeout
    if (maxAttemptsCount != wifiConnectTimeoutSeconds) {
      SerialPrint(F("Successfully Connected to WiFi. IP Address: "));
      SerialPrintln(WiFi.localIP());

      isWifiConfigured = true;
    }
  }

#endif
}
