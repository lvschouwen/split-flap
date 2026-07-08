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

// Single source of truth for the master<->unit I2C contract (opcodes,
// address base, alphabet, flap count) — shared with firmware/v1/Unit and
// verified against data/script.js at build time (#149). Pure macros, no
// system deps, so it is safe this early (before the config block below uses
// SFP_FLAP_AMOUNT / SFP_ALPHABET).
#include "SplitFlapProtocol.h"

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
#define UNITS_AMOUNT        16      //Hardware ceiling: max units the DIP-switch addressing supports (4 bits -> I2C 0x01..0x10). Array bound only — the effective display width is detected at boot from the I2C probe (#123), so one image fits every display size. Do not lower per-display.
#define SERIAL_BAUDRATE     115200  //Serial debugging BAUD rate
#define USE_MULTICAST       true    //Option to broadcast a ".local" URL on your local network default split-flap.local. You can change the name under configurable settings. On by default since #112 — the begin/update plumbing existed all along and this makes the display findable without knowing its DHCP lease.

//MQTT / Home Assistant integration (#121, runtime config #57). Always
//compiled in; idle unless a broker host is configured via the web UI
//(persisted to EEPROM, applied on reboot). The former MQTT_ENABLE build
//gate and MqttCredentials.h header are gone.
#define MQTT_TELEMETRY_INTERVAL_S 60   //Seconds between MQTT health telemetry publishes
#define MQTT_MAX_TEXT_LEN         256  //Inbound MQTT payload buffer cap (bytes)

//PlatformIO's .ino -> .cpp converter (InoToCPPConverter in pioino.py) scans
//the WHOLE merged translation unit with a regex for anything shaped like a
//function definition/declaration, then inserts an extern-prototype block
//right before the very FIRST such match -- entirely blind to #if/#ifdef. It
//finds the AsyncMqttClient callbacks defined further down in
//ServiceMqttFunctions.ino (onMqttDisconnect/onMqttMessage) before
//AsyncMqttClient.h (which defines their parameter types) has textually
//appeared in the merged file — the header is included by
//ServiceMqttFunctions.ino, far below where the generator inserts its
//prototype block. Without this forward declaration the phantom prototype's
//unknown-type parameter fails to parse; GCC's error recovery then treats
//the unresolved type as an implicit-int guess, so the phantom prototype
//and the real later definition become two *different* declarations of the
//same name -> "unresolved overloaded function type" where the real code
//registers the callback.
//Forward-declaring here (before any #include, so it precedes wherever the
//generator's textual scan finds its first match) makes the phantom
//prototype and the real AsyncMqttClient.h definition refer to the
//identical type. Plain
//`unsigned char` (not uint8_t) because no header has been included yet;
//AsyncMqttClient's real enum also has an `unsigned char`-compatible
//underlying type, so the later full definition is a legal completion, not a
//redefinition conflict.
enum class AsyncMqttClientDisconnectReason : unsigned char;
struct AsyncMqttClientMessageProperties;

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
#define FLAP_AMOUNT         SFP_FLAP_AMOUNT  //Amount of Flaps per unit — derived from SFP_ALPHABET (SplitFlapProtocol.h, #149)
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

//WiFi setup-portal library (#126: always compiled in — the captive portal
//is the runtime fallback whenever the SDK-persisted credentials are absent
//or won't connect, not a build-time either/or anymore).
//Specifically put here in this order to avoid conflict with other libraries
#include <DNSServer.h>
#include <ESPAsyncWiFiManager.h>

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
#include "DeviceIdentity.h"
#include "MdnsDiscovery.h"
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
//WiFi credentials are NOT normally compiled in (#126). The SDK's flash
//config sector is the single credential store: the setup portal writes it
//(ESPAsyncWiFiManager wraps its connect in WiFi.persistent(true)), a bare
//WiFi.begin() reads it back, and it survives reboots and sketch OTAs.
//
//One exception: an optional MIGRATION SEED. Pre-#126 firmware supplied its
//compiled credentials with persistence OFF (the core default), i.e. RAM
//only — so a device upgraded over the air may have nothing usable in the
//SDK sector. If a gitignored WifiCredentials.h is present at build time,
//initWiFi() tries (and this time persists) its credentials when the stored
//ones fail, before falling back to the portal. Fresh checkouts and CI
//build without it; once the seed has persisted, the header can be deleted.
#if __has_include("WifiCredentials.h")
  #include "WifiCredentials.h"
  #define WIFI_SEED_AVAILABLE 1
