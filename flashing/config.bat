@echo off
REM -----------------------------------------------------------------------
REM Split-Flap one-time flash — configuration
REM -----------------------------------------------------------------------
REM Tool paths only. COM ports are asked interactively by each flash script
REM so you don't have to edit this file between Nanos. If a path below is
REM wrong for your setup, override it here.

REM === Path to avrdude.exe ===============================================
REM Default: PlatformIO's bundled avrdude (present if you've installed PIO
REM for the Unit sketch). Arduino IDE users can point this at
REM %LocalAppData%\Arduino15\packages\arduino\tools\avrdude\<ver>\bin\avrdude.exe
set AVRDUDE=%USERPROFILE%\.platformio\packages\tool-avrdude\avrdude.exe
set AVRDUDE_CONF=%USERPROFILE%\.platformio\packages\tool-avrdude\avrdude.conf

REM === Python (needed to run esptool for the master flash) ================
REM PlatformIO installs Python; if `python` isn't on PATH try `py` or `python3`.
set PYTHON=python

REM === Path to esptool.py (bundled with PlatformIO) =======================
set ESPTOOL=%USERPROFILE%\.platformio\packages\tool-esptoolpy\esptool.py
