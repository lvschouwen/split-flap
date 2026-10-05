#pragma once
// FollowerWeb.h — the routes this board keeps without a master (everything
// else reaches it over the wall link, FollowerLink.h): POST /firmware/master
// (the recovery path; ?md5= mandatory), GET /settings (who this board is and
// what it runs) and POST /pair. The WiFi setup portal registers its own
// (FollowerWifi.cpp). Rescue mode serves the same set. Async rule: handlers
// validate and stage; loop() writes flash records and restarts the board.

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

extern volatile bool isPendingReboot;

void webEndpointsInit(AsyncWebServer& server);

// True while a firmware upload is streaming in — loop() freezes all
// display/unit work (v1 #116). Owns the 30 s stalled-upload auto-thaw,
// including freeing the Update session slot (v1 #191: a dangling owner
// would 409 every later upload forever).
bool webOtaUploadFrozen();

// Running image + stored upload share [0, this) of the flash.
uint32_t appAreaBytes();