#else
  #define WIFI_SEED_AVAILABLE 0
#endif

//MQTT broker config is runtime now (#57): host/port/user/password live in
//EEPROM, set via the web UI, applied on reboot. Empty host → initMqtt()
//logs and disables itself.

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

//Prefix seed for the per-device identity (#125). The effective name every
//network-facing consumer uses is resolved at boot by resolveDeviceIdentity():
//the EEPROM deviceName (set via the web UI) if present, else
//"<mdnsName>-<hex chip id>" — unique per device out of the box, so multiple
//displays can share one LAN on one firmware image without editing this.
const char* mdnsName = "split-flap";

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

//The drum's fixed character set — the master sends an index into this table.
//Sourced from SFP_ALPHABET so master, unit and script.js can never drift
//(#149). To change the alphabet, edit SplitFlapProtocol.h, not here.
const char letters[] = SFP_ALPHABET;
//Effective display width in character slots (#123): highest unit index the
//boot-time I2C probe saw + 1 (a dead unit mid-display keeps its slot).
//Every text-layout helper targets this instead of the UNITS_AMOUNT ceiling,
//so a 5-unit display no longer centers text across absent units. Falls back
//to UNITS_AMOUNT when the probe finds nothing (bench ESP, SERIAL_ENABLE).
//Set by probeI2cBus(); declared extern in HelpersStringHandling.ino.
int displayWidth = UNITS_AMOUNT;
//Units whose most recent letter write failed at the Wire level
//(endTransmission != 0) during the last showMessage() pass. Surfaced via
//MQTT health telemetry (#121); harmless standalone counter otherwise.
int lastShowUnitWriteErrors = 0;
unsigned long previousMillis = 0;

//Search for parameter in HTTP POST request
const char* PARAM_ALIGNMENT = "alignment";
const char* PARAM_FLAP_SPEED = "flapSpeed";
const char* PARAM_DEVICEMODE = "deviceMode";
const char* PARAM_INPUT_TEXT = "inputText";
//Transient text (#165/#176) — calibration patterns and timed messages,
//shown via the notification show-then-revert state, never persisted.
const char* PARAM_TRANSIENT_TEXT  = "transientText";
const char* PARAM_TRANSIENT_DWELL = "transientDwell";
const char* PARAM_TIMEZONE   = "timezone";
const char* PARAM_DEVICE_NAME = "deviceName";
const char* PARAM_MQTT_HOST     = "mqttHost";
const char* PARAM_MQTT_PORT     = "mqttPort";
const char* PARAM_MQTT_USER     = "mqttUser";
const char* PARAM_MQTT_PASSWORD = "mqttPassword";

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
//Per-device identity (#125). deviceNameSetting mirrors the raw EEPROM slot
//(may be empty = "use chip-id default"); effectiveDeviceName is resolved
//once by resolveDeviceIdentity() before any network bring-up and read by
//every consumer: mDNS, WiFi hostname, MQTT client id / topics, and the
//recovery / quiet-OTA / captive-portal AP SSIDs. A rename saves to EEPROM
//and applies on the next reboot — this global never changes mid-run.
String deviceNameSetting = "";
String effectiveDeviceName = "";
//MQTT broker config (#57), loaded from EEPROM. Empty host = MQTT disabled.
//Changes via the web UI persist immediately but apply on the next reboot —
//initMqtt() copies them into its own stable Strings at boot (AsyncMqttClient
//stores raw pointers, so it must never observe these being reassigned).
String mqttHostSetting = "";
String mqttPortSetting = "";
String mqttUserSetting = "";
String mqttPasswordSetting = "";
String lastWrittenText = "";
String lastReceivedMessageDateTime = "";
bool alignmentUpdated = false;
//Set from async web handlers, drained by loop() — volatile like every other
//cross-context flag (isPendingStop and isPendingWifiReset likewise, below).
volatile bool isPendingReboot = false;
volatile bool isPendingUnitsReset = false;
bool isWifiConfigured = false;

