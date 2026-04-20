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

**Only one ICSP flash per Nano — twiboot only.** The Unit sketch is bundled inside the master's LittleFS and gets pushed to each Nano over I2C the first time the master sees a Nano sitting in bootloader mode with no sketch installed.

For **each Nano**:

1. Wire the Arduino-as-ISP programmer to the Nano's ICSP header.
2. Set the Nano's DIP switches to its address (see table below).
3. Run **`1-flash-unit-bootloader.bat`** — installs the patched twiboot (reads DIPs, stays alive on empty flash) and sets fuses. Script prompts for the COM port.
4. Disconnect the programmer. Move on to the next Nano.

After **all** Nanos are done:

5. Wire the USB-to-UART adapter to the ESP-01 (GPIO0 → GND for programming, then to VCC / floating for run mode after the flash).
6. Run **`2-flash-master.bat`** — writes master firmware + LittleFS (which contains the bundled `unit-firmware.hex`). Script prompts for the COM port.
7. Remove GPIO0-to-GND, power-cycle the ESP into normal mode.
8. Connect to the `Split-Flap-AP` WiFi network (no password). A captive portal should open; if not, browse to `http://192.168.4.1/`. Enter your home WiFi credentials.
9. The master reboots onto your WiFi. Look up its IP on your router and browse to it for the dashboard.

**On that first boot** with all Nanos powered up, the master:

- Scans the I2C bus, logs which units it finds and whether each one is in bootloader or sketch mode.
- For every unit in bootloader mode, it streams the bundled firmware over I2C automatically.
- You can watch the whole thing scroll by in the **Log** section at the bottom of the dashboard.

End state: every Nano is running the Unit sketch, responding at its DIP-derived I2C address, and the display is live. Future unit firmware updates ride along with master firmware updates — the master's bundled `unit-firmware.hex` gets pushed to any Nano it later finds in bootloader mode (use the **Master Firmware (OTA)** card to flash a new master build, and if you also need to refresh the units, force them into bootloader first).

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
- **Master Firmware (OTA)** section: upload a new `firmware.bin` to update the ESP itself. The master also bundles the unit sketch in PROGMEM and auto-flashes any Nano it sees in bootloader mode, so updating the master is the normal way to push a new unit build.

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
├── config.bat                         (override tool paths if needed)
├── 1-flash-unit-bootloader.bat        (per-Nano, one-time — twiboot only)
├── 2-flash-master.bat                 (one-time, after all Nanos done)
└── prebuilt/
    ├── twiboot-atmega328p-16mhz.hex   (DIP-aware I2C bootloader)
    ├── unit-firmware.hex              (reference copy — master's LittleFS has its own)
    ├── master-firmware.bin            (ESPMaster.ino compiled)
    └── master-littlefs.bin            (web UI + bundled unit firmware)
```

The `unit-firmware.hex` in `prebuilt/` is the same file that's bundled inside `master-littlefs.bin`. It's kept as a standalone copy for reference / manual debugging only — you don't flash it via ICSP anymore.
