/* ####################################################################################################################### */
/* # ____  ____  _     ___ _____   _____ _        _    ____    _____ ____  ____    __  __    _    ____ _____ _____ ____  # */
/* #/ ___||  _ \| |   |_ _|_   _| |  ___| |      / \  |  _ \  | ____/ ___||  _ \  |  \/  |  / \  / ___|_   _| ____|  _ \ # */
/* #\___ \| |_) | |    | |  | |   | |_  | |     / _ \ | |_) | |  _| \___ \| |_) | | |\/| | / _ \ \___ \ | | |  _| | |_) |# */
/* # ___) |  __/| |___ | |  | |   |  _| | |___ / ___ \|  __/  | |___ ___) |  __/  | |  | |/ ___ \ ___) || | | |___|  _ < # */
/* #|____/|_|   |_____|___| |_|   |_|   |_____/_/   \_|_|     |_____|____/|_|     |_|  |_/_/   \_|____/ |_| |_____|_| \_\# */
/* ####################################################################################################################### */
/*
  This project project is done for fun as part of: https://github.com/JonnyBooker/split-flap
  None of this would be possible without the brilliant work of David Königsmann: https://github.com/Dave19171/split-flap

  Licensed under GNU: https://github.com/JonnyBooker/split-flap/blob/master/LICENSE
*/

/* .--------------------------------------------------------------------------------. */
/* |  ___           __ _                    _    _       ___       __ _             | */
/* | / __|___ _ _  / _(_)__ _ _  _ _ _ __ _| |__| |___  |   \ ___ / _(_)_ _  ___ ___| */
/* || (__/ _ | ' \|  _| / _` | || | '_/ _` | '_ | / -_) | |) / -_|  _| | ' \/ -_(_-<| */
/* | \___\___|_||_|_| |_\__, |\_,_|_| \__,_|_.__|_\___| |___/\___|_| |_|_||_\___/__/| */
/* |                    |___/                                                       | */
/* '--------------------------------------------------------------------------------' */
/*
  These define statements can be changed as you desire for changing the functionality and
  behaviour of your device.
*/
#define SERIAL_ENABLE       false   //Option to enable serial debug messages. "true" Will disable I2C communications to allow serial monitoring.
#define UNIT_CALLS_DISABLE  false   //Option to disable the call to the units so can just debug the ESP with no connections
#define UNITS_AMOUNT        10      //Amount of connected units !IMPORTANT TO BE SET CORRECTLY!
#define SERIAL_BAUDRATE     115200  //Serial debugging BAUD rate
#define WIFI_USE_DIRECT     false   //Option to either direct connect to a WiFi Network or setup a AP to configure WiFi. Setting to false will setup as a AP.
#define USE_MULTICAST       false    //Option to broadcast a ".local" URL on your local network default split-flap.local. You can change the name under configurable settings.

/*
  EXPERIMENTAL: Try to use your Router when possible to set a Static IP address for your device to avoid conflicts with other devices
  on your network. This will try and setup your device with a static IP address of your chosing. See below for more details.
*/
#define WIFI_STATIC_IP      false

/* .--------------------------------------------------------. */
/* | ___         _               ___       __ _             | */
/* |/ __|_  _ __| |_ ___ _ __   |   \ ___ / _(_)_ _  ___ ___| */
/* |\__ | || (_-|  _/ -_| '  \  | |) / -_|  _| | ' \/ -_(_-<| */
/* ||___/\_, /__/\__\___|_|_|_| |___/\___|_| |_|_||_\___/__/| */
/* |     |__/                                               | */
/* '--------------------------------------------------------' */
/*
  These are important to maintain normal system behaviour. Only change if you know 
  what your doing.
*/
#define ANSWER_SIZE         1       //Size of unit's request answer
#define FLAP_AMOUNT         45      //Amount of Flaps in each unit
#define MIN_SPEED           1       //Min Speed
#define MAX_SPEED           12      //Max Speed

/* .-----------------------------------. */
/* | _    _ _                 _        | */
/* || |  (_| |__ _ _ __ _ _ _(_)___ ___| */
/* || |__| | '_ | '_/ _` | '_| / -_(_-<| */
/* ||____|_|_.__|_| \__,_|_| |_\___/__/| */
/* '-----------------------------------' */
/*
  External library dependencies, not much more to say!
*/

