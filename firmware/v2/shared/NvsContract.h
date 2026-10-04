#pragma once
// NvsContract.h — the NVS namespace and keys of an S3 board that more than
// one image reads. The Master app writes them; the Rescue app in the factory
// slot reads them to find its name, join the same WiFi and rank the OTA
// slots. A key renamed in one image only would leave a rescue that boots
// into an AP nobody expects, so both spell them from here.

#define SF_NVS_NAMESPACE "splitflap"

// Identity + WiFi credentials (Master Settings.h writes, Rescue reads).
#define SF_NVS_KEY_DEVICE_NAME "deviceName"
#define SF_NVS_KEY_WIFI_SSID "wifiSsid"
#define SF_NVS_KEY_WIFI_PASS "wifiPass"

// Slot records (SlotRecord.h format): the Master stamps app0/app1 when it
// confirms an image and the factory slot when it installs a rescue image;
// Rescue ranks /rescue/exit by the app records.
#define SF_NVS_KEY_SLOT_REC_APP0 "slotRec0"
#define SF_NVS_KEY_SLOT_REC_APP1 "slotRec1"
#define SF_NVS_KEY_SLOT_REC_FACTORY "slotRecF"
