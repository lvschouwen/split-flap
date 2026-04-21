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
#define WIFI_USE_DIRECT     true    //Option to either direct connect to a WiFi Network or setup a AP to configure WiFi. Setting to false will setup as a AP.
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
#include <DNSServer.h>
#include <ESPAsyncWiFiManager.h>
#endif

#include <Arduino.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebSrv.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <time.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include "ESPMaster.h"
#include <EEPROM.h>
#include "SettingsEepromLayout.h"
#include "HelpersSerialHandling.h"
#include "WebAssets.h"
#include "BuildVersion.h"
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
//WiFi credentials for WIFI_USE_DIRECT == true live in a gitignored local
//header so the public repo doesn't leak them. Copy WifiCredentials.h.example
//to WifiCredentials.h and fill in your SSID / password. If the file is
//missing (fresh checkout, CI) we fall back to empty strings so the build
//still compiles — the device just won't connect until you provide creds.
#if WIFI_USE_DIRECT == true && __has_include("WifiCredentials.h")
  #include "WifiCredentials.h"
#else
  #if WIFI_USE_DIRECT == true
    #warning "WIFI_USE_DIRECT is true but WifiCredentials.h is missing — copy WifiCredentials.h.example to fill it in."
  #endif
  const char* wifiDirectSsid = "";
  const char* wifiDirectPassword = "";
#endif

// timezonePosix: build-time DEFAULT POSIX TZ string. Overridden at runtime
// by the web UI setting (persisted to EEPROM). Also baked into the fresh-
// init EEPROM value (see #53) so a wipe+reflash yields a correctly-clocked
// device without needing web-UI setup.
// Examples:
//   "CET-1CEST,M3.5.0,M10.5.0/3"  Central European Time with DST (default)
//   "GMT0BST,M3.5.0/1,M10.5.0"    UK (Europe/London)
//   "EST5EDT,M3.2.0,M11.1.0"      US Eastern Time
// Full list: https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
const char* timezonePosix = "CET-1CEST,M3.5.0,M10.5.0/3";

// timezoneServer: NTP server for time sync. Empty defaults to "pool.ntp.org".
const char* timezoneServer = "";


//Clock format string. strftime(3) conversion specifiers:
//  https://en.cppreference.com/w/c/chrono/strftime
const char* clockFormat = "%H:%M";   //Examples: %H:%M -> 21:19, %I:%M%p -> 09:19PM

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
//Build tag shown in the web UI. Injected at compile time by
//build_assets.py (generates BuildVersion.h). Falls back to "unknown"
//if the build environment isn't a git checkout.
const char* espVersion = GIT_REV;