//WiFi Setup Library if we use that mode
//Specifically put here in this order to avoid conflict with other libraries
#if WIFI_USE_DIRECT == false
//Needed in order to be compatible with WiFiManager: https://github.com/me-no-dev/ESPAsyncWebServer/issues/418#issuecomment-667976368
#define WEBSERVER_H
#include <WiFiManager.h>
#endif

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebSrv.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <time.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include "Classes.h"
#include "ESPMaster.h"
#include "HelpersSerialHandling.h"
#include "WebAssets.h"
/* .------------------------------------------------------------------------------------. */
/* |  ___           __ _                    _    _       ___     _   _   _              | */
/* | / __|___ _ _  / _(_)__ _ _  _ _ _ __ _| |__| |___  / __|___| |_| |_(_)_ _  __ _ ___| */
/* || (__/ _ | ' \|  _| / _` | || | '_/ _` | '_ | / -_) \__ / -_|  _|  _| | ' \/ _` (_-<| */
/* | \___\___|_||_|_| |_\__, |\_,_|_| \__,_|_.__|_\___| |___\___|\__|\__|_|_||_\__, /__/| */
/* |                    |___/                                                  |___/    | */
/* '------------------------------------------------------------------------------------' */
/*
  Settings you can feel free to change to customise how your display works.
*/
//Used if connecting via "WIFI_USE_DIRECT" of "true" - Otherwise, leave blank
const char* wifiDirectSsid = "";
const char* wifiDirectPassword = "";

// timezonePosix: POSIX TZ string for the local timezone. Leave empty for UTC.
// Examples:
//   "CET-1CEST,M3.5.0,M10.5.0/3"  Central European Time with DST
//   "GMT0BST,M3.5.0/1,M10.5.0"    UK (Europe/London)
//   "EST5EDT,M3.2.0,M11.1.0"      US Eastern Time
// Full list: https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
const char* timezonePosix = "";

// timezoneServer: NTP server for time sync. Empty defaults to "pool.ntp.org".
const char* timezoneServer = "";


//Date / clock format strings. strftime(3) conversion specifiers:
//  https://en.cppreference.com/w/c/chrono/strftime
const char* dateFormat = "%d.%m.%Y"; //Examples: %d.%m.%Y -> 11.09.2021, %a %b %y -> Sat Sep 21
const char* clockFormat = "%H:%M";   //Examples: %H:%M -> 21:19, %I:%M%p -> 09:19PM

//How long to show a message for when a scheduled message is shown for
const int scheduledMessageDisplayTimeMillis = 7500;

//Name to broadcast when USE_MULTICAST is enabled. Default is split-flap.local. Be mindful to choose something
//unique to your local network. if running more than one display you'll need a unique name for each. 
const char* mdnsName = "split-flap";

#if WIFI_STATIC_IP == true
//Static IP address for your device. Try take care to not conflict with something else on your network otherwise
//it is likely to not work
IPAddress wifiDeviceStaticIp(192, 168, 1, 100);

//Your router details
IPAddress wifiRouterGateway(192, 168, 1, 1);
IPAddress wifiSubnet(255, 255, 0, 0);

//DNS Entry. Default: Google DNS
IPAddress wifiPrimaryDns(8, 8, 8, 8);
#endif

/* .------------------------------------------------------------. */
/* | ___         _               ___     _   _   _              | */
/* |/ __|_  _ __| |_ ___ _ __   / __|___| |_| |_(_)_ _  __ _ ___| */
/* |\__ | || (_-|  _/ -_| '  \  \__ / -_|  _|  _| | ' \/ _` (_-<| */
/* ||___/\_, /__/\__\___|_|_|_| |___\___|\__|\__|_|_||_\__, /__/| */
/* |     |__/                                          |___/    | */
/* '------------------------------------------------------------' */
/*
  Used for normal running of the system so changing things here might make things 
  behave a little strange.
*/
//The current version of code to display on the UI
const char* espVersion = "2.2.0";

