"""Serial port enumeration + plug-in watching (pyserial)."""
import time

from serial.tools import list_ports

DRIVER_HINT = """\
No serial ports found. On a fresh Windows machine this almost always means
a missing USB-serial driver:
  - Clone Nanos / most adapters: CH340 driver
    -> https://www.wch-ic.com/downloads/CH341SER_ZIP.html
  - NodeMCU-style adapters: CP210x driver
    -> https://www.silabs.com/interface/usb-bridges (CP210x VCP)
Install, replug the USB cable, then retry. The device must show up in
Device Manager under "Ports (COM & LPT)".
"""


def list_port_names() -> list:
    return [p.device for p in list_ports.comports()]


def describe_ports() -> list:
    return [f"{p.device} — {p.description}" for p in list_ports.comports()]


def diff_new(before: set, now: set) -> set:
    return now - before


def wait_for_new_port(before: set, timeout: float = 60.0, poll: float = 1.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        fresh = diff_new(before, set(list_port_names()))
        if fresh:
            return sorted(fresh)[0]
        time.sleep(poll)
    return None
