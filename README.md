# Split-Flap

[![Build ESP Master Sketch](https://github.com/JonnyBooker/split-flap/actions/workflows/build-esp-master.yml/badge.svg)](https://github.com/JonnyBooker/split-flap/actions/workflows/build-esp-master.yml) [![Build EEPROM Write Sketch](https://github.com/JonnyBooker/split-flap/actions/workflows/build-eeprom-write.yml/badge.svg)](https://github.com/JonnyBooker/split-flap/actions/workflows/build-eeprom-write.yml) [![Build Arduino Unit Sketch](https://github.com/JonnyBooker/split-flap/actions/workflows/build-unit.yml/badge.svg)](https://github.com/JonnyBooker/split-flap/actions/workflows/build-unit.yml)

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
- Countdown Mode
  - Countdown in days to a specified date
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
- Message Scheduling
  - Ability to send a message and display it at a later date.
  - Options for when a scheduled message being shown
    - If the clock was in another mode, such as `Clock` mode, it will show the message for a duration (changable via updating `scheduledMessageDisplayTimeMillis` in `ESPMaster.ino`), then return to that mode afterwards
    - A checkbox on the UI is presented ("Show Indefinitely") that when checked, will show a message and leave it on the display
- Arduino OTA
  - Over the Air updates to the display
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
pio run -t upload         # flash firmware
pio run -t uploadfs       # flash the LittleFS contents (ESPMaster only)
pio device monitor        # serial monitor at 115200 baud
```

For the Unit and EEPROM_Write_Offset sketches, if upload fails because of the old bootloader on your Nano, use the `*_old_bootloader` env:

```bash
pio run -e unit_old_bootloader -t upload
```

Host-side unit tests for the ESPMaster string helpers live in `ESPMaster/test/` and run with:

```bash
cd ESPMaster && pio test -e native
```

If you'd rather stay with the Arduino IDE, the sketches are still compatible — each `*.ino` file sits next to its `platformio.ini` and opens as a normal sketch. The pinned library versions are documented in each `platformio.ini`.

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

#### Uploading the Static Assets via LittleFS

The static files in [`ESPMaster/data/`](./ESPMaster/data/) make up the web interface. From the `ESPMaster/` folder:

```bash
pio run -t uploadfs
```

This uploads the website onto the ESP8266's file system. Sketch upload is a separate step (below).

#### Updating Settings of the Sketch

There are several options in the Sketch you can modify to customise or change the behaviour of the display. These are marked in the code as "Configurable".

By default, the system will run in an "Access Point" mode where you will be able to connect to the display and put in WiFi credentials directly. This means if you WiFi changes, you don't have to re-upload a new sketch. Screenshot of the WiFi setup portal:

![Screenshot WiFi Portal](./Images/Access-Point-Screenshot.jpg)

Alternatively, you can specify credentials directly. You can go ahead and change the credentials in these variables:

```c++
const char* wifiDirectSsid = "";
const char* wifiDirectPassword = "";
```

You will also need to change the WiFi Mode in the code via changing this variable to `true`:

```c++
//Option to either direct connect to a WiFi Network or setup a AP to configure WiFi. Default: false (puts device in AP mode)
#define WIFI_USE_DIRECT false
```

You will also want to change the `timezoneString` to your time zone. You can find the TZ database names here: https://en.wikipedia.org/wiki/List_of_tz_database_time_zones

You can also modify the date and clock format easily by using this table: https://github.com/ropg/ezTime#datetime

There are several helper `define` variables to help during debugging/running:

- **SERIAL_ENABLE**
  - Use this to enable Serial output lines for tracking executing code
- **OTA_ENABLE**
  - Use this to enable OTA updates from the Arduino IDE
  - Subsequently, you can set a password for OTA via the `otaPassword` variable
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

Once the LittleFS upload is done, flash the sketch itself from the `ESPMaster/` folder:

```bash
pio run -t upload
```

The ESP8266 will reboot running the new sketch. Stick it onto the first unit's PCB and navigate to the IP-address the ESP8266 is getting assigned from your router.

### Common Problems

- If the ESP is not talking to the units correctly, check `UNITS_AMOUNT` in `ESPMaster/ESPMaster.ino`. It must match the number of physical units you have connected.
- `SERIAL_ENABLE` must stay `false` for the ESP-01 to communicate with the Nanos over I2C (serial and I2C share the same pins on the ESP-01).
- Remember to run both `pio run -t uploadfs` (static files) and `pio run -t upload` (sketch) on first setup.
- When the system is powered, your hall sensor should only light up when a magnet is nearby.
- User [@beroliv](https://github.com/beroliv) has reported having issues with WiFi connections. One solution they have proposed is soldering a wire to the antenna to be able to extend its range by creating an antenna. Here is the [link](https://www.stall.biz/project/verbesserte-wlan-konnektivitaet-mit-externen-antennen-fuer-wiffi-weatherman-und-andere-module-mit-esp8266/) (in German but Google Translate does a good job for other languages) they provided to detail the solution. Please take care when carrying out this solution. Thank you for the information [@beroliv](https://github.com/beroliv)!