//All the letters on the units that we have to be displayed. You can change these if it so pleases at your own risk
const char letters[] = {' ', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '$', '&', '#', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', '.', '-', '?', '!'};
int displayState[UNITS_AMOUNT];
unsigned long previousMillis = 0;

//Search for parameter in HTTP POST request
const char* PARAM_ALIGNMENT = "alignment";
const char* PARAM_FLAP_SPEED = "flapSpeed";
const char* PARAM_DEVICEMODE = "deviceMode";
const char* PARAM_INPUT_TEXT = "inputText";
const char* PARAM_SCHEDULE_ENABLED = "scheduleEnabled";
const char* PARAM_SCHEDULE_DATE_TIME = "scheduledDateTimeUnix";
const char* PARAM_SCHEDULE_SHOW_INDEFINITELY = "scheduleShowIndefinitely";
const char* PARAM_COUNTDOWN_DATE = "countdownDateTimeUnix";
const char* PARAM_ID = "id";

//Device Modes
const char* DEVICE_MODE_TEXT = "text";
const char* DEVICE_MODE_CLOCK = "clock";
const char* DEVICE_MODE_DATE = "date";
const char* DEVICE_MODE_COUNTDOWN = "countdown";

//Alignment options
const char* ALIGNMENT_MODE_LEFT = "left";
const char* ALIGNMENT_MODE_CENTER = "center";
const char* ALIGNMENT_MODE_RIGHT = "right";

//Variables for storing things for checking and use in normal running
String alignment = "";
String flapSpeed = "";
String inputText = "";
String deviceMode = "";
String countdownToDateUnix = "";
String lastWrittenText = "";
String lastReceivedMessageDateTime = "";
bool alignmentUpdated = false;
bool isPendingReboot = false;
bool isPendingUnitsReset = false;
bool isWifiConfigured = false;
std::vector<ScheduledMessage> scheduledMessages;

//Create AsyncWebServer object on port 80
AsyncWebServer webServer(80);

//Used for creating a Access Point to allow WiFi setup
#if WIFI_USE_DIRECT == false
WiFiManager wifiManager;
bool isPendingWifiReset = false;
#endif

/* .-----------------------------------------------. */
/* | ___          _          ___     _             | */
/* ||   \ _____ _(_)__ ___  / __|___| |_ _  _ _ __ | */
/* || |) / -_\ V | / _/ -_) \__ / -_|  _| || | '_ \| */
/* ||___/\___|\_/|_\__\___| |___\___|\__|\_,_| .__/| */
/* |                                         |_|   | */
/* '-----------------------------------------------' */
void setup() {
#if SERIAL_ENABLE == true
  //Setup so we can see serial messages
  Serial.begin(SERIAL_BAUDRATE);
#else
  //For ESP01 only. Wire buffer size is bumped to 256 via the -D
  //I2C_BUFFER_LENGTH=256 build flag so that a full twiboot flash page
  //(128 bytes + 4-byte header) fits in one transmission during firmware
  //OTA. Default ESP8266 Wire buffer is only 128 bytes.
  Wire.begin(1, 3);

  //De-activate I2C if debugging the ESP, otherwise serial does not work
  //Wire.begin(D1, D2); //For NodeMCU testing only SDA=D1 and SCL=D2
#endif
  SerialPrintln("");
  SerialPrintln("#######################################################");
  SerialPrintln("..............Split Flap Display Starting..............");
  SerialPrintln("#######################################################");

  //Load and read all the things
  initWiFi();
  
  //Helpful if want to force reset WiFi settings for testing
  //wifiManager.resetSettings();

  if (isWifiConfigured && !isPendingReboot) {
    //Time sync via ESP8266 core + libc time.h
    const char* tz  = (strlen(timezonePosix) > 0) ? timezonePosix : "UTC0";
    const char* ntp = (strlen(timezoneServer) > 0) ? timezoneServer : "pool.ntp.org";
    configTime(tz, ntp);

    SerialPrint("Waiting for NTP sync (tz=");
    SerialPrint(tz);
    SerialPrint(", server=");
    SerialPrint(ntp);
    SerialPrintln(")");

    time_t nowSec = 0;
    for (int i = 0; i < 100 && nowSec < 1000000000L; i++) {
      delay(100);
      nowSec = time(nullptr);
    }

    SerialPrint("Current time: ");
    SerialPrintln(formatDateTime("%Y-%m-%d %H:%M:%S"));
    
    //Load various variables
    initialiseFileSystem();
    loadValuesFromFileSystem();

    //Scan the I2C bus so we know how many units actually answered. Result is
    //exposed via /settings so the UI can warn about a mismatch. The probe
    //also distinguishes bootloader-mode vs sketch-mode units so the next
    //step can auto-install the bundled firmware on any blank units.
    probeI2cBus();

    //If any unit is sitting in its twiboot bootloader with no app installed,
    //stream the PROGMEM-embedded Unit.ino binary to it. This is what makes a
    //fresh build come alive with only one ICSP per Nano (twiboot only — the
    //Unit sketch is delivered over I2C on first boot of the master).
    autoInstallFirmwareToBootloaderUnits();

#if USE_MULTICAST == true
  if (MDNS.begin(mdnsName)) {
      SerialPrintln("mDNS responder started");
    } else {
      SerialPrintln("Error setting up MDNS responder!");
    }
#endif

    //Web Server Endpoint configuration — all assets served from PROGMEM
    //(generated by build_assets.py into WebAssets.h). Gzipped entries are
    //sent with Content-Encoding: gzip; browsers unpack transparently.
    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
      SerialPrintln("Request Home Page Received");
      AsyncWebServerResponse *resp = request->beginResponse_P(200, "text/html", INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
      resp->addHeader("Content-Encoding", "gzip");
      request->send(resp);
    });
    webServer.on("/index.html", HTTP_GET, [](AsyncWebServerRequest * request) {
      AsyncWebServerResponse *resp = request->beginResponse_P(200, "text/html", INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
      resp->addHeader("Content-Encoding", "gzip");
      request->send(resp);
    });
    webServer.on("/script.js", HTTP_GET, [](AsyncWebServerRequest * request) {
      AsyncWebServerResponse *resp = request->beginResponse_P(200, "application/javascript", SCRIPT_JS_GZ, SCRIPT_JS_GZ_LEN);
      resp->addHeader("Content-Encoding", "gzip");
      request->send(resp);
    });
    webServer.on("/style.css", HTTP_GET, [](AsyncWebServerRequest * request) {
      AsyncWebServerResponse *resp = request->beginResponse_P(200, "text/css", STYLE_CSS_GZ, STYLE_CSS_GZ_LEN);
      resp->addHeader("Content-Encoding", "gzip");
      request->send(resp);
    });
    webServer.on("/favicon.png", HTTP_GET, [](AsyncWebServerRequest * request) {
      request->send_P(200, "image/png", FAVICON_PNG, FAVICON_PNG_LEN);
    });

    webServer.on("/settings", HTTP_GET, [](AsyncWebServerRequest * request) {
      SerialPrintln("Request for Settings Received");
      
      String json = getCurrentSettingValues();
      request->send(200, "application/json", json);
      json = String();
    });
    
    webServer.on("/health", HTTP_GET, [](AsyncWebServerRequest * request) {
      SerialPrintln("Request for Health Check Received");
      request->send(200, "text/plain", "Healthy");
    });

    webServer.on("/log", HTTP_GET, [](AsyncWebServerRequest * request) {
      //Don't SerialPrintln here; every log request would otherwise stamp
      //itself into the buffer on every poll and drown out real activity.
      request->send(200, "text/plain", webLogRead());
    });
    
    webServer.on("/reboot", HTTP_GET, [](AsyncWebServerRequest * request) {
      SerialPrintln("Request to Reboot Received");
      
      //Create HTML page to explain the system is rebooting
      IPAddress ip = WiFi.localIP();
      
      String html = "<div style='text-align:center'>";
      html += "<font face='arial'><h1>Split Flap - Rebooting</h1>";
      html += "<p>Reboot is pending now...<p>";
      html += "<p>This can take anywhere between 10-20 seconds<p>";
      html += "<p>You can go to the main home page after this time by clicking the button below or going to '/'.</p>";
      html += "<p><a href=\"http://" + ip.toString() + "\">Home</a></p>";
      html += "</font>";
      html += "</div>";
      
      request->send(200, "text/html", html);
      isPendingReboot = true;
    });
    
    //POST /firmware/master — web OTA for the ESP itself. Receives a .bin
    //via multipart upload, flashes via the ESP8266 Update class, reboots.
    //Usable because the eagle.flash.1m.ld (no-FS) layout leaves ~1 MB for
    //sketch+staging — the 443 KB firmware has room for another 443 KB
    //staged image below _FS_start (which is 0xFB000 with no FS).
    webServer.on("/firmware/master", HTTP_POST,
      [](AsyncWebServerRequest * request) {
        if (Update.hasError()) {
          String msg = String("Master OTA failed: ") + Update.getErrorString();
          SerialPrintln(msg);
          request->send(500, "text/plain", msg);
        } else {
          request->send(200, "text/plain", "Master firmware flashed; rebooting…");
          isPendingReboot = true;
        }
      },
      [](AsyncWebServerRequest * request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        if (index == 0) {
          SerialPrint("Master OTA starting: ");
          SerialPrintln(filename);
          uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
          Update.runAsync(true);
          if (!Update.begin(maxSketchSpace, U_FLASH)) {
            SerialPrint("Update.begin failed: ");
            SerialPrintln(Update.getErrorString());
          }
        }
        if (!Update.hasError() && len > 0) {
          if (Update.write(data, len) != len) {
            SerialPrint("Update.write failed: ");
            SerialPrintln(Update.getErrorString());
          }
        }
        if (final) {
          if (Update.end(true)) {
            SerialPrint("Master OTA complete, ");
            SerialPrint(index + len);
            SerialPrintln(" bytes written");
          } else {
            SerialPrint("Update.end failed: ");
            SerialPrintln(Update.getErrorString());
          }
        }
      }
    );

    //Debug endpoint: tells a unit to reboot into its twiboot bootloader.
    //GET /unit/reboot?address=0x01 — useful without a HEX file to sanity-
    //check the enter-bootloader path (unit should disappear briefly and
    //twiboot show up on 0x29).
    webServer.on("/unit/reboot", HTTP_GET, [](AsyncWebServerRequest * request) {
      if (!request->hasParam("address")) {
        request->send(400, "text/plain", "Missing 'address' query param (e.g. /unit/reboot?address=0x01)");
        return;
      }
      String addressValue = request->getParam("address")->value();
      long parsedAddress = strtol(addressValue.c_str(), nullptr, 0);
      if (parsedAddress < 1 || parsedAddress > 126) {
        request->send(400, "text/plain", "Address must be 1..126");
        return;
      }
      int status = rebootUnitToBootloader((int)parsedAddress);
      if (status == 0) {
        request->send(200, "text/plain", "Reboot command sent. Unit should be in twiboot at 0x29 for ~1s.");
      } else {
        String body = "Wire.endTransmission returned ";
        body += status;
        request->send(502, "text/plain", body);
      }
    });

    webServer.on("/reset-units", HTTP_GET, [](AsyncWebServerRequest * request) {
      SerialPrintln("Request to Reset Units Received");
      
      //This will be picked up in the loop
      isPendingUnitsReset = true;
      
      request->redirect("/?is-resetting-units=true");
    });

    webServer.on("/scheduled-message/remove", HTTP_DELETE, [](AsyncWebServerRequest * request) {
      SerialPrintln("Request to Remove Scheduled Message Received");
      
      if (request->hasParam(PARAM_ID)) {
        String idValue = request->getParam(PARAM_ID)->value();

        if (isNumber(idValue)) {
          long parsedIdValue = atol(idValue.c_str());
          bool removed = removeScheduledMessage(parsedIdValue);
          
          if (removed) {
            request->send(202, "text/plain", "Removed");
          }
          else {
            request->send(400, "text/plain", "Unable to find message with ID specified. Id: " + idValue);
          }
        }
        else {
          SerialPrintln("Invalid Delete Scheduled Message ID Received");
          request->send(400, "text/plain", "Invalid ID value");
        }
      } 
      else {
          SerialPrintln("Delete Scheduled Message Received with no ID");
          request->send(400, "text/plain", "No ID specified");
      }
    });

    webServer.on("/", HTTP_POST, [](AsyncWebServerRequest * request) {
      SerialPrintln("Request Post of Form Received");    

      bool submissionError = false;
      
      long newMessageScheduleDateTimeUnixValue = 0;
      bool newMessageScheduleEnabledValue = false;
      bool newMessageScheduleShowIndefinitely = false;
      String newAlignmentValue, newDeviceModeValue, newFlapSpeedValue, newInputTextValue, newCountdownToDateUnixValue;
      
      int params = request->params();
      for (int paramIndex = 0; paramIndex < params; paramIndex++) {
        AsyncWebParameter* p = request->getParam(paramIndex);
        if (p->isPost()) {
          //HTTP POST alignment value
          if (p->name() == PARAM_ALIGNMENT) {
            String receivedValue = p->value();
            if (receivedValue == ALIGNMENT_MODE_LEFT || receivedValue == ALIGNMENT_MODE_CENTER || receivedValue == ALIGNMENT_MODE_RIGHT) {
              newAlignmentValue = receivedValue;
            }
            else {
              SerialPrintln("Alignment provided was not valid. Value: " + receivedValue); 
              submissionError = true;
            }
          }

          //HTTP POST device mode value
          if (p->name() == PARAM_DEVICEMODE) {
            String receivedValue = p->value();
            if (receivedValue == DEVICE_MODE_TEXT || receivedValue == DEVICE_MODE_CLOCK || receivedValue == DEVICE_MODE_DATE || receivedValue == DEVICE_MODE_COUNTDOWN) {
              newDeviceModeValue = receivedValue;          
            }
            else {
              SerialPrintln("Device Mode provided was not valid. Invalid Value: " + receivedValue); 
              submissionError = true;
            }
          }

          //HTTP POST Flap Speed Slider value
          if (p->name() == PARAM_FLAP_SPEED) {
            newFlapSpeedValue = p->value().c_str();
          }

          //HTTP POST inputText value
          if (p->name() == PARAM_INPUT_TEXT) {
            newInputTextValue = p->value().c_str();
          }

          //HTTP POST Schedule Enabled
          if (p->name() == PARAM_SCHEDULE_ENABLED) {
            String newMessageScheduleEnabledString = p->value().c_str();
            newMessageScheduleEnabledValue = newMessageScheduleEnabledString == "on" ?
              true : 
              false;
          }
          
          //HTTP POST Schedule Show Indefinitely
          if (p->name() == PARAM_SCHEDULE_SHOW_INDEFINITELY) {
            String newMessageScheduleShowIndefinitelyString = p->value().c_str();
            newMessageScheduleShowIndefinitely = newMessageScheduleShowIndefinitelyString == "on" ?
              true : 
              false;
          }

          //HTTP POST Schedule Seconds
          if (p->name() == PARAM_SCHEDULE_DATE_TIME) {
            String receivedValue = p->value().c_str();
            if (isNumber(receivedValue)) {
              newMessageScheduleDateTimeUnixValue = atol(receivedValue.c_str());
            }
            else {
              SerialPrintln("Schedule date time provided was not valid. Invalid Value: " + receivedValue); 
              submissionError = true;
            }
          }

          //HTTP POST Countdown Seconds
          if (p->name() == PARAM_COUNTDOWN_DATE) {
            String receivedValue = p->value().c_str();
            if (isNumber(receivedValue)) {
              newCountdownToDateUnixValue = receivedValue;
            }
            else {
              SerialPrintln("Countdown date provided was not valid. Invalid Value: " + receivedValue); 
              submissionError = true;
            }
          }
        }
      }    

      //If there was an error, report back to check what has been input
      if (submissionError) {
        SerialPrintln("Finished Processing Request with Error");
        request->redirect("/?invalid-submission=true");
      }
      else {
        SerialPrintln("Finished Processing Request Successfully");

        lastReceivedMessageDateTime = formatDateTime("%d %b %y %H:%M:%S");

        //Only if a new alignment value
        if (alignment != newAlignmentValue) {
          alignment = newAlignmentValue;
          alignmentUpdated = true;

          saveAlignment();
          SerialPrintln("Alignment Updated: " + alignment);
        }

        //Only if a new flap speed value
        if (flapSpeed != newFlapSpeedValue) {
          flapSpeed = newFlapSpeedValue;

          saveFlapSpeed();
          SerialPrintln("Flap Speed Updated: " + flapSpeed);
        }

        //Only if countdown date has changed
        if (countdownToDateUnix != newCountdownToDateUnixValue) {
          countdownToDateUnix = newCountdownToDateUnixValue;

          saveCountdown();
          SerialPrintln("Countdown Date Time Unix Updated: " + countdownToDateUnix);
        }

        //If its a new scheduled message, add it to the backlog and proceed, don't want to change device mode
        //Else, we do want to change the device mode and clear out the input text
        if (newMessageScheduleEnabledValue) {
          addAndPersistScheduledMessage(newInputTextValue, newMessageScheduleDateTimeUnixValue, newMessageScheduleShowIndefinitely);
          SerialPrintln("New Scheduled Message added");
        }
        else {
          //Only if device mode has changed
          if (deviceMode != newDeviceModeValue) {
            deviceMode = newDeviceModeValue;

            saveDeviceMode();
            SerialPrintln("Device Mode Set: " + deviceMode);
          }

          //Only if we are showing text
          if (deviceMode == DEVICE_MODE_TEXT) {
            inputText = newInputTextValue;
          }
        }

        //Redirect so that we don't have the "re-submit form" problem in browser for refresh
        request->redirect("/");
      }
    });

#if WIFI_USE_DIRECT == false
    webServer.on("/reset-wifi", HTTP_GET, [](AsyncWebServerRequest * request) {
      SerialPrintln("Request to Reset WiFi Received");
      
      IPAddress ip = WiFi.localIP();
      
      String html = "<div style='text-align:center'>";
      html += "<font face='arial'><h1>Split Flap - Resetting WiFi</h1>";
      html += "<p>WiFi Settings have been erased. Device will now reboot...<p>";
      html += "<p>You will now be able to connect to this device in AP mode to configure the WiFi once more<p>";
      html += "<p>You can go to the main home page after this time by clicking the button below or going to '/'.</p>";
      html += "<p><a href=\"http://" + ip.toString() + "\">Home</a></p>";
      html += "</font>";
      html += "</div>";
      
      request->send(200, "text/html", html);
      isPendingWifiReset = true;
    });
#endif   

    delay(250);
    webServer.begin();

    SerialPrintln("Split Flap Ready!");
    SerialPrintln("#######################################################");
  }
  else {
    if (isPendingReboot) {
      SerialPrintln("Reboot is pending to be able to continue device function. Hold please...");
      SerialPrintln("#######################################################");
    }
    else {
      SerialPrintln("Unable to connect to WiFi... Not starting web server");
      SerialPrintln("Please hard restart your device to try connect again");
      SerialPrintln("#######################################################");
    }
  }
}

/* .----------------------------------------------------. */
/* | ___                _             _                 | */
/* || _ \_  _ _ _  _ _ (_)_ _  __ _  | |   ___ ___ _ __ | */
/* ||   | || | ' \| ' \| | ' \/ _` | | |__/ _ / _ | '_ \| */
/* ||_|_\\_,_|_||_|_||_|_|_||_\__, | |____\___\___| .__/| */
/* |                          |___/               |_|   | */
/* '----------------------------------------------------' */
void loop() {
  //Reboot in here as if we restart within a request handler, no response is returned
  if (isPendingReboot) {
    SerialPrintln("Rebooting Now... Fairwell!");
    SerialPrintln("#######################################################");
    delay(100);

    ESP.restart();
    return;
  }

#if WIFI_USE_DIRECT == false
  //Clear off the WiFi Manager Settings
  if (isPendingWifiReset) {
    SerialPrintln("Removing WiFi settings");
    wifiManager.resetSettings();
    delay(100);

    isPendingReboot = true;
    return;
  }
#endif

#if USE_MULTICAST == true
  MDNS.update();
#endif

  //Do nothing if WiFi is not configured
  if (!isWifiConfigured) {
    //Show there is an error via text on display
    deviceMode = DEVICE_MODE_TEXT;
    alignment = ALIGNMENT_MODE_CENTER;
    flapSpeed = "80";

    showText("OFFLINE");
    delay(100);
    return;
  }

  if (isPendingUnitsReset) {
    SerialPrintln("Reseting Units now...");

    //Blank out the message
    String blankOutText1 = createRepeatingString('-');
    showText(blankOutText1);
    delay(2000);

    //Do just enough to do a full iteration which triggers the re-calibration
    String blankOutText2 = createRepeatingString('.');
    showText(blankOutText2);

    //We did a reset!
    isPendingUnitsReset = false;

    SerialPrintln("Done Units Reset!");
  }
  
  //Don't touch the I2C bus while a unit firmware flash is in flight — the
  //Wire bus is owned by ServiceFirmwareFunctions for the duration.
  if (firmwareFlashInProgress) {
    delay(1);
    return;
  }

  //Process every second
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= 1000) {
    previousMillis = currentMillis;

    checkScheduledMessages();
    checkCountdown();

    //Mode Selection
    if (deviceMode == DEVICE_MODE_TEXT || deviceMode == DEVICE_MODE_COUNTDOWN) { 
      showText(inputText);
    } 
    else if (deviceMode == DEVICE_MODE_DATE) {
      showText(formatDateTime(dateFormat));
    }
    else if (deviceMode == DEVICE_MODE_CLOCK) {
      showText(formatDateTime(clockFormat));
    }
  }
}

