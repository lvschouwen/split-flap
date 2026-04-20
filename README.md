# Split-Flap

[![Build](https://github.com/lvschouwen/split-flap/actions/workflows/build.yml/badge.svg)](https://github.com/lvschouwen/split-flap/actions/workflows/build.yml)

![Split Flap Display](./Images/Split-Flap.jpg)

---
**NOTE:** I will make my best attempt to provide answers where possible in issues and try to keep things up to date where possible dependency wise, however I do not have the time/capacity to work on this project on a consistent basis. Answers and updates may take time.

My personal Split-Flap has been working since I first published this repository and continues to work to this day happily. Several people have also successfully been in touch with their working creations of this Split-Flap which I am very happy it has worked for :) 

I hope this project continues to work for any whom wish to embark on it but be aware, it is a lot of work to complete.

Thank you everyone whom has contributed, included in the "main" release or not, it is very appreciated. I hope to keep the project going in future.

<3

---

This project has been forked from the brilliant [Split Flap Project](https://github.com/Dave19171/split-flap) by [David Königsmann](https://github.com/Dave19171). None of this would have been possible without the great foundations that have been put in place.

This project has built on the original project to add extra features such as:

- Message Splittng
  - If a message is longer then the number of units there are, the message will be split up and displayed in sequence with a delay between each message
  - Also messages can be split up by adding a `\n`
- Added ability to setup WiFi connection on device
  - The device will set itself up as a Access Point (AP) on first start. You will be able to connect to this network and a web portal will be provided where you can setup the WiFi network you want to connect to
  - If the device was to lose connection, it should retry and if all else fails, it will open up the web portal again to change the WiFi settings if necessary
  - Option to be able use direct mode is also possible as previously implemented
- Reworked UI
  - Can see messages scheduled to be displayed and option to remove them
  - Loading indicators
  - Show hide information to fill out for the mode you select
  - See extra information on the UI such as:
    - Last Message Received
    - Number of Flaps registered
    - Version Number running
    - How many characters/lines are in the textbox for text
    - Add newline button (as typing `\n` is a pain on a mobile keyboard)
- Web OTA
  - Upload a new `firmware.bin` from the browser; the ESP reboots into it.
  - Optional `?md5=<hex>` query param verifies image integrity before eboot commits it.
  - Size-checks the upload against the sketch slot before writing a single byte.
  - Recovery SoftAP (`split-flap-recovery`) comes up after 3 consecutive failed boots so a bad firmware can always be reflashed without USB.
  - Also pushes the bundled unit firmware to every Nano it detects on the bus, so unit updates ride along with master updates.
- I2C OTA for unit firmware
  - Master ships a compiled Unit sketch in PROGMEM and auto-installs it on any Nano sitting in twiboot (the [patched bootloader](./UnitBootloader/README.md) is flashed once per unit via ICSP).
- Updated `README.md` to add scenarios of problems encountered

Also the code has been refactored to try facilitate easier development:

- Changed serial prints to one central location so don't have to declare serial enable checks when a new one is required
- Renamed files and functions
- Ping endpoint
- Updated `data` so can test out project locally without having to call a webserver via a `localDevelopment` flag

3D-files here on [Printables](https://www.prusaprinters.org/prints/69464-split-flap-display)!

## Building the firmware

The project is built with [PlatformIO](https://platformio.org/). You no longer need the Arduino IDE — `pio` handles dependencies, builds, and flashing from the command line.

Install PlatformIO Core (once):

```bash
pip install -U platformio
```

Each of the three sketches has its own `platformio.ini` in its folder. From the folder, run:

```bash
pio run                   # build
pio run -t upload         # flash firmware over USB
pio device monitor        # serial monitor at 115200 baud
```

The web UI and the bundled unit firmware are compiled into the master's PROGMEM at build time by `ESPMaster/build_assets.py` (no separate filesystem flash step).

For the Unit and EEPROM_Write_Offset sketches, if upload fails because of the old bootloader on your Nano, use the `*_old_bootloader` env:

```bash
pio run -e unit_old_bootloader -t upload
```

Host-side unit tests for the ESPMaster string helpers live in `ESPMaster/test/` and run with:

```bash
cd ESPMaster && pio test -e native
```

Arduino IDE is **not** supported for the master sketch any more: the PROGMEM asset generation runs as a PlatformIO pre-build step, and the build flags / lib dependencies are managed through `platformio.ini`. The Unit and EEPROM_Write_Offset sketches are simple enough to still open in the IDE, but the PlatformIO flow is the supported path for every part of the project.

## General

The display's electronics use 1 x ESP01 (ESP8266) as the main hub and up to 16 Arduinos as receivers. The ESP handles the web interface and communicates to the units via I2C. Each unit is resposible for setting the zero position of the drum on startup and displaying any letter the main hub send its way.

Assemble everything according to the instruction manual which you can find on [GitHub](./Instructions/SplitFlapInstructions.pdf).

### PCB

Gerber files are in the `PCB` folder. These are the scehematics for the PCB boards and say, what is needed and where. You need one per unit. Populate components according to the [instruction manual](./Instructions/SplitFlapInstructions.pdf).

Options to potentially get boards created for you:

- [PCB Way](https://www.pcbway.com/)
- [JLC PCB](https://jlcpcb.com/)

> Note: Services are offered by these companies to assembly the boards for you. There are surface mounted components to these devices that you might not be able to do yourself like small resistors for instance, which must be flow soldered. It could be worth having the company do this aspect for you.

### Unit

Each split-flap unit consists of an Arduino Nano mounted on a custom PCB. It controls a 28BYJ-48 stepper motor via a ULN2003 driver chip. The drum with the flaps is homed with a KY003 hall sensor and a magnet mounted to the drum.

Flash each unit's Nano from the `Unit/` folder:

```bash
pio run -t upload
```

Before that, set the per-unit calibration offset by flashing `EEPROM_Write_Offset/` (see [Set Zero Position Offset](#set-zero-position-offset) below).

Inside `Unit.ino`, there is a setting for testing the units so that a few letters are cycled through to ensure what is shown is what you expect. At the top of the file once you have opened the project, you will find a line that is commented out:

```c++
#define SERIAL_ENABLE   // uncomment for serial debug communication
#define TEST_ENABLE    	// uncomment for test mode where the unit will cycle a series of test letters.
```

> Note: If upload fails, your Nano may have the old bootloader. Use `pio run -e unit_old_bootloader -t upload` instead.

Remove the comment characters to help with your testing for the next step of Setting the Zero Position Offset.

#### Set Zero Position Offset

The zero position (or blank flaps position in this case) is attained by driving the stepper to the hall sensor and step a few steps forward. This offset is individual to every unit and needs to be saved to the arduino nano's EEPROM.

A simple sketch has been written to set the offset. Upload the `EEPROM_Write_Offset.ino` sketch and open the serial monitor with 115200 baudrate. It will tell you the current offset and you can enter a new offset. It should be around 100 but yours may vary. You may need to upload the `Unit.ino` sketch with the `TEST_ENABLE` flag uncommented and see if the offset is correct. Repeat until the blank flap is showing every time the unit homes.

#### Set Unit Address

Every unit's address is set by a DIP switch. Set them ascending from zero in binary — the firmware offsets the DIP value by 1 before calling `Wire.begin()` so the first unit lands on I2C address `0x01` (I2C address `0x00` is the reserved general-call address and cannot be used).

| Unit | DIP  | I2C address |
| ---- | ---- | ----------- |
| 1    | 0000 | 0x01        |
| 2    | 0001 | 0x02        |
| 3    | 0010 | 0x03        |
| 4    | 0011 | 0x04        |
| 5    | 0100 | 0x05        |
| 6    | 0101 | 0x06        |
| 7    | 0110 | 0x07        |
| 8    | 0111 | 0x08        |
| 9    | 1000 | 0x09        |
| 10   | 1001 | 0x0A        |

`1` means the switch is in the up position. The master scans the bus at boot and logs which addresses responded (viewable via `pio device monitor` when `SERIAL_ENABLE` is true on the master, or via the `/settings` endpoint).

### ESP01/ESP8266

#### Pre-requisites

Library versions are pinned in [`ESPMaster/platformio.ini`](./ESPMaster/platformio.ini) and installed automatically when you run `pio run`. You no longer need to install them manually.

To flash the ESP8266 you can either use an [Arduino Uno](https://create.arduino.cc/projecthub/pratikdesai/how-to-program-esp8266-esp-01-module-with-arduino-uno-598166) as a programmer or buy a dedicated programmer. A dedicated programmer is much faster.

> Note: Be wary of ESP8266 programmers that are available which allow USB connection to your PC which may not have programming abilities. Typically extra switches are available so that the ESP8266 can be put in programming mode, although you can modify the programmer through a simple solder job to allow it to enter programming mode. Examples can be found in the customer reviews of [Amazon](https://www.amazon.co.uk/gp/product/B078J7LDLY/ref=ppx_yo_dt_b_search_asin_title?ie=UTF8&th=1).

> Alternatively, you can get a dedicated programmer from Amazon such as [this one](https://www.amazon.co.uk/dp/B083QHJW21). This is also available on [AliExpress](https://www.aliexpress.com/item/1005001793822720.html?spm=a2g0o.detail.0.0.48622aefV0Zv89&mp=1) if you are willing to wait a while for it.

#### Web assets live in PROGMEM — no LittleFS upload

The files in [`ESPMaster/data/`](./ESPMaster/data/) (HTML/JS/CSS, favicon, bundled `unit-firmware.hex`) are gzipped and compiled directly into the master's binary by the `ESPMaster/build_assets.py` pre-build script. That means:

- **No separate `pio run -t uploadfs` step.** Just `pio run -t upload` and the whole thing lands in flash.
- The bundled `unit-firmware.hex` is what the master auto-installs to any Nano it finds in twiboot on boot (see [Unit firmware updates](#unit-firmware-updates-via-master)).

#### Updating Settings of the Sketch

There are several options in the Sketch you can modify to customise or change the behaviour of the display. These are marked in the code as "Configurable".

By default, the system runs in **captive-portal mode** (`WIFI_USE_DIRECT false`). On first boot the master exposes a `Split-Flap-AP` access point; connect, pick your real network, done.

![Screenshot WiFi Portal](./Images/Access-Point-Screenshot.jpg)

Alternatively, for a direct-connect firmware that skips the portal entirely, set:

```c++
//Option to either direct connect to a WiFi Network or setup a AP to configure WiFi. Default: false (puts device in AP mode)
#define WIFI_USE_DIRECT true
```

…and provide credentials in a **gitignored** local header so they never land in a public commit:

```bash
cp ESPMaster/WifiCredentials.h.example ESPMaster/WifiCredentials.h
# edit WifiCredentials.h with your real SSID / password
```

The template sets the expected `wifiDirectSsid` / `wifiDirectPassword` globals; the real file is in `.gitignore`. If `WIFI_USE_DIRECT` is `true` but `WifiCredentials.h` is missing, the build still compiles (with a warning) — it just won't join a network.

For the clock mode, set `timezonePosix` in `ESPMaster.ino` to a POSIX TZ string. Common examples:

| Region | POSIX TZ |
| --- | --- |
| UK (Europe/London) | `GMT0BST,M3.5.0/1,M10.5.0` |
| Central Europe | `CET-1CEST,M3.5.0,M10.5.0/3` |
| US Eastern | `EST5EDT,M3.2.0,M11.1.0` |

Full list: https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv

`clockFormat` uses [`strftime(3)`](https://en.cppreference.com/w/c/chrono/strftime) conversion specifiers (`%H:%M`, `%I:%M%p`, etc).

There are several helper `define` variables to help during debugging/running:

- **SERIAL_ENABLE**
  - Use this to enable Serial output lines for tracking executing code. Must stay `false` on the ESP-01 for I2C to work (serial + I2C share the same pins).
- **UNIT_CALLS_DISABLE**
  - Use this to disable the communication with the Arduino Nano Units. This will mean you can check code over function for the ESP module.

#### Experiments

In the main Sketch under "Configurable Defines", an "EXPERIMENTAL" section has been included. This section has been created for features that are things that can be changed and trialled however aren't going to be necessary to be changed for general use.

##### Static IP Address

A feature request of being able to set a Static IP Address was created by [beroliv](https://github.com/beroliv) (thank you for the suggestion). This was to get around issues whereby in some routers, there was issues in being able to do this.

Code has been added to be able to set a Static IP Address on device. To do this:

1. Set the `WIFI_STATIC_IP` variable to `true` (defaulted to `false`)
2. Update the following settings as necessary for your needs:

   ```c++
   IPAddress wifiDeviceStaticIp(192, 168, 1, 100);

   IPAddress wifiRouterGateway(192, 168, 1, 1);
   IPAddress wifiSubnet(255, 255, 0, 0);

   IPAddress wifiPrimaryDns(8, 8, 8, 8);
   ```

**Suggestion:** Set your device up with a Static IP via your router if possible and to avoid conflicts on your network, however feel free to run this code if you are not able to. Testing this functionality showed it does work in both AP/Direct WiFi modes.

#### Sketch Upload

Flash the sketch from the `ESPMaster/` folder:

```bash
pio run -t upload
```

The ESP8266 will reboot running the new sketch. Stick it onto the first unit's PCB and navigate to the IP-address the ESP8266 is getting assigned from your router. The build also drops a self-describing copy of the binary next to `firmware.bin`:

```
ESPMaster/.pio/build/espmaster/firmware-<short-git-rev>.bin
```

…which is the artifact you'd typically archive or push via the web OTA flow below.

### Web OTA

Once the master is on WiFi, subsequent flashes don't need a USB cable:

- Go to the master's web UI → **Master Firmware (OTA)** card → upload the new `firmware-<rev>.bin`. The ESP reboots into the new sketch.
- The same OTA flow also queues every detected unit for its twiboot bootloader right before the reboot, so after the master comes back up it auto-pushes the bundled unit firmware to each one. In one step the whole display is updated.
- If you need to force just the units to re-flash (e.g. you changed `Unit.ino` and regenerated `ESPMaster/data/unit-firmware.hex` but the master is unchanged), click **Flash all unit(s)** under Actions.

### Unit firmware updates via master

The master ships with a copy of the compiled Unit sketch in PROGMEM (see `ESPMaster/data/unit-firmware.hex`, regenerated by the package script from the current `Unit/` build). Any Nano the master probes and finds sitting in twiboot — typically a freshly ICSP-flashed one with no sketch — gets that bundled image automatically on boot. Combined with the web OTA flow above this means:

1. Rebuild `Unit/` after code changes.
2. Copy the resulting `Unit/.pio/build/unit/firmware.hex` over `ESPMaster/data/unit-firmware.hex` (the `flashing/package-flasher.sh` script does this as part of a release).
3. Rebuild master + OTA it.
4. Master reboots, probes, auto-flashes every unit.

### Common Problems

- If the ESP is not talking to the units correctly, check `UNITS_AMOUNT` in `ESPMaster/ESPMaster.ino`. It must match the number of physical units you have connected. The web UI's "Units: N / 10" field shows how many of the expected `UNITS_AMOUNT` the boot-time probe actually found.
- `SERIAL_ENABLE` must stay `false` for the ESP-01 to communicate with the Nanos over I2C (serial and I2C share the same pins on the ESP-01).
- `pio run -t upload` is the only flash step now — no separate filesystem upload. Web assets and the bundled unit firmware are baked into the master binary at build time.
- When the system is powered, your hall sensor should only light up when a magnet is nearby.
- User [@beroliv](https://github.com/beroliv) has reported having issues with WiFi connections. One solution they have proposed is soldering a wire to the antenna to be able to extend its range by creating an antenna. Here is the [link](https://www.stall.biz/project/verbesserte-wlan-konnektivitaet-mit-externen-antennen-fuer-wiffi-weatherman-und-andere-module-mit-esp8266/) (in German but Google Translate does a good job for other languages) they provided to detail the solution. Please take care when carrying out this solution. Thank you for the information [@beroliv](https://github.com/beroliv)!
