#pragma once
// WallActions.h — the actions of POST /api/v2/action that are done the moment
// they are accepted (#559/#572): what the wall shows, its mode, quiet. Pure,
// natively tested by test_wall_actions. Each becomes the same staged post the
// settings change makes (PendingSettingsPost.h), so the checks and the drain
// are the same.
//
//   {"name":"show","args":{"text":"HELLO\nWORLD"}}
//       the wall's text until something else is shown; switches to text mode.
//       A "\n" starts the next row of the wall
//   {"name":"show","args":{"text":"DINNER","forS":300}}
//       for 5 to 3600 seconds, then back to what the mode shows; on every
//       row of the wall
//   {"name":"mode","args":{"mode":"clock"}}        clock | text
//   {"name":"quiet","args":{"on":true}}
// How long a text may be is the request body's limit; the wall shows what
// fits on it.

#include <ArduinoJson.h>
#include <string.h>

#include "PendingSettingsPost.h"

inline bool wallActionIsContent(const char* name) {
  return strcmp(name, "show") == 0 || strcmp(name, "mode") == 0 || strcmp(name, "quiet") == 0;
}

// Is every key under args one of these two (nullptr = not used)?
inline bool wallActionOnlyKeys(JsonVariantConst args, const char* a, const char* b) {
  for (JsonPairConst pair : args.as<JsonObjectConst>()) {
    const char* key = pair.key().c_str();
    if (strcmp(key, a) != 0 && (b == nullptr || strcmp(key, b) != 0)) return false;
  }
  return true;
}

// Fills `post` from the action's args. Returns why not, nullptr when it can
// be staged. `post` is untouched on a refusal.
inline const char* wallActionBuild(const char* name, JsonVariantConst args,
                                   PendingSettingsPost& post) {
  PendingSettingsPost built;
  if (strcmp(name, "show") == 0) {
    if (!wallActionOnlyKeys(args, "text", "forS")) return "show takes args.text and args.forS";
    JsonVariantConst text = args["text"];
    if (!text.is<const char*>()) return "args.text is the text to show";
    JsonVariantConst forS = args["forS"];
    if (forS.isNull()) {
      stageSettingsParam(built, PARAM_INPUT_TEXT, String(text.as<const char*>()));
      stageSettingsParam(built, PARAM_DEVICEMODE, String("text"));
    } else {
      // A float is no number of seconds, and neither is a string of digits.
      if (!forS.is<long>() ||
          stageSettingsParam(built, PARAM_TRANSIENT_DWELL, String(forS.as<long>())) !=
              SettingsParamResult::Accepted) {
        return "args.forS is a whole number of seconds, 5 to 3600";
      }
      stageSettingsParam(built, PARAM_TRANSIENT_TEXT, String(text.as<const char*>()));
    }
  } else if (strcmp(name, "mode") == 0) {
    if (!wallActionOnlyKeys(args, "mode", nullptr)) return "mode takes args.mode only";
    JsonVariantConst mode = args["mode"];
    if (!mode.is<const char*>() ||
        stageSettingsParam(built, PARAM_DEVICEMODE, String(mode.as<const char*>())) !=
            SettingsParamResult::Accepted) {
      return "args.mode is \"clock\" or \"text\"";
    }
  } else if (strcmp(name, "quiet") == 0) {
    if (!wallActionOnlyKeys(args, "on", nullptr)) return "quiet takes args.on only";
    JsonVariantConst on = args["on"];
    if (!on.is<bool>()) return "args.on is true or false";
    stageSettingsParam(built, PARAM_QUIET, String(on.as<bool>() ? "true" : "false"));
  } else {
    return "no such action";
  }
  post = built;
  return nullptr;
}