//Gets all the currently stored calues from memory in a JSON object
String getCurrentSettingValues() {
  JsonDocument document;

  document["timezoneOffset"] = getTimezoneOffsetMinutes();
  document["unitCount"] = UNITS_AMOUNT;
  document["detectedUnitCount"] = detectedUnitCount;
  for (int i = 0; i < detectedUnitCount; i++) {
    document["detectedUnitAddresses"][i] = detectedUnitAddresses[i];
  }
  document["alignment"] = alignment;
  document["flapSpeed"] = flapSpeed;
  document["deviceMode"] = deviceMode;
  document["version"] = espVersion;
  document["lastTimeReceivedMessageDateTime"] = lastReceivedMessageDateTime;
  document["lastWrittenText"] = lastWrittenText;
  document["countdownToDateUnix"] = atol(countdownToDateUnix.c_str());

  for(int scheduledMessageIndex = 0; scheduledMessageIndex < scheduledMessages.size(); scheduledMessageIndex++) {
    ScheduledMessage scheduledMessage = scheduledMessages[scheduledMessageIndex];
    
    document["scheduledMessages"][scheduledMessageIndex]["scheduledDateTimeUnix"] = scheduledMessage.ScheduledDateTimeUnix;
    document["scheduledMessages"][scheduledMessageIndex]["message"] = scheduledMessage.Message;
    document["scheduledMessages"][scheduledMessageIndex]["showIndefinitely"] = scheduledMessage.ShowIndefinitely;
  }

  document["otaEnabled"] = false;
  document["isInOtaMode"] = false;

#if WIFI_USE_DIRECT == false
  document["wifiSettingsResettable"] = true;
#else
  document["wifiSettingsResettable"] = false;
#endif
  
  String jsonString;
  serializeJson(document, jsonString);

  return jsonString;
}

//Format the current local time via strftime. Replaces ezTime's
//timezone.dateTime(fmt); format tokens are the standard strftime ones.
String formatDateTime(const char* fmt) {
  time_t nowSec = time(nullptr);
  struct tm tmInfo;
  localtime_r(&nowSec, &tmInfo);
  char buf[64];
  strftime(buf, sizeof(buf), fmt, &tmInfo);
  return String(buf);
}

//Returns the current UTC offset in minutes (east of UTC is positive).
//Matches what ezTime's Timezone::getOffset() used to return — the JS
//frontend multiplies this by 60000 to get a millisecond offset.
long getTimezoneOffsetMinutes() {
  time_t nowSec = time(nullptr);
  struct tm utcTm;
  gmtime_r(&nowSec, &utcTm);
  //Treat the UTC-broken-down time as if it were local; the delta from the
  //real epoch is the local offset.
  time_t asLocal = mktime(&utcTm);
  return (long)((nowSec - asLocal) / 60);
}
