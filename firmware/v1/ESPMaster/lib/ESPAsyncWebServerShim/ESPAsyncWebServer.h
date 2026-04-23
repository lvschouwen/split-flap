// Compatibility shim: ESPAsyncWiFiManager #includes <ESPAsyncWebServer.h>
// but we depend on the dvarrel fork which ships as <ESPAsyncWebSrv.h>.
// Same API, different library name.
#pragma once
#include <ESPAsyncWebSrv.h>
