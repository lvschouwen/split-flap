@echo off
REM Step 1 of the one-time flash: installs twiboot (the I2C bootloader) on a
REM single Nano via Arduino-as-ISP. Repeat this for every physical Nano.
REM Also sets fuses for 16MHz crystal + bootloader section.

setlocal
call "%~dp0config.bat"

echo.
echo === Step 1/2 per Nano: install twiboot bootloader ===
echo.
echo Which COM port is your Arduino-as-ISP programmer on?
echo (example: COM4 — check Device Manager if unsure)
set /p ISP_PORT=COM port:
if "%ISP_PORT%"=="" (
  echo No port entered; aborting.
  pause
  exit /b 1
)

echo.
echo === Flashing twiboot to Nano via %ISP_PORT% ===
echo.

"%AVRDUDE%" -C "%AVRDUDE_CONF%" -c stk500v1 -P %ISP_PORT% -b 19200 -p m328p ^
  -U flash:w:"%~dp0prebuilt\twiboot-atmega328p-16mhz.hex":i
if errorlevel 1 goto :fail

echo.
echo === Setting fuses (LFUSE=0xFF, HFUSE=0xDA, EFUSE=0xFD) ===
echo.

"%AVRDUDE%" -C "%AVRDUDE_CONF%" -c stk500v1 -P %ISP_PORT% -b 19200 -p m328p ^
  -U lfuse:w:0xff:m -U hfuse:w:0xda:m -U efuse:w:0xfd:m
if errorlevel 1 goto :fail

echo.
echo === Done. Twiboot is installed on this Nano. ===
echo     The Unit sketch is NOT flashed via ICSP — the master will push it
echo     over I2C automatically on first boot. Set this Nano's DIP switches,
echo     repeat for the next Nano, then run 2-flash-master.bat when all
echo     Nanos are done.
echo.
pause
exit /b 0

:fail
echo.
echo *** FAILED *** See error above. Common causes:
echo   - Wrong COM port
echo   - 10uF reset-disable cap not installed between RESET and GND of the programmer
echo   - ICSP wiring wrong (MOSI/MISO swapped is the classic)
echo   - Target Nano not powered via 5V from the programmer
echo.
pause
exit /b 1
