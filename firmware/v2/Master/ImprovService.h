#pragma once
// ImprovService.h — WiFi credentials over USB (#601): the serial side of
// ImprovPolicy.h. Listens on both of the devkit's USB ports (the native one
// and UART0, whichever the board was flashed through), answers on the port
// that asked, and hands a name and password to WifiService exactly as the
// setup page does. It never touches the radio or the settings store.

#include <Arduino.h>

// setup(), after uartLogBegin() and before tasksInit().
void improvServiceInit(const String& deviceName);

// netTask only, after wifiServiceTick() (it reads that tick's phase).
void improvServiceTick();