//Deferred settings apply (#150). The async POST / handler only parses and
//validates; every accepted field is staged here and applied from loop() by
//applyPendingSettingsPost() — shared-String mutation, EEPROM commits and
//configTime must not run in async_tcp context. A second POST arriving
//before the drain overlays its fields (the provided flags accumulate);
//`pending` is set last by the handler and cleared by the drain.
struct PendingSettingsPost {
  volatile bool pending = false;
  bool alignmentProvided = false;    String alignment;
  bool flapSpeedProvided = false;    String flapSpeed;
  bool deviceModeProvided = false;   String deviceMode;
  bool inputTextProvided = false;    String inputText;
  bool transientTextProvided = false; String transientText; long transientDwell = 0;
  bool timezoneProvided = false;     String timezone;
  bool deviceNameProvided = false;   String deviceName;
  bool mqttHostProvided = false;     String mqttHost;
  bool mqttPortProvided = false;     String mqttPort;
  bool mqttUserProvided = false;     String mqttUser;
  bool mqttPasswordProvided = false; String mqttPassword;
};
PendingSettingsPost pendingSettingsPost;

//OTA failure diagnostics (#52, extended in #53). Captured at boot; exposed
//via /settings so the flasher can tell a silent revert apart from a clean
//reboot.
//  lastResetReason       — ESP.getResetReason() at the top of setup()
//  intendedVersionEeprom — GIT_REV we told the device to become via /firmware/master?v=
//  otaReverted           — true iff intendedVersionEeprom is non-empty AND
//                          differs from the running GIT_REV (i.e. the upload
//                          "took" from the handler's perspective but the new
//                          image isn't actually running now).
//  lastFlashResult       — resolved on boot from the pre-flash RTC cookie:
//                          "" = no flash attempt pending / unrelated boot,
//                          "ok" = new sketch MD5 differs from cookie (flash took),
//                          "reverted" = MD5 matches cookie (eboot rejected the
//                          staged image). See #53.
String lastResetReason = "";
String intendedVersionEeprom = "";
bool otaReverted = false;
String lastFlashResult = "";

//Set by POST /stop to break out of the wait loop inside showMessage() when
//a unit gets physically stuck and its status byte pegs at "rotating"
//forever. Checked every ~100 ms; cleared at the start of each new
//showMessage. Issue #35.
volatile bool abortCurrentShow = false;
//Deferred /stop tail (#150): the handler flips abortCurrentShow (polled
//inside showMessage's wait loop, so the abort is immediate) but the
//broadcast-home I2C transaction and the shared-text clears run from loop().
volatile bool isPendingStop = false;

//Recovery mode + boot-loop protection (issue #37). Counter lives in RTC user
//memory (survives warm restart, cleared on cold power cycle). Every boot
//increments; a loop that runs cleanly for HEALTHY_BOOT_MS clears it. If the
//counter reaches RECOVERY_BOOT_THRESHOLD, the device skips the main app and
//brings up a minimal "<deviceName>-rec" SoftAP that only accepts a master
//firmware upload.
//Layout + pure helpers live in RtcBootState.h so they can be exercised by
//host-side tests (`pio test -e native`). Hardware-touching read/write
//wrappers stay here.
#include "RtcBootState.h"

bool isRecoveryMode = false;
//Quiet OTA mode (issue #117): user-requested reboot into a minimal
//wait-for-image environment — no I2C, no NTP wait, no display traffic.
bool isOtaMode = false;

//Freeze-during-upload state (issue #116). While a master firmware image is
//streaming in, the loop stops all display/unit work — stepper current +
//WiFi TX + flash-write bursts on one small supply is exactly the storm that
//endangers a flash. masterOtaLastChunkMs lets the loop auto-thaw if the
//client dies mid-upload and the completion handler never fires.
volatile bool masterOtaUploadActive = false;
volatile unsigned long masterOtaLastChunkMs = 0;
bool healthyBootMarked = false;

