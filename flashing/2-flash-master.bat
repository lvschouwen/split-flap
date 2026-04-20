@echo off
REM Step 3 of the one-time flash: writes the master firmware + LittleFS web UI
REM to the ESP-01 via your USB-to-UART adapter. The ESP-01 must be in
REM programming mode (GPIO0 tied low while power/reset is applied).
REM
REM After this runs successfully, power-cycle the ESP normally (GPIO0 high),
REM connect to its "Split-Flap-AP" WiFi network, configure your real WiFi via
REM the captive portal, and the master is live.

setlocal
call "%~dp0config.bat"

echo.
echo === Flashing master firmware + web UI to ESP-01 ===
echo.
echo Which COM port is your USB-to-UART adapter on?
echo (example: COM5 — check Device Manager if unsure)
set /p ESP_PORT=COM port:
if "%ESP_PORT%"=="" (
  echo No port entered; aborting.
  pause
  exit /b 1
)

echo.
echo Make sure the ESP is in PROGRAMMING MODE (GPIO0 tied to GND at boot).
echo.

REM --before no_reset / --after no_reset because most cheap USB-UART adapters
REM don't have DTR/RTS wired to the ESP's RESET/GPIO0 lines. You put the ESP
REM in programming mode manually (jumper GPIO0 to GND, power-cycle) before
REM running this; after it finishes, remove the jumper and power-cycle to boot.
"%PYTHON%" "%ESPTOOL%" ^
  --chip esp8266 ^
  --port %ESP_PORT% ^
  --baud 115200 ^
  --before no_reset --after no_reset ^
  write_flash ^
  0x0      "%~dp0prebuilt\master-firmware.bin" ^
  0xBB000  "%~dp0prebuilt\master-littlefs.bin"
if errorlevel 1 goto :fail

echo.
echo === Done. Master firmware + web UI installed. ===
echo  1. Remove GPIO0-GND jumper, power-cycle the ESP.
echo  2. From phone/laptop: join WiFi network "Split-Flap-AP" (no password).
echo  3. Captive portal opens — enter your real WiFi credentials.
echo  4. Master reboots onto your WiFi. Check your router for its IP and
echo     browse to http://ITS.IP.ADDRESS/ for the dashboard.
echo.
pause
exit /b 0

:fail
echo.
echo *** FAILED *** See error above. Common causes:
echo   - ESP not in programming mode (GPIO0 must be LOW at power-up)
echo   - Wrong COM port
echo   - Insufficient power — ESP-01 can brownout on cheap USB-UARTs
echo   - python or esptool paths in config.bat are wrong
echo.
pause
exit /b 1
