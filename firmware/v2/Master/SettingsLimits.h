#pragma once
// SettingsLimits.h — maximum-length bounds for validated settings fields.
//
// The values are the v1 EEPROM slot lengths, kept as pure validation limits
// after the storage moved to NVS (#185): the SettingsValidation.h validators
// take these as their `slotLen` parameter and accept values STRICTLY SHORTER
// than the limit (the extra byte is the NUL terminator of the slot heritage).
// Keeping the numbers identical means a value accepted by a v2 master is
// also representable on a v1 device and vice versa — one wire contract for
// /settings across both firmware generations.

#define LEN_TIMEZONE           40
#define LEN_DEVICE_NAME        25
#define LEN_INTENDED_VERSION   24
#define LEN_LAST_FLASH_RESULT  16
#define LEN_MQTT_HOST          65
#define LEN_MQTT_PORT           6
#define LEN_MQTT_USER          33
#define LEN_MQTT_PASSWORD      65
