#pragma once
// WallSettingsJson.h — the settings as the operator API reads and writes
// them (#559/#572). Pure, natively tested by test_wall_settings_json. A PUT
// carries any part of what the GET gives; every value takes its field's check
// (PendingSettingsPost.h) and one that fails refuses the whole request.
//
// GET / PUT /api/v2/settings/wall
//   {"mode":"clock","quiet":false,"alignment":"center","speed":80,
//    "timezone":"CET-1CEST,M3.5.0,M10.5.0/3","updateUnitsAtStart":true,
//    "releaseCheck":true,"releaseChannel":"stable",
//    "mqtt":{"host":"192.168.1.4","port":1883,"user":"splitflap",
//            "passwordSet":true}}
//       timezone is the POSIX rule (GET /tz.json has one for every zone
//       name); mqtt.password is write-only, "" keeps the stored one;
//       updateUnitsAtStart is for every board of the wall; releaseCheck is
//       the daily look for a release, releaseChannel "stable" or "test"
// GET / PUT /api/v2/settings/board/<master's name>
//   {"name":"","unitCount":0}
//       name "" = the name from the chip's id; unitCount 0 = as many as answer
// A change to the name or to mqtt takes effect at the next restart: the PUT
// answers {"done":true,"restart":true} then.

#include <ArduinoJson.h>
#include <string.h>

#include "PendingSettingsPost.h"

namespace wallsettings {

enum class Kind : uint8_t { Text, Number, Flag };

struct Key {
  const char* json;
  const char* param;  // the field it is checked and staged as
  Kind kind;
};

static const Key WALL_KEYS[] = {
    {"mode", PARAM_DEVICEMODE, Kind::Text},
    {"quiet", PARAM_QUIET, Kind::Flag},
    {"alignment", PARAM_ALIGNMENT, Kind::Text},
    {"speed", PARAM_FLAP_SPEED, Kind::Number},
    {"timezone", PARAM_TIMEZONE, Kind::Text},
    {"updateUnitsAtStart", PARAM_REFLASH_ON_BOOT, Kind::Flag},
    {"releaseCheck", PARAM_RELEASE_CHECK, Kind::Flag},
    {"releaseChannel", PARAM_RELEASE_CHANNEL, Kind::Text},
};
static const Key MQTT_KEYS[] = {
    {"host", PARAM_MQTT_HOST, Kind::Text},
    {"port", PARAM_MQTT_PORT, Kind::Number},
    {"user", PARAM_MQTT_USER, Kind::Text},
    {"password", PARAM_MQTT_PASSWORD, Kind::Text},
};
static const Key BOARD_KEYS[] = {
    {"name", PARAM_DEVICE_NAME, Kind::Text},
    {"unitCount", PARAM_UNIT_COUNT, Kind::Number},
};

// The longest key named in a refusal; a longer one is cut.
#define WALL_SETTINGS_KEY_MAX 24

struct Refused {
  char key[WALL_SETTINGS_KEY_MAX + 1] = {0};  // "" = the body is no object
  // Copied: the name of a key that is no setting lives in the request's body.
  bool at(const char* name) {
    strncpy(key, name, WALL_SETTINGS_KEY_MAX);
    key[WALL_SETTINGS_KEY_MAX] = 0;
    return false;
  }
};

// Stages every pair of `object` that `keys` names. False, with the key in
// `refused`, when one could not be taken. `also` is one more key the caller
// reads itself.
template <size_t N>
inline bool stage(JsonObjectConst object, const Key (&keys)[N], const char* also,
                  PendingSettingsPost& post, Refused& refused) {
  for (JsonPairConst pair : object) {
    const char* name = pair.key().c_str();
    if (also != nullptr && strcmp(name, also) == 0) continue;
    const Key* key = nullptr;
    for (const Key& k : keys) {
      if (strcmp(k.json, name) == 0) key = &k;
    }
    if (key == nullptr) return refused.at(name);
    JsonVariantConst v = pair.value();
    String text;
    switch (key->kind) {
      case Kind::Text:
        if (!v.is<const char*>()) return refused.at(key->json);
        text = v.as<const char*>();
        break;
      case Kind::Number:
        if (!v.is<long>()) return refused.at(key->json);
        text = String(v.as<long>());
        break;
      case Kind::Flag:
        if (!v.is<bool>()) return refused.at(key->json);
        text = v.as<bool>() ? "true" : "false";
        break;
    }
    if (stageSettingsParam(post, key->param, text) == SettingsParamResult::Invalid) {
      return refused.at(key->json);
    }
  }
  return true;
}

}  // namespace wallsettings

typedef wallsettings::Refused WallSettingsRefused;

// Fills `post` from a PUT body. False when it cannot be staged, with the key
// that was refused; `post` is untouched then.
inline bool wallSettingsBuild(JsonVariantConst body, PendingSettingsPost& post,
                              WallSettingsRefused& refused) {
  refused = WallSettingsRefused();
  if (!body.is<JsonObjectConst>()) return false;
  PendingSettingsPost built;
  if (!wallsettings::stage(body.as<JsonObjectConst>(), wallsettings::WALL_KEYS, "mqtt", built,
                           refused)) {
    return false;
  }
  JsonVariantConst mqtt = body["mqtt"];
  if (!mqtt.isNull()) {
    if (!mqtt.is<JsonObjectConst>()) return refused.at("mqtt");
    if (!wallsettings::stage(mqtt.as<JsonObjectConst>(), wallsettings::MQTT_KEYS, "passwordSet",
                             built, refused)) {
      return false;
    }
  }
  post = built;
  return true;
}

inline bool boardSettingsBuild(JsonVariantConst body, PendingSettingsPost& post,
                               WallSettingsRefused& refused) {
  refused = WallSettingsRefused();
  if (!body.is<JsonObjectConst>()) return false;
  PendingSettingsPost built;
  if (!wallsettings::stage(body.as<JsonObjectConst>(), wallsettings::BOARD_KEYS, nullptr, built,
                           refused)) {
    return false;
  }
  post = built;
  return true;
}

inline void wallSettingsWrite(JsonObject out, const MasterSettings& s) {
  out["mode"] = s.deviceMode;
  out["quiet"] = s.quiet;
  out["alignment"] = s.alignment;
  out["speed"] = s.flapSpeed;
  out["timezone"] = s.timezonePosix;
  out["updateUnitsAtStart"] = s.reflashOnBoot;
  out["releaseCheck"] = s.releaseCheck;
  out["releaseChannel"] = s.releaseChannel;
  JsonObject mqtt = out["mqtt"].to<JsonObject>();
  mqtt["host"] = s.mqttHost;
  mqtt["port"] = s.mqttPort;
  mqtt["user"] = s.mqttUser;
  mqtt["passwordSet"] = s.mqttPassword.length() > 0;
}

inline void boardSettingsWrite(JsonObject out, const MasterSettings& s) {
  out["name"] = s.deviceName;
  out["unitCount"] = s.unitCountOverride;
}