//All the letters on the units that we have to be displayed. You can change these if it so pleases at your own risk
const char letters[] = {' ', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '$', '&', '#', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', '.', '-', '?', '!'};
int displayState[UNITS_AMOUNT];
unsigned long previousMillis = 0;

//Search for parameter in HTTP POST request
const char* PARAM_ALIGNMENT = "alignment";
const char* PARAM_FLAP_SPEED = "flapSpeed";
const char* PARAM_DEVICEMODE = "deviceMode";
const char* PARAM_INPUT_TEXT = "inputText";
const char* PARAM_TIMEZONE   = "timezone";

//Device Modes
const char* DEVICE_MODE_TEXT = "text";
const char* DEVICE_MODE_CLOCK = "clock";

//Alignment options
const char* ALIGNMENT_MODE_LEFT = "left";
const char* ALIGNMENT_MODE_CENTER = "center";
const char* ALIGNMENT_MODE_RIGHT = "right";

//Variables for storing things for checking and use in normal running
String alignment = "";
String flapSpeed = "";
String inputText = "";
String deviceMode = "";
//Runtime POSIX TZ string (web-UI setting, persisted to EEPROM). Empty falls
//back to compile-time `timezonePosix`, which in turn falls back to "UTC0".
//Issue #48.
String timezonePosixSetting = "";
String lastWrittenText = "";
String lastReceivedMessageDateTime = "";
bool alignmentUpdated = false;
bool isPendingReboot = false;
bool isPendingUnitsReset = false;
bool isWifiConfigured = false;

//OTA failure diagnostics (#52). Captured at boot; exposed via /settings so
//the flasher can tell a silent revert apart from a clean reboot.
//  lastResetReason    — ESP.getResetReason() at the top of setup()
//  intendedVersionEeprom — GIT_REV we told the device to become via /firmware/master?v=
//  otaReverted        — true iff intendedVersionEeprom is non-empty AND differs from
//                       the running GIT_REV (i.e. the upload "took" from the handler's
//                       perspective but the new image isn't actually running now).
String lastResetReason = "";
String intendedVersionEeprom = "";
bool otaReverted = false;

//Set by POST /stop to break out of the wait loop inside showMessage() when
//a unit gets physically stuck and its status byte pegs at "rotating"
//forever. Checked every ~100 ms; cleared at the start of each new
//showMessage. Issue #35.
volatile bool abortCurrentShow = false;

//Recovery mode + boot-loop protection (issue #37). Counter lives in RTC user
//memory (survives warm restart, cleared on cold power cycle). Every boot
//increments; a loop that runs cleanly for HEALTHY_BOOT_MS clears it. If the
//counter reaches RECOVERY_BOOT_THRESHOLD, the device skips the main app and
//brings up a minimal "split-flap-recovery" SoftAP that only accepts a master
//firmware upload.
struct RtcBootState {
  uint32_t magic;
  uint32_t bootCounter;
};
static const uint32_t RTC_BOOT_MAGIC = 0xC0FFEE42UL;
static const uint32_t RTC_BOOT_OFFSET_BLOCKS = 0;
static const uint32_t RECOVERY_BOOT_THRESHOLD = 3;
static const unsigned long HEALTHY_BOOT_MS = 30000UL;

bool isRecoveryMode = false;
bool healthyBootMarked = false;

//Master OTA rejection state — set in the upload handler when the content
//length exceeds the sketch slot or the caller-supplied MD5 is malformed.
//Post-handler reads it to return 413/400 instead of the generic 500.
static bool otaRejected = false;
static int otaRejectionStatus = 0;
static String otaRejectionReason;

//Create AsyncWebServer object on port 80
AsyncWebServer webServer(80);

//Used for creating a Access Point to allow WiFi setup. ESPAsyncWiFiManager
//reuses the AsyncWebServer above for its captive portal so we don't carry
//a second (sync) HTTP server in the binary.
#if WIFI_USE_DIRECT == false
DNSServer       dnsServer;
AsyncWiFiManager wifiManager(&webServer, &dnsServer);
bool isPendingWifiReset = false;
#endif

//Read boot state from RTC user memory. Returns a zeroed state if the magic
//doesn't match (fresh power-on, corruption, or the ESP8266 core has been
//using the block for something else).
RtcBootState readBootStateRtc() {
  RtcBootState state;
  ESP.rtcUserMemoryRead(RTC_BOOT_OFFSET_BLOCKS,
                        reinterpret_cast<uint32_t*>(&state),
                        sizeof(state));
  if (state.magic != RTC_BOOT_MAGIC) {
    state.magic = RTC_BOOT_MAGIC;
    state.bootCounter = 0;
  }
  return state;
}

void writeBootStateRtc(RtcBootState state) {
  ESP.rtcUserMemoryWrite(RTC_BOOT_OFFSET_BLOCKS,
                         reinterpret_cast<uint32_t*>(&state),
                         sizeof(state));
}

void clearBootCounterRtc() {
  writeBootStateRtc({RTC_BOOT_MAGIC, 0});
}

//Registers POST /firmware/master. Shared between main mode and recovery mode
//so the upload path (MD5 verify, size check, gated reboot) is identical.
//In recovery mode we skip the I2C unit-reboot hook since the bus may be
//unreachable — that's precisely why the device is in recovery.
void registerMasterFirmwareEndpoint() {
  webServer.on("/firmware/master", HTTP_POST,
    [](AsyncWebServerRequest * request) {
      if (otaRejected) {
        int status = otaRejectionStatus;
        String reason = otaRejectionReason;
        otaRejected = false;
        otaRejectionStatus = 0;
        otaRejectionReason = String();
        request->send(status, "text/plain", reason);
        return;
      }
      if (Update.hasError()) {
        String msg = String("Master OTA failed: ") + Update.getErrorString();
        SerialPrintln(msg);
        request->send(500, "text/plain", msg);
      } else if (!Update.isFinished()) {
        String msg = "Master OTA incomplete: Update.isFinished() == false after final chunk";
        SerialPrintln(msg);
        request->send(500, "text/plain", msg);
      } else {
        //Single source of truth: reboot only when the updater considers the
        //image committed AND no error was latched along the way.
        request->send(200, "text/plain", "Master firmware flashed; rebooting…");
        //A fresh firmware should start with a clean boot counter — otherwise
        //the very first post-flash boot could tip us back into recovery.
        clearBootCounterRtc();
        isPendingReboot = true;
      }
    },
    [](AsyncWebServerRequest * request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      if (index == 0) {
        otaRejected = false;
        otaRejectionStatus = 0;
        otaRejectionReason = String();

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

        Update.runAsync(true);
        if (!Update.begin(maxSketchSpace, U_FLASH)) {
          SerialPrint(F("Update.begin failed: "));
          SerialPrintln(Update.getErrorString());
          return;
        }

        //MD5 is optional but strongly recommended — eboot's built-in checksum
        //only catches the staged image getting corrupted in flash, not a
        //truncated/partially-uploaded image landing on top of a trusted boot.
        if (request->hasParam("md5")) {
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
        }
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
          if (!isRecoveryMode) {
            //Push every sketch-running unit into its twiboot bootloader
            //before we reboot. When the new master comes up and probes
            //the bus it'll find them in bootloader state and auto-flash
            //them from the (possibly updated) PROGMEM bundle.
            int rebooted = enterBootloaderAllDetected(false);
            SerialPrint(F("Queued "));
            SerialPrint(rebooted);
            SerialPrintln(F(" unit(s) for bootloader on next boot"));
          }
        } else {
          SerialPrint(F("Update.end failed: "));
          SerialPrintln(Update.getErrorString());
        }
      }
    }
  );
}

