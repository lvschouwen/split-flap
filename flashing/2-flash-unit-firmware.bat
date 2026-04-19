@echo off
REM Step 2 of the one-time flash: installs the Unit.ino sketch on a single
REM Nano via Arduino-as-ISP. Assumes step 1 (twiboot bootloader) has been
REM run on this Nano. After step 2, this Nano is fully set up and future
REM firmware updates can go over I2C via the master's web UI.

setlocal
call "%~dp0config.bat"

echo.
echo === Step 2/2 per Nano: install Unit.ino sketch ===
echo.
echo Which COM port is your Arduino-as-ISP programmer on?
echo (example: COM4 — same port as step 1 if you haven't replugged)
set /p ISP_PORT=COM port:
if "%ISP_PORT%"=="" (
  echo No port entered; aborting.
  pause
  exit /b 1
)

echo.
echo === Flashing Unit.ino to Nano via %ISP_PORT% ===
echo.

"%AVRDUDE%" -C "%AVRDUDE_CONF%" -c stk500v1 -P %ISP_PORT% -b 19200 -p m328p ^
  -U flash:w:"%~dp0prebuilt\unit-firmware.hex":i
if errorlevel 1 goto :fail

echo.
echo === Done. Unit sketch installed. ===
echo     Set the DIP switches on this Nano to its address (see README.md)
echo     and move on to the next Nano, or run 3-flash-master.bat if all
echo     Nanos are done.
echo.
pause
exit /b 0

:fail
echo.
echo *** FAILED *** See error above. Same debugging applies as step 1 —
echo common cause is missing reset cap or wrong COM port.
echo.
pause
exit /b 1
