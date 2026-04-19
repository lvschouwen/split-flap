# Split-Flap — one-time initial flash (Windows)

This folder holds everything you need to flash a fresh split-flap build: every Nano gets **twiboot** (an I2C bootloader) plus the **Unit sketch**, and the ESP-01 master gets its **firmware + web UI**.

After this is done once, future unit firmware updates happen entirely **over WiFi from the master's web UI** — no more ICSP cables, no more pulling Nanos out.

## Prerequisites

You need:

- **Windows machine** with Python installed (PlatformIO brings its own, so if you installed PIO you're covered).
- **PlatformIO Core** installed (`pip install platformio` — or if you installed it as part of earlier steps in this repo, that's fine). This folder uses PlatformIO's bundled `avrdude` and `esptool.py`.
- **Arduino-as-ISP programmer**: a spare Uno/Nano running the `ArduinoISP` example sketch (see `UnitBootloader/README.md` for setup — need the 10 µF cap!), with its 6 ICSP wires going to the target Nano.
- **USB-to-UART adapter** wired to the ESP-01 for the master flash. GPIO0 must be pullable to GND for programming mode.
- A copy of this `flashing/` folder copied to the Windows machine.

## Order of operations

For **each Nano**:

1. Wire the Arduino-as-ISP programmer to the Nano's ICSP header.
2. Set the Nano's DIP switches to its address (see table below).
3. Edit `config.bat` on your first run — set `ISP_PORT=COMx` to your programmer's port.
4. Run **`1-flash-unit-bootloader.bat`** — installs twiboot + sets fuses.
5. Run **`2-flash-unit-firmware.bat`** — installs the Unit sketch.
6. Disconnect the programmer. Move on to the next Nano.

After **all** Nanos are done:

7. Wire the USB-to-UART adapter to the ESP-01 (GPIO0 → GND for programming, then to VCC for run mode after the flash).
8. Edit `config.bat` — set `ESP_PORT=COMx` to the USB-to-UART adapter's port.
9. Run **`3-flash-master.bat`** — writes master firmware + LittleFS web UI.
10. Remove GPIO0-to-GND, power-cycle the ESP into normal mode.
11. From your phone/laptop: connect to the WiFi network named **"Split-Flap-AP"** (no password). Your device should auto-open a captive portal; if not, browse to `http://192.168.4.1/`. Enter your home WiFi credentials.
12. The master reboots onto your WiFi. Look up its IP on your router and browse to it for the dashboard.

## DIP switch addresses

Each Nano needs a unique address, assigned by its DIP switches:

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

`1` means the switch is in the up position. The firmware offsets the DIP value by 1 before calling `Wire.begin()` because I2C address `0x00` is reserved (general call).

## Once it's all running

In the web UI:

- **Log section** (bottom of the page): shows live master-side logs including the I2C bus scan on startup. You should see every unit responding there.
- **Unit Firmware (I2C OTA)** section: future unit firmware updates go here. Pick a target, upload a new `Unit.ino.hex`, done.

## Troubleshooting

**`not in sync` or `programmer not responding`**
Missing 10 µF reset-disable cap on your programmer, wrong COM port, or serial wasn't released. See `UnitBootloader/README.md` for details.

**`Invalid device signature: 0xFFFFFF`**
Wiring to the target is wrong (most commonly MOSI/MISO swapped), or the target isn't powered. Disconnect the stepper motor from the Unit PCB — it can interfere with SPI during flash.

**ESP flash fails with "Failed to connect"**
ESP-01 isn't in programming mode. GPIO0 has to be held LOW at power-up. Some USB-UART adapters can't supply enough current — try powering the ESP externally and connecting only TX/RX/GND to the UART.

**Captive portal doesn't appear**
Some phones suppress it. Just browse manually to `http://192.168.4.1/` after joining the `Split-Flap-AP` network.

**Master doesn't see units after boot**
Open the Log section. If it says `I2C scan complete. Detected 0/N expected units.`, check that:
- Nanos are powered (common 5V rail with the ESP)
- SDA/SCL lines are wired (GPIO1/GPIO3 on the ESP-01 → SDA/SCL on each Nano)
- DIP switches are set correctly, no two Nanos share an address

## Files here

```
flashing/
├── README.md                          (this file)
├── config.bat                         (you edit — COM ports, tool paths)
├── 1-flash-unit-bootloader.bat        (per-Nano, one-time)
├── 2-flash-unit-firmware.bat          (per-Nano, one-time)
├── 3-flash-master.bat                 (one-time, after all Nanos done)
└── prebuilt/
    ├── twiboot-atmega328p-16mhz.hex   (I2C bootloader for the Nanos)
    ├── unit-firmware.hex              (Unit.ino compiled)
    ├── master-firmware.bin            (ESPMaster.ino compiled)
    └── master-littlefs.bin            (web UI filesystem image)
```

All four `.hex` / `.bin` files are built from the current `master` branch of the repo.