//Minimal recovery SoftAP: serves a single upload form and the OTA endpoint.
//No WiFi client, no I2C traffic, no persistence — just enough to let the
//user reflash a working image without dragging out the USB cable.
void enterRecoveryMode() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAP("split-flap-recovery");
  SerialPrint(F("Recovery SoftAP IP: "));
  SerialPrintln(WiFi.softAPIP().toString());

  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
    String html =
      "<!doctype html><html><head><title>Split-Flap Recovery</title>"
      "<meta name='viewport' content='width=device-width, initial-scale=1'/>"
      "<style>body{font-family:sans-serif;max-width:480px;margin:2em auto;padding:1em}"
      "button{padding:.5em 1em}</style></head><body>"
      "<h1>Split-Flap Recovery</h1>"
      "<p>Device entered recovery after repeated failed boots. "
      "Upload a known-good <code>firmware.bin</code> to recover.</p>"
      "<form method='post' action='/firmware/master' enctype='multipart/form-data'>"
      "<p><input type='file' name='firmware' accept='.bin' required/></p>"
      "<p><button type='submit'>Flash firmware</button></p>"
      "</form></body></html>";
    request->send(200, "text/html", html);
  });
  registerMasterFirmwareEndpoint();
  webServer.begin();
  SerialPrintln(F("Recovery web server ready"));
}

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
  SerialPrintln(F(""));
  SerialPrintln(F("#######################################################"));
  SerialPrintln(F("..............Split Flap Display Starting.............."));
  SerialPrintln(F("#######################################################"));

  //Capture the reset cause for the PREVIOUS reboot before anything else
  //can touch the RTC. Reported in /settings so a remote flasher can tell
  //"Software Watchdog" / "Exception" (crash-revert) apart from "External
  //System" (clean user-triggered reboot). See #52.
  lastResetReason = ESP.getResetReason();
  SerialPrint(F("Last reset reason: "));
  SerialPrintln(lastResetReason);

  //Increment the RTC boot counter before anything else can crash. The main
  //loop will clear it again once HEALTHY_BOOT_MS of uptime proves the sketch
  //is behaving. See issue #37.
  RtcBootState bootState = readBootStateRtc();
  bootState.bootCounter++;
  writeBootStateRtc(bootState);
  SerialPrint(F("RTC boot counter: "));
  SerialPrintln(bootState.bootCounter);

  if (bootState.bootCounter >= RECOVERY_BOOT_THRESHOLD) {
    SerialPrintln(F("#######################################################"));
    SerialPrintln(F("RECOVERY MODE: 3+ consecutive boots without a healthy"));
    SerialPrintln(F("loop. Bringing up split-flap-recovery SoftAP for reflash."));
    SerialPrintln(F("#######################################################"));
    isRecoveryMode = true;
    enterRecoveryMode();
    return;
  }

  //Early-boot I2C scan — run before WiFi/NTP so we fall inside twiboot's
  //~1-second boot window. On simultaneous power-cycle the Nanos come up in
  //twiboot first and only jump to their sketch after TIMEOUT_MS; if we waited
  //for WiFi + NTP (up to 20 s combined) they'd be long gone and we'd never
  //catch old-firmware units that need updating. The 200 ms delay gives the
  //Nanos' TWI hardware time to come up after their reset.
  //See issue #30.
  SerialPrintln(F("Early I2C scan (twiboot window)..."));
  delay(200);
  probeI2cBus();
  autoInstallFirmwareToBootloaderUnits();

  //Load and read all the things
  initWiFi();
  
  //Helpful if want to force reset WiFi settings for testing
  //wifiManager.resetSettings();

  if (isWifiConfigured && !isPendingReboot) {
    //Load persisted settings first so the runtime timezone (if set via
    //the web UI) takes effect on this boot's configTime() call. Issue #48.
    initialiseFileSystem();
    loadValuesFromFileSystem();

    //OTA revert detection (#52). If the last `/firmware/master` call
    //persisted an intended GIT_REV and we're now running a *different* rev,
    //the new image was staged but didn't stick (eboot rejected it, or it
    //crashed fast enough to trip recovery and revert to the prior slot).
    //Empty intendedVersionEeprom = no flash has happened on this EEPROM
    //yet -> skip the check, not a false-positive.
    intendedVersionEeprom = readIntendedVersion();
    otaReverted = (intendedVersionEeprom.length() > 0 &&
                   intendedVersionEeprom != String(espVersion));
    if (otaReverted) {
      SerialPrint(F("OTA REVERTED: intended="));
      SerialPrint(intendedVersionEeprom);
      SerialPrint(F(", running="));
      SerialPrintln(espVersion);
    }

    //Time sync via ESP8266 core + libc time.h
    applyTimezoneAndNtp();

    time_t nowSec = 0;
    for (int i = 0; i < 100 && nowSec < 1000000000L; i++) {
      delay(100);
      nowSec = time(nullptr);
    }

    SerialPrint(F("Current time: "));
    SerialPrintln(formatDateTime("%Y-%m-%d %H:%M:%S"));

    //Re-scan the I2C bus now that WiFi/NTP are up and any early-boot
    //auto-install has settled. This refreshes /settings with the final
    //post-install state — including firmware versions for newly installed
    //units, which couldn't be queried yet during the first scan.
    SerialPrintln(F("Settled I2C scan (post-WiFi)..."));
    probeI2cBus();
    //autoInstallFirmwareToBootloaderUnits() was already called from the
    //early-boot path — any still-in-bootloader units here are either a)
    //new arrivals during boot, or b) failed on the early pass. Re-run so
    //they get a second chance.
    autoInstallFirmwareToBootloaderUnits();

    //Auto-update any sketch-running unit whose firmware rev doesn't match
    //the bundled one. Runs once after the settled probe so a single stale
    //unit mixed into a fresh-flashed display self-heals without a
    //manual "Flash all unit(s)" click. Issue #32.
    autoUpdateOutdatedUnits();

#if USE_MULTICAST == true
  if (MDNS.begin(mdnsName)) {
      SerialPrintln(F("mDNS responder started"));
    } else {
      SerialPrintln(F("Error setting up MDNS responder!"));
    }
#endif

    //Web Server Endpoint configuration — all assets served from PROGMEM
    //(generated by build_assets.py into WebAssets.h). Gzipped entries are
    //sent with Content-Encoding: gzip; browsers unpack transparently.
    //Static assets live in PROGMEM and only change via a master OTA.
    //Tell the browser to revalidate every navigation so an OTA that
    //swaps the UI doesn't leave stale HTML/JS in the tab cache.
    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
      SerialPrintln(F("Request Home Page Received"));
      AsyncWebServerResponse *resp = request->beginResponse_P(200, "text/html", INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
      resp->addHeader("Content-Encoding", "gzip");
      resp->addHeader("Cache-Control", "no-cache");
      request->send(resp);
    });
    webServer.on("/index.html", HTTP_GET, [](AsyncWebServerRequest * request) {
      AsyncWebServerResponse *resp = request->beginResponse_P(200, "text/html", INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
      resp->addHeader("Content-Encoding", "gzip");
      resp->addHeader("Cache-Control", "no-cache");
      request->send(resp);
    });
    webServer.on("/script.js", HTTP_GET, [](AsyncWebServerRequest * request) {
      AsyncWebServerResponse *resp = request->beginResponse_P(200, "application/javascript", SCRIPT_JS_GZ, SCRIPT_JS_GZ_LEN);
      resp->addHeader("Content-Encoding", "gzip");
      resp->addHeader("Cache-Control", "no-cache");
      request->send(resp);
    });
    webServer.on("/style.css", HTTP_GET, [](AsyncWebServerRequest * request) {
      AsyncWebServerResponse *resp = request->beginResponse_P(200, "text/css", STYLE_CSS_GZ, STYLE_CSS_GZ_LEN);
      resp->addHeader("Content-Encoding", "gzip");
      resp->addHeader("Cache-Control", "no-cache");
      request->send(resp);
    });
    webServer.on("/favicon.png", HTTP_GET, [](AsyncWebServerRequest * request) {
      request->send_P(200, "image/png", FAVICON_PNG, FAVICON_PNG_LEN);
    });

    webServer.on("/settings", HTTP_GET, [](AsyncWebServerRequest * request) {
      SerialPrintln(F("Request for Settings Received"));
      
      String json = getCurrentSettingValues();
      request->send(200, "application/json", json);
      json = String();
    });
    
    webServer.on("/health", HTTP_GET, [](AsyncWebServerRequest * request) {
      SerialPrintln(F("Request for Health Check Received"));
      request->send(200, "text/plain", "Healthy");
    });

    webServer.on("/log", HTTP_GET, [](AsyncWebServerRequest * request) {
      //Don't SerialPrintln here; every log request would otherwise stamp
      //itself into the buffer on every poll and drown out real activity.
      request->send(200, "text/plain", webLogRead());
    });
    
    webServer.on("/reboot", HTTP_GET, [](AsyncWebServerRequest * request) {
      SerialPrintln(F("Request to Reboot Received"));
      
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
    
    //GET /debug/ota — dumps the flash-layout numbers the Update class cares
    //about so we can tell from a browser whether web OTA should work at all.
    webServer.on("/debug/ota", HTTP_GET, [](AsyncWebServerRequest * request) {
      uint32_t freeSpace    = ESP.getFreeSketchSpace();
      uint32_t maxSketch    = (freeSpace - 0x1000) & 0xFFFFF000;
      String body;
      body += "sketchSize        = "; body += ESP.getSketchSize();         body += "\n";
      body += "freeSketchSpace   = "; body += freeSpace;                   body += "\n";
      body += "maxSketchSpace    = "; body += maxSketch;                   body += " (what Update.begin uses)\n";
      body += "flashChipSize     = "; body += ESP.getFlashChipSize();      body += "\n";
      body += "flashChipRealSize = "; body += ESP.getFlashChipRealSize();  body += "\n";
      body += "sketchMD5         = "; body += ESP.getSketchMD5();          body += "\n";
      body += "coreVersion       = "; body += ESP.getCoreVersion();        body += "\n";
      body += "version           = "; body += espVersion;                  body += "\n";
      request->send(200, "text/plain", body);
    });

    //POST /firmware/master — web OTA for the ESP itself. Receives a .bin
    //via multipart upload, flashes via the ESP8266 Update class, reboots.
    //Usable because the eagle.flash.1m.ld (no-FS) layout leaves ~1 MB for
    //sketch+staging — the 443 KB firmware has room for another 443 KB
    //staged image below _FS_start (which is 0xFB000 with no FS).
    //Handler lives in registerMasterFirmwareEndpoint() so recovery mode
    //can mount the same upload path on a minimal SoftAP. See issue #37.
    registerMasterFirmwareEndpoint();

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

    //Interactive calibration endpoints (issue #32). All four share the
    //same preconditions: valid I2C address in the 1..126 range, and the
    //unit must be a sketch-running responder (we don't expose these to
    //units in bootloader mode or missing entirely). Guarded against the
    //firmware-flash window so we don't collide on the Wire bus.
    auto parseCalibrationAddress = [](AsyncWebServerRequest * request, int &outAddr) -> bool {
      if (firmwareFlashInProgress) {
        request->send(503, "text/plain", "Unit firmware flash in progress — try again in a moment");
        return false;
      }
      if (!request->hasParam("address")) {
        request->send(400, "text/plain", "Missing 'address' query param");
        return false;
      }
      long parsed = strtol(request->getParam("address")->value().c_str(), nullptr, 0);
      if (parsed < 1 || parsed > 126) {
        request->send(400, "text/plain", "Address must be 1..126");
        return false;
      }
      int unitIndex = (int)parsed - 1;  // I2C_ADDRESS_BASE == 1
      if (unitIndex < 0 || unitIndex >= UNITS_AMOUNT ||
          detectedUnitStates[unitIndex] != 1) {
        request->send(404, "text/plain", "No sketch-running unit at that address");
        return false;
      }
      outAddr = (int)parsed;
      return true;
    };

    webServer.on("/unit/offset", HTTP_GET, [parseCalibrationAddress](AsyncWebServerRequest * request) {
      int addr = 0;
      if (!parseCalibrationAddress(request, addr)) return;
      int16_t offset = 0;
      if (!readUnitOffset(addr, offset)) {
        request->send(502, "text/plain", "Unit did not return a valid offset (firmware may predate #32)");
        return;
      }
      String body = "{\"offset\":";
      body += offset;
      body += "}";
      request->send(200, "application/json", body);
    });

    webServer.on("/unit/offset", HTTP_POST, [parseCalibrationAddress](AsyncWebServerRequest * request) {
      int addr = 0;
      if (!parseCalibrationAddress(request, addr)) return;
      if (!request->hasParam("value")) {
        request->send(400, "text/plain", "Missing 'value' query param");
        return;
      }
      long parsed = strtol(request->getParam("value")->value().c_str(), nullptr, 0);
      if (parsed < -32768 || parsed > 32767) {
        request->send(400, "text/plain", "Value must fit in int16 (-32768..32767)");
        return;
      }
      int status = writeUnitOffset(addr, (int16_t)parsed);
      if (status != 0) {
        String body = "Wire.endTransmission returned ";
        body += status;
        request->send(502, "text/plain", body);
        return;
      }
      request->send(200, "text/plain", "Offset written");
    });

    webServer.on("/unit/jog", HTTP_POST, [parseCalibrationAddress](AsyncWebServerRequest * request) {
      int addr = 0;
      if (!parseCalibrationAddress(request, addr)) return;
      if (!request->hasParam("steps")) {
        request->send(400, "text/plain", "Missing 'steps' query param");
        return;
      }
      long parsed = strtol(request->getParam("steps")->value().c_str(), nullptr, 0);
      if (parsed < -127 || parsed > 127) {
        request->send(400, "text/plain", "Steps must be -127..127");
        return;
      }
      int status = jogUnit(addr, (int)parsed);
      if (status != 0) {
        String body = "Wire.endTransmission returned ";
        body += status;
        request->send(502, "text/plain", body);
        return;
      }
      request->send(200, "text/plain", "Jogged");
    });

    webServer.on("/unit/home", HTTP_POST, [parseCalibrationAddress](AsyncWebServerRequest * request) {
      int addr = 0;
      if (!parseCalibrationAddress(request, addr)) return;
      int status = homeUnit(addr);
      if (status != 0) {
        String body = "Wire.endTransmission returned ";
        body += status;
        request->send(502, "text/plain", body);
        return;
      }
      request->send(200, "text/plain", "Home requested");
    });

    //POST /stop — user-visible kill-switch (issue #35). Aborts any running
    //showMessage() wait loop, broadcasts CMD_HOME to every unit in one I2C
    //transaction (issue #47 — previously N sequential sends), and clears
    //the in-memory text so the event loop doesn't immediately re-send the
    //previous message. Safe to call when nothing is happening; idempotent.
    webServer.on("/stop", HTTP_POST, [](AsyncWebServerRequest * request) {
      if (firmwareFlashInProgress) {
        request->send(503, "text/plain", "Unit firmware flash in progress — try again in a moment");
        return;
      }
      SerialPrintln(F("Request to Stop Received"));
      abortCurrentShow = true;
      int broadcastStatus = broadcastHome();
      //Prevent the event loop from re-issuing showText() with the previous
      //content. Clearing both inputText and lastWrittenText makes the
      //showText("" vs "") comparison a no-op.
      inputText = "";
      lastWrittenText = "";
      String body;
      if (broadcastStatus == 0) {
        body = F("Stop requested; homed all units via broadcast.");
      } else {
        body = String(F("Stop requested; broadcast Wire.endTransmission returned ")) + broadcastStatus;
      }
      request->send(200, "text/plain", body);
    });

    //GET /reflash-units — forces every sketch-running unit into its twiboot
    //bootloader and then re-runs the PROGMEM auto-install. Use after a master
    //OTA that ships a new bundled unit firmware if the chained reboot didn't
    //catch every unit, or to manually refresh units without a master reboot.
    webServer.on("/reflash-units", HTTP_GET, [](AsyncWebServerRequest * request) {
      SerialPrintln(F("Request to Reflash Units Received"));
      int rebooted = enterBootloaderAllDetected(true);
      autoInstallFirmwareToBootloaderUnits();
      String body = String("Requested bootloader entry on ") + rebooted +
                    " unit(s); re-probed and triggered PROGMEM auto-install. " +
                    "Watch the Log panel for per-unit results.";
      request->send(200, "text/plain", body);
    });

    webServer.on("/reset-units", HTTP_GET, [](AsyncWebServerRequest * request) {
      SerialPrintln(F("Request to Reset Units Received"));
      
      //This will be picked up in the loop
      isPendingUnitsReset = true;
      
      request->redirect("/?is-resetting-units=true");
    });

    webServer.on("/", HTTP_POST, [](AsyncWebServerRequest * request) {
      SerialPrintln(F("Request Post of Form Received"));

      bool submissionError = false;

      String newAlignmentValue, newDeviceModeValue, newFlapSpeedValue, newInputTextValue, newTimezoneValue;
      bool timezoneProvided = false;

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
            if (receivedValue == DEVICE_MODE_TEXT || receivedValue == DEVICE_MODE_CLOCK) {
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

          //HTTP POST timezone POSIX TZ string (issue #48).
          //Accept empty (= UTC fallback) or a printable-ASCII string
          //short enough to fit the EEPROM slot; anything longer/weirder
          //is rejected rather than silently truncated.
          if (p->name() == PARAM_TIMEZONE) {
            String receivedValue = p->value();
            bool valid = (int)receivedValue.length() < LEN_TIMEZONE;
            for (size_t i = 0; valid && i < receivedValue.length(); i++) {
              char c = receivedValue[i];
              if (c < 0x20 || c > 0x7E) valid = false;
            }
            if (valid) {
              newTimezoneValue = receivedValue;
              timezoneProvided = true;
            } else {
              SerialPrintln("Timezone provided was not valid. Value: " + receivedValue);
              submissionError = true;
            }
          }
        }
      }

      //If there was an error, report back to check what has been input
      if (submissionError) {
        SerialPrintln(F("Finished Processing Request with Error"));
        request->redirect("/?invalid-submission=true");
      }
      else {
        SerialPrintln(F("Finished Processing Request Successfully"));

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

        //Only if device mode has changed
        if (deviceMode != newDeviceModeValue) {
          deviceMode = newDeviceModeValue;

          saveDeviceMode();
          SerialPrintln("Device Mode Set: " + deviceMode);
        }

        //Only if a new timezone value was submitted and it changed.
        //Re-apply configTime() so the clock picks up the new zone on
        //its next tick — no reboot required. Issue #48.
        if (timezoneProvided && timezonePosixSetting != newTimezoneValue) {
          timezonePosixSetting = newTimezoneValue;
          saveTimezone();
          applyTimezoneAndNtp();
          SerialPrintln("Timezone Updated: " + (timezonePosixSetting.length() ? timezonePosixSetting : String("(default)")));
        }

        //Only if we are showing text
        if (deviceMode == DEVICE_MODE_TEXT) {
          inputText = newInputTextValue;
        }

        //Redirect so that we don't have the "re-submit form" problem in browser for refresh
        request->redirect("/");
      }
    });

#if WIFI_USE_DIRECT == false
    webServer.on("/reset-wifi", HTTP_GET, [](AsyncWebServerRequest * request) {
      SerialPrintln(F("Request to Reset WiFi Received"));
      
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

    SerialPrintln(F("Split Flap Ready!"));
    SerialPrintln(F("#######################################################"));
  }
  else {
    if (isPendingReboot) {
      SerialPrintln(F("Reboot is pending to be able to continue device function. Hold please..."));
      SerialPrintln(F("#######################################################"));
    }
    else {
      SerialPrintln(F("Unable to connect to WiFi... Not starting web server"));
      SerialPrintln(F("Please hard restart your device to try connect again"));
      SerialPrintln(F("#######################################################"));
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
    SerialPrintln(F("Rebooting Now... Fairwell!"));
    SerialPrintln(F("#######################################################"));
    //Longer pause so AsyncWebServer can flush the 200 OK body to the client
    //before the restart yanks the TCP socket (issue #37). 100 ms was enough
    //on localhost but often cost the response on real networks.
    delay(500);

    ESP.restart();
    return;
  }

  //Once the sketch has been running cleanly for long enough, clear the RTC
  //boot counter so future cold resets start from zero. This is the signal
  //that the currently-running firmware actually works (issue #37). Skipped
  //in recovery mode: we want a user-visible flash (or a new firmware that
  //runs cleanly) to exit, not just 30 s of the recovery SoftAP being up.
  if (!healthyBootMarked && !isRecoveryMode && millis() >= HEALTHY_BOOT_MS) {
    clearBootCounterRtc();
    healthyBootMarked = true;
    SerialPrintln(F("Healthy boot — RTC boot counter cleared"));
  }

  //In recovery mode the AsyncWebServer handles every incoming request; the
  //main loop has nothing to do besides yield back to the scheduler.
  if (isRecoveryMode) {
    delay(10);
    return;
  }

#if WIFI_USE_DIRECT == false
  //Clear off the WiFi Manager Settings
  if (isPendingWifiReset) {
    SerialPrintln(F("Removing WiFi settings"));
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
    SerialPrintln(F("Reseting Units now..."));

    //Blank out the message
    String blankOutText1 = createRepeatingString('-');
    showText(blankOutText1);
    delay(2000);

    //Do just enough to do a full iteration which triggers the re-calibration
    String blankOutText2 = createRepeatingString('.');
    showText(blankOutText2);

    //We did a reset!
    isPendingUnitsReset = false;

    SerialPrintln(F("Done Units Reset!"));
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

    //Mode Selection
    if (deviceMode == DEVICE_MODE_TEXT) {
      showText(inputText);
    }
    else if (deviceMode == DEVICE_MODE_CLOCK) {
      showText(formatDateTime(clockFormat));
    }
  }
}

//Appends a JSON-encoded string literal (including the surrounding quotes)
//to `out`. Handles the escape sequences that can legitimately appear in
//user-provided text (quotes, backslashes, control chars). Good enough for
//the fixed shape of /settings — ArduinoJson would be overkill given we only
//serialize, never parse.
static void appendJsonString(String& out, const String& value) {
  out += '"';
  for (unsigned int i = 0; i < value.length(); i++) {
    char c = value[i];
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b";  break;
      case '\f': out += "\\f";  break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((unsigned char)c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

//Gets all the currently stored values from memory as a JSON string.
//Hand-rolled to avoid pulling in the ArduinoJson library (~30 KB) for a
//fixed-shape serializer that never needs to parse (issue #40).
String getCurrentSettingValues() {
  String out;
  out.reserve(512);
  out += '{';

  out += F("\"timezoneOffset\":");          out += getTimezoneOffsetMinutes();
  out += F(",\"unitCount\":");              out += UNITS_AMOUNT;
  out += F(",\"detectedUnitCount\":");      out += detectedUnitCount;

  out += F(",\"detectedUnitAddresses\":[");
  for (int i = 0; i < detectedUnitCount; i++) {
    if (i) out += ',';
    out += detectedUnitAddresses[i];
  }
  out += ']';

  //Per-unit firmware version, indexed by unit slot (0..UNITS_AMOUNT-1) so
  //the UI can show a badge per address even for silent units (issue #28).
  out += F(",\"detectedUnitVersionStatus\":[");
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    if (i) out += ',';
    out += detectedUnitVersionStatus[i];
  }
  out += F("],\"detectedUnitVersions\":[");
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    if (i) out += ',';
    appendJsonString(out, detectedUnitVersions[i]);
  }
  out += ']';

  out += F(",\"alignment\":");                       appendJsonString(out, alignment);
  out += F(",\"flapSpeed\":");                       appendJsonString(out, flapSpeed);
  out += F(",\"deviceMode\":");                      appendJsonString(out, deviceMode);
  out += F(",\"timezonePosix\":");                   appendJsonString(out, timezonePosixSetting);
  out += F(",\"version\":");                         appendJsonString(out, String(espVersion));
  //OTA failure diagnostics (#52). These fields let a remote flasher tell a
  //genuine revert apart from a same-version false-alarm. `sketchMd5` is the
  //MD5 of the running sketch as read from flash (via ESP.getSketchMD5(),
  //cached by core on first call) — unambiguous identity check independent
  //of GIT_REV strings, added in #53.
  out += F(",\"sketchMd5\":");                       appendJsonString(out, ESP.getSketchMD5());
  out += F(",\"intendedVersion\":");                 appendJsonString(out, intendedVersionEeprom);
  out += F(",\"otaReverted\":");                     out += (otaReverted ? F("true") : F("false"));
  out += F(",\"lastResetReason\":");                 appendJsonString(out, lastResetReason);
  out += F(",\"bootCounter\":");                     out += String(readBootStateRtc().bootCounter);
  out += F(",\"recoveryMode\":");                    out += (isRecoveryMode ? F("true") : F("false"));
  out += F(",\"lastTimeReceivedMessageDateTime\":"); appendJsonString(out, lastReceivedMessageDateTime);
  out += F(",\"lastWrittenText\":");                 appendJsonString(out, lastWrittenText);

  out += F(",\"otaEnabled\":false");
  out += F(",\"isInOtaMode\":false");

#if WIFI_USE_DIRECT == false
  out += F(",\"wifiSettingsResettable\":true");
#else
  out += F(",\"wifiSettingsResettable\":false");
#endif

  out += '}';
  return out;
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