//Master OTA rejection state — set in the upload handler when the content
//length exceeds the sketch slot or the caller-supplied MD5 is malformed.
//Post-handler reads it to return 413/400 instead of the generic 500.
static bool otaRejected = false;
static int otaRejectionStatus = 0;
static String otaRejectionReason;

//WiFi TX power management during OTA upload (#60). ESP-01 has no onboard
//regulator; simultaneous WiFi RX + flash writes can sag VCC below brownout
//and corrupt the staged image. Dropping TX power for the upload phase
//reduces peak current without breaking LAN connectivity (10 dBm ≈ 10 mW,
//still plenty for same-LAN / same-router reach). Does NOT help the eboot
//copy-at-boot phase — that runs with no WiFi active.
static bool otaTxPowerReduced = false;
static constexpr float OTA_TX_POWER_DBM = 10.0f;
static constexpr float DEFAULT_TX_POWER_DBM = 20.5f;

//Create AsyncWebServer object on port 80
AsyncWebServer webServer(80);

//Used for creating a Access Point to allow WiFi setup. ESPAsyncWiFiManager
//reuses the AsyncWebServer above for its captive portal so we don't carry
//a second (sync) HTTP server in the binary.
DNSServer       dnsServer;
AsyncWiFiManager wifiManager(&webServer, &dnsServer);
volatile bool isPendingWifiReset = false;

//MQTT broker auto-detect (#129). The async handler only arms the flag —
//MDNS.queryService() blocks ~1 s per query (up to ~2 s when the fallback
//query also runs), which must not happen in async_tcp context — and loop()
//does the work (runPendingMqttDiscovery). The result JSON is cached until
//the next POST /mqtt/discover re-arms.
volatile bool mqttDiscoverPending = false;
String mqttDiscoverResultJson = "";

//Read boot state from RTC user memory. Returns a fully-zeroed state with a
//fresh magic if the stored magic doesn't match (cold power-on, corruption,
//ESP8266 core using the block for something else, or a firmware upgrade
//that bumped the magic — see #53).
RtcBootState readBootStateRtc() {
  RtcBootState state;
  ESP.rtcUserMemoryRead(RTC_BOOT_OFFSET_BLOCKS,
                        reinterpret_cast<uint32_t*>(&state),
                        sizeof(state));
  normalizeBootState(state);
  return state;
}

void writeBootStateRtc(RtcBootState state) {
  ESP.rtcUserMemoryWrite(RTC_BOOT_OFFSET_BLOCKS,
                         reinterpret_cast<uint32_t*>(&state),
                         sizeof(state));
}

void clearBootCounterRtc() {
  RtcBootState state = readBootStateRtc();
  state.bootCounter = 0;
  writeBootStateRtc(state);
}

