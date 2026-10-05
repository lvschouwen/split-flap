"""Python twin of WallLink.h for the host-side stand-ins (fake row, fake master).

The frame layout and field order are defined by WallLink.h; this file follows
it and tests/test_wall_link_vectors.py holds the two together through the byte
vectors in the Master tree's native test.
"""
from __future__ import annotations

import struct

PROTOCOL = 1
PORT = 7411
HEADER_LEN = 3
MAX_PAYLOAD = 512

# Field kinds: B/H/I/b = struct codes (big-endian); sN = string, u8 length,
# at most N; aN = bytes, u8 length, at most N; dN = bytes, u16 length, at most
# N; fN = exactly N bytes; r = the rest of the payload.
MESSAGES: dict[str, tuple[int, list[tuple[str, str]]]] = {
    "welcome": (1, [("protocol", "B"), ("masterId", "s32")]),
    "time": (2, [("echoRowMs", "I"), ("masterMs", "I"), ("epochS", "I")]),
    "show": (3, [("renderId", "I"), ("atMs", "I"), ("speed", "B"), ("text", "s16")]),
    "quiet": (4, [("on", "B")]),
    "config": (5, [("fallback", "B"), ("updateUnitsAtStart", "B"), ("tz", "s64")]),
    "op": (6, [("opId", "I"), ("opcode", "B"), ("address", "B"), ("args", "a16")]),
    "update": (7, [("rev", "s16"), ("size", "I"), ("md5", "f16"), ("packed", "B")]),
    "logctl": (8, [("on", "B")]),
    "ping": (9, []),
    "restart": (10, []),
    "release": (11, []),
    "hello": (32, [("protocol", "B"), ("id", "s32"), ("rev", "s16"), ("bootId", "I"),
                   ("flags", "B"), ("width", "B")]),
    "timereq": (33, [("rowMs", "I"), ("rxCount", "H")]),
    "status": (34, [("rxCount", "H"), ("upS", "I"), ("heap", "I"), ("minHeap", "I"),
                    ("maxBlock", "I"), ("rssi", "b"), ("txPower", "B"), ("busTx", "I"),
                    ("busErr", "I"), ("busDead", "B"), ("busEpisodes", "H"),
                    ("escalations", "B"), ("jobRunning", "B"), ("imageSize", "I")]),
    "shown": (35, [("renderId", "I"), ("lateMs", "H"), ("rxCount", "H")]),
    "opstate": (36, [("opId", "I"), ("phase", "B"), ("reason", "B"), ("rxCount", "H"),
                     ("dataOffset", "H"), ("data", "d480")]),
    "event": (37, [("code", "H"), ("unit", "B"), ("a", "I"), ("b", "I"), ("upS", "I")]),
    "logline": (38, [("text", "r")]),
    "pong": (39, [("rxCount", "H")]),
}
TYPE_NAMES = {number: name for name, (number, _) in MESSAGES.items()}


class LinkError(ValueError):
    pass


def encode(name: str, **fields) -> bytes:
    number, schema = MESSAGES[name]
    out = bytearray()
    for field, kind in schema:
        value = fields[field]
        if kind in "BHIb":
            out += struct.pack(">" + kind, value)
        elif kind[0] in "sa":
            raw = value.encode() if kind[0] == "s" else bytes(value)
            if len(raw) > int(kind[1:]):
                raise LinkError(f"{name}.{field} is longer than {kind[1:]}")
            out += bytes([len(raw)]) + raw
        elif kind[0] == "d":
            if len(value) > int(kind[1:]):
                raise LinkError(f"{name}.{field} is longer than {kind[1:]}")
            out += struct.pack(">H", len(value)) + bytes(value)
        elif kind[0] == "f":
            if len(value) != int(kind[1:]):
                raise LinkError(f"{name}.{field} must be {kind[1:]} bytes")
            out += bytes(value)
        else:
            out += value.encode() if isinstance(value, str) else bytes(value)
    if len(out) > MAX_PAYLOAD:
        raise LinkError(f"{name} payload is {len(out)} bytes")
    return struct.pack(">HB", len(out), number) + bytes(out)


