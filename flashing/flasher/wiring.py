"""Pin-by-pin wiring diagrams shown inline at every hardware step.

The exe is the single source of connection truth at the bench — no README,
no browser. Facts sourced from flashing/README.md + UnitBootloader/README.md.
"""


def dip_pattern(unit_no: int) -> str:
    if not 1 <= unit_no <= 16:
        raise ValueError(f"unit number out of range 1..16: {unit_no}")
    return format(unit_no - 1, "04b")


def dip_visual(unit_no: int) -> str:
    bits = dip_pattern(unit_no)
    switches = "  ".join(
        f"SW{i + 1}:{'up' if b == '1' else 'down'}" for i, b in enumerate(bits)
    )
    return f"DIP {bits}  ({switches})   [1 = up]"


DIAGRAMS = {
    "programmer": """\
BUILD THE PROGRAMMER (one-time)
A spare Uno or Nano becomes an Arduino-as-ISP programmer.

  1. Plug the spare board into USB. This tool flashes ArduinoISP.hex
     onto it through its normal serial bootloader — nothing to wire yet.
  2. AFTER that flash succeeds, fit a 10uF capacitor between the
     PROGRAMMER's RESET pin and GND (stripe/minus leg to GND).
     This stops the programmer auto-resetting when avrdude opens the port.
  3. Leave it on USB — it now programs the target Nanos via 6 wires.
""",
    "icsp": """\
PROGRAMMER -> TARGET NANO (6 wires, repeat per unit)

  programmer D10  ->  target RST
  programmer D11  ->  target D11 (MOSI)
  programmer D12  ->  target D12 (MISO)
  programmer D13  ->  target D13 (SCK)
  programmer 5V   ->  target 5V
  programmer GND  ->  target GND

  ! DISCONNECT THE STEPPER from the unit PCB while flashing —
    it loads the SPI pins and causes 0xFFFFFF signature errors.
  ! MOSI/MISO swapped is the classic wiring mistake.
""",
    "esp_uart": """\
USB-UART ADAPTER -> ESP-01 (master flash)

  adapter 3.3V  ->  ESP VCC        (NEVER 5V — kills the ESP)
  adapter 3.3V  ->  ESP CH_PD/EN
  adapter GND   ->  ESP GND
  adapter TX    ->  ESP RX         (crossed!)
  adapter RX    ->  ESP TX
  ESP GPIO0     ->  GND            (programming mode, only during flash)

  Flash sequence: jumper GPIO0 to GND -> plug in adapter -> flash ->
  unplug -> REMOVE the GPIO0 jumper -> plug back in (normal boot).
  Cheap adapters can't power the ESP — if flashing is flaky, feed
  VCC from a separate 3.3V supply and share GND.
""",
    "assembly": """\
DISPLAY ASSEMBLY (before the master's first normal boot)

  - One shared 5V rail powers all Nanos and the ESP-01 (via its 3.3V reg).
  - I2C bus: ESP-01 GPIO1 -> SDA of every Nano, GPIO3 -> SCL of every Nano.
  - Every Nano: DIP set (contiguous from unit 1 = 0000), twiboot installed.
  - Power EVERYTHING before booting the master: on its first boot scan the
    master auto-installs the unit firmware over I2C to every Nano it sees
    in bootloader mode. Units missing from the bus miss that window (they
    get picked up on a later reboot, but verify reads cleaner first time).
""",
}