//Set the flash-outcome cookie used by the boot-time revert detector
//(#53, reworked in #118). Called from the /firmware/master success path
//just before ESP.restart(). With COOKIE_KIND_EXPECTED_MD5 the caller
//passes the uploaded image's MD5 (from ?md5=) and boot reports "ok" iff
//the running MD5 matches it — unambiguous even for a same-image reflash.
//With COOKIE_KIND_PRE_FLASH (md5-less web-form uploads) the caller passes
//the pre-reboot ESP.getSketchMD5() and a post-boot match means the new
//image didn't take. RTC memory survives a warm restart, which is exactly
//the eboot-revert failure path.
void setFlashCookieRtc(const String& md5, uint32_t kind) {
  RtcBootState state = readBootStateRtc();
  setPreFlashMd5(state, md5.c_str());
  state.cookieKind = kind;
  writeBootStateRtc(state);
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

  //Settings EEPROM comes up once here, for every boot path — the single
  //begin + migration point. Quiet-OTA/recovery need it for /firmware/
  //master's intendedVersion/lastFlashResult writes (#119), and the
  //identity resolver right after needs a migrated blob for the deviceName
  //slot; its result feeds each mode's AP SSID and initWiFi()'s hostname.
  //Deliberately AFTER the RTC counter increment above so a crash in here
  //still counts toward the recovery threshold.
  initialiseSettings();
  resolveDeviceIdentity();

  //User-requested quiet OTA mode (#117). One-shot: clear the flag (and the
  //boot counter this entry just bumped) immediately, so any reboot or power
  //cycle out of OTA mode lands in a normal boot — no way to get stuck.
  if (bootState.bootMode == BOOT_MODE_OTA) {
    bootState.bootMode = BOOT_MODE_NORMAL;
    bootState.bootCounter = 0;
    writeBootStateRtc(bootState);
    SerialPrintln(F("#######################################################"));
    SerialPrintln(F("OTA MODE: quiet flash environment. No I2C, no clock,"));
    SerialPrintln(F("no display traffic — waiting for a firmware image."));
    SerialPrintln(F("#######################################################"));
    isOtaMode = true;
    enterOtaMode();
    return;
  }

  if (bootState.bootCounter >= RECOVERY_BOOT_THRESHOLD) {
    SerialPrintln(F("#######################################################"));
    SerialPrintln(F("RECOVERY MODE: 3+ consecutive boots without a healthy"));
    SerialPrintln(F("loop. Bringing up recovery SoftAP for reflash."));
    SerialPrintln(F("#######################################################"));
    isRecoveryMode = true;
    enterRecoveryMode();
    return;
  }

  //Early-boot I2C scan — runs before WiFi/NTP so we provision blank-app
  //units (twiboot's stay-alive-on-empty-flash patch keeps them in the
  //bootloader indefinitely) without making the user wait for WiFi/NTP.
  //
  //We deliberately wait LONGER than twiboot's TIMEOUT_MS (1000 ms) before
  //probing. Reasoning:
  //  - Already-installed units exit twiboot to their sketch at ~1000 ms.
  //    Probing later means we see them in state=1 (sketch) and skip the
  //    auto-install path. autoUpdateOutdatedUnits() handles version
  //    upgrades via the proper version-check path.
  //  - Blank-app units stay in twiboot forever, so a longer delay still
  //    catches them — they're flashed by autoInstallFirmwareToBootloaderUnits().
  //
  //Probing inside the twiboot window is *harmful* for already-installed
  //units: isUnitInBootloader() sends CMD_ACCESS_MEMORY (0x02), which in
  //twiboot's first-byte switch falls through to the CMD_WAIT case and
  //pins boot_timeout=0 — keeping twiboot alive forever and triggering an
  //unnecessary re-flash on every cold boot. See issue #88.
  //See also issue #30 for the original "early scan" rationale.
  SerialPrintln(F("Early I2C scan (post-twiboot window)..."));
  delay(1500);
  probeI2cBus();
  autoInstallFirmwareToBootloaderUnits();

  //Load and read all the things
  initWiFi();
  
  //Helpful if want to force reset WiFi settings for testing
  //wifiManager.resetSettings();

  if (isWifiConfigured && !isPendingReboot) {
    //Load persisted settings first so the runtime timezone (if set via
    //the web UI) takes effect on this boot's configTime() call. Issue #48.
    //(EEPROM itself came up at the top of setup().)
    loadSettings();

    //Flash-outcome cookie check (#53/#118). If the OTA handler stashed a
    //cookie in RTC, resolveFlashVerdict() decides the outcome according to
    //the cookie kind (expected-new-MD5 vs legacy pre-flash-MD5 — see
    //RtcBootState.h). Write the verdict to EEPROM so /settings can surface
    //it even after another reboot, then clear the cookie so a later
    //unrelated warm reboot doesn't re-trigger the check. A malformed
    //cookie resolves to "" (unknown) — which still overwrites the ""
    //the handler staged, never a stale verdict.
    RtcBootState bootStateNow = readBootStateRtc();
    {
      String runningMd5 = ESP.getSketchMD5();
      const char* verdict = resolveFlashVerdict(bootStateNow, runningMd5.c_str());
      if (verdict != nullptr) {
        //Bounded copy of the cookie for logging only. A raw SerialPrint of
        //the slot would walk until the first NUL, which if RTC is corrupt
        //could run past the struct. See #53.
        size_t ckLen = cookieLength(bootStateNow);
        String cookieStr;
        if (ckLen < PRE_FLASH_MD5_LEN) {
          cookieStr.reserve(ckLen);
          for (size_t i = 0; i < ckLen; i++) cookieStr += bootStateNow.preFlashSketchMd5[i];
        } else {
          cookieStr = F("<unterminated>");
        }
        SerialPrint(F("Flash cookie resolved: "));
        SerialPrint(verdict[0] == '\0' ? "unknown" : verdict);
        SerialPrint(F(" (kind="));
        SerialPrint(bootStateNow.cookieKind);
        SerialPrint(F(", cookie="));
        SerialPrint(cookieStr);
        SerialPrint(F(", running="));
        SerialPrint(runningMd5);
        SerialPrintln(F(")"));
        saveLastFlashResult(verdict);
        //Verdict "ok" proves the flash we're adjudicating took, so the
        //running rev IS the intended rev. Heal the slot (#119) — it can
        //hold a stale value when the upload was handled by an environment
        //that couldn't persist ?v= (a pre-#119 quiet-OTA/recovery boot, or
        //any pre-#118 firmware), which would otherwise leave a permanent
        //false "OTA REVERTED" in /settings.
        if (strcmp(verdict, "ok") == 0) {
          saveIntendedVersion(String(espVersion));
        }
        memset(bootStateNow.preFlashSketchMd5, 0, PRE_FLASH_MD5_LEN);
        bootStateNow.cookieKind = COOKIE_KIND_NONE;
        writeBootStateRtc(bootStateNow);
      }
    }
    //Load the most-recent resolved result (may be "" if no flash has ever
    //happened on this EEPROM blob) for /settings.
    lastFlashResult = readLastFlashResult();

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
  if (MDNS.begin(effectiveDeviceName.c_str())) {
      SerialPrintln(F("mDNS responder started"));
    } else {
      SerialPrintln(F("Error setting up MDNS responder!"));
    }
#endif

    registerWebEndpoints();

    delay(250);
    webServer.begin();

    //Show the freshly acquired IP on the flaps so the display can be found
    //after any DHCP change (issue #111). Blocks for ~15 s of flap time, but
    //the web server is already up and serving.
    showIpAddressOnBoot();

    SerialPrintln(F("Split Flap Ready!"));
    SerialPrintln(F("#######################################################"));

    //MQTT / Home Assistant integration (#121). Normal boots only — the
    //quiet-OTA and recovery paths returned out of setup() long before here.
    initMqtt();
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
    //Flush any settings staged by POST / before restarting (#150). Every
    //reboot trigger funnels through this branch, and it runs before the
    //normal drain point further down — without this, a save whose client
    //was already told "ok"/"ok-reboot" would be lost if a reboot (e.g. the
    //post-OTA one, set while the upload freeze was skipping the drain)
    //fires first. isPendingStop is deliberately NOT flushed: the parking
    //wait below already settles the display, and the text clears are
    //meaningless across a restart.
    applyPendingSettingsPost();
#if SERIAL_ENABLE == false
    //Park the display before restarting (issue #116). Eboot's staged-image
    //copy runs immediately after reset with no sketch in control — make
    //sure no stepper is energized and sagging the rail during it. Bounded
    //wait; absent/silent units are skipped inside isDisplayMoving().
    if (!isRecoveryMode && !isOtaMode) {
      unsigned long parkStart = millis();
      while (isDisplayMoving() && millis() - parkStart < 15000UL) {
        delay(100);
      }
    }
#endif
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
  if (!healthyBootMarked && !isRecoveryMode && !isOtaMode && millis() >= HEALTHY_BOOT_MS) {
    clearBootCounterRtc();
    healthyBootMarked = true;
    SerialPrintln(F("Healthy boot — RTC boot counter cleared"));
  }

  //In recovery mode and OTA mode the AsyncWebServer handles every incoming
  //request; the main loop has nothing to do besides yield to the scheduler.
  if (isRecoveryMode || isOtaMode) {
    delay(10);
    return;
  }

  //Clear off the WiFi Manager Settings — erases the SDK-persisted
  //credentials, so the next boot lands in the "<deviceName>-setup" portal.
  if (isPendingWifiReset) {
    SerialPrintln(F("Removing WiFi settings"));
    wifiManager.resetSettings();
    delay(100);

    isPendingReboot = true;
    return;
  }

#if USE_MULTICAST == true
  MDNS.update();
#endif

  //Freeze all display/unit activity while a master firmware upload is
  //streaming in (issue #116). Auto-thaw after 30 s without a chunk so a
  //client that died mid-upload can't wedge the display forever.
  if (masterOtaUploadActive) {
    if (millis() - masterOtaLastChunkMs > 30000UL) {
      SerialPrintln(F("OTA upload stalled >30 s — resuming normal operation"));
      masterOtaUploadActive = false;
      mqttResumeAfterOta();
    } else {
      delay(50);
      return;
    }
  }

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

  //Deferred /stop tail (#150): abortCurrentShow was already flipped in the
  //handler; the bus work + shared-String clears happen here, after the
  //flash-window early-return so the broadcast owns the Wire bus cleanly.
  if (isPendingStop) {
    isPendingStop = false;
    int broadcastStatus = broadcastHome();
    if (broadcastStatus == 0) {
      SerialPrintln(F("Stop: homed all units via broadcast"));
    } else {
      SerialPrintln("Stop: broadcast Wire.endTransmission returned " + String(broadcastStatus));
    }
    //Prevent the event loop from re-issuing showText() with the previous
    //content. Clearing both inputText and lastWrittenText makes the
    //showText("" vs "") comparison a no-op.
    inputText = "";
    lastWrittenText = "";
  }

#if USE_MULTICAST == true
  //Deferred MQTT broker discovery (#129). Runs here — never in the async
  //handler — because MDNS.queryService() blocks ~1 s per query (~2 s worst
  //case with the home-assistant fallback). Placement after the recovery/
  //OTA/upload/unit-flash early-returns means a discovery can never stall
  //those paths.
  runPendingMqttDiscovery();
#endif

  //Deferred + throttled unit reflash (#138): same rule as MQTT discovery — the
  //multi-second blocking flash runs here, never in the async handler. Placed
  //after the firmwareFlashInProgress early-return so it owns the bus cleanly.
  runPendingUnitReflash();

  //On-demand unit-health refresh (#45): the blocking per-unit CMD_GET_STATUS
  //poll runs here, never in the async GET/POST handler. POST /units/health/refresh
  //arms the flag; the MQTT telemetry tick (loopMqtt) also refreshes this same
  //cache on its 60 s cadence (#137). Placed after the flash early-returns/ reflash
  //so it owns the bus cleanly.
  if (unitHealthRefreshPending) {
    unitHealthRefreshPending = false;
    i2cBusBusy = true;
    //Re-probe first when armed (#56): an address change moved a unit, and
    //pollUnitHealth() only reads slots the last probe marked sketch-running.
    if (busReprobePending) {
      busReprobePending = false;
      probeI2cBus();
    }
    pollUnitHealth();
    i2cBusBusy = false;
  }

  //Deferred settings apply (#150): drain the fields staged by POST / —
  //EEPROM commits and configTime run here, never in the async handler.
  applyPendingSettingsPost();

  //MQTT pump (#121): reconnect schedule, inbound notifications, telemetry.
  //No-op when no broker is configured (#57).
  loopMqtt();

  //Process every second
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= 1000) {
    previousMillis = currentMillis;

    //Mode Selection. An active MQTT notification (#121) temporarily owns
    //the display; when it expires the normal mode content re-flaps via
    //showText's lastWrittenText comparison (show-then-revert).
    if (!mqttNotificationTick()) {
      if (deviceMode == DEVICE_MODE_TEXT) {
        showText(inputText);
      }
      else if (deviceMode == DEVICE_MODE_CLOCK) {
        showText(formatDateTime(clockFormat));
      }
    }
  }
}