def decode(type_number: int, payload: bytes) -> tuple[str, dict]:
    """Known fields only; bytes after them are ignored (a message grows at its end)."""
    name = TYPE_NAMES[type_number]
    pos, fields = 0, {}

    def take(n: int) -> bytes:
        nonlocal pos
        if pos + n > len(payload):
            raise LinkError(f"{name} is cut short")
        pos += n
        return payload[pos - n:pos]

    for field, kind in MESSAGES[name][1]:
        if kind in "BHIb":
            fields[field] = struct.unpack(">" + kind, take(struct.calcsize(kind)))[0]
        elif kind[0] in "sa":
            n = take(1)[0]
            if n > int(kind[1:]):
                raise LinkError(f"{name}.{field} is longer than {kind[1:]}")
            raw = take(n)
            fields[field] = raw.decode() if kind[0] == "s" else raw
        elif kind[0] == "d":
            n = struct.unpack(">H", take(2))[0]
            if n > int(kind[1:]):
                raise LinkError(f"{name}.{field} is longer than {kind[1:]}")
            fields[field] = take(n)
        elif kind[0] == "f":
            fields[field] = take(int(kind[1:]))
        else:
            fields[field] = take(len(payload) - pos)
    return name, fields


class Decoder:
    """Reassembles frames from a TCP byte stream."""

    def __init__(self) -> None:
        self.buf = bytearray()

    def feed(self, data: bytes) -> list[tuple[int, bytes]]:
        self.buf += data
        frames = []
        while len(self.buf) >= HEADER_LEN:
            length, number = struct.unpack(">HB", self.buf[:HEADER_LEN])
            if length > MAX_PAYLOAD:
                raise LinkError(f"frame of {length} bytes")
            if len(self.buf) < HEADER_LEN + length:
                break
            frames.append((number, bytes(self.buf[HEADER_LEN:HEADER_LEN + length])))
            del self.buf[:HEADER_LEN + length]
        return frames


# One example per message; the native test builds the same messages in C++
# and both sides must produce the bytes written there.
VECTORS: dict[str, tuple[str, dict]] = {
    "welcome": ("welcome", dict(protocol=1, masterId="split-flap-c8a746")),
    "time": ("time", dict(echoRowMs=39153, masterMs=7200123, epochS=1791223510)),
    "show": ("show", dict(renderId=94, atMs=7202000, speed=80, text="19:34")),
    "show_blank": ("show", dict(renderId=95, atMs=0, speed=1, text="")),
    "quiet": ("quiet", dict(on=1)),
    "config": ("config", dict(fallback=1, updateUnitsAtStart=0, tz="CET-1CEST,M3.5.0,M10.5.0/3")),
    "op": ("op", dict(opId=0xA1B20007, opcode=5, address=6, args=bytes([0x01, 0xF4]))),
    "update": ("update", dict(rev="3f1a516", size=323047, md5=bytes(range(16)), packed=1)),
    "logctl": ("logctl", dict(on=1)),
    "ping": ("ping", {}),
    "restart": ("restart", {}),
    "release": ("release", {}),
    "hello": ("hello", dict(protocol=1, id="split-flap-261bb6", rev="3f1a516", bootId=0xDEADBEEF,
                            flags=1, width=5)),
    "timereq": ("timereq", dict(rowMs=39153, rxCount=17)),
    "status": ("status", dict(rxCount=17, upS=5429, heap=27312, minHeap=17464, maxBlock=26232,
                              rssi=-58, txPower=20, busTx=13502, busErr=0, busDead=0,
                              busEpisodes=2, escalations=0, jobRunning=1, imageSize=461600)),
    "shown": ("shown", dict(renderId=94, lateMs=0, rxCount=18)),
    "opstate": ("opstate", dict(opId=0xA1B20007, phase=1, reason=0, rxCount=19, dataOffset=128,
                                data=bytes([0x0C, 0xA2, 0x0C]))),
    "event": ("event", dict(code=3, unit=6, a=10, b=0, upS=8343)),
    "logline": ("logline", dict(text="[5429] bus recovered")),
    "pong": ("pong", dict(rxCount=65535)),
}


def vector_bytes(name: str) -> bytes:
    message, fields = VECTORS[name]
    return encode(message, **fields)
