"""Length-delimited wall link messages for the host-side stand-ins and tests.

Stock Python protobuf has no helper for "varint length, then the message" on a
stream, so the few lines live here. The boards do the same with nanopb's
PB_ENCODE_DELIMITED and WallLinkStream.h.
"""
from __future__ import annotations

PORT = 7411
PROTOCOL = 1
MAX_BODY = 600  # above the largest envelope either board sends


def frame(message) -> bytes:
    body = message.SerializeToString()
    prefix = bytearray()
    n = len(body)
    while True:
        prefix.append((n & 0x7F) | (0x80 if n > 0x7F else 0))
        n >>= 7
        if not n:
            return bytes(prefix) + body


class Splitter:
    """Feed bytes as they arrive; get whole message bodies back."""

    def __init__(self) -> None:
        self.buf = bytearray()

    def feed(self, data: bytes) -> list[bytes]:
        self.buf += data
        bodies = []
        while True:
            length, shift, pos = 0, 0, 0
            while pos < len(self.buf):
                byte = self.buf[pos]
                length |= (byte & 0x7F) << shift
                pos += 1
                shift += 7
                if not byte & 0x80:
                    break
            else:
                return bodies  # the length itself is not complete yet
            if length > MAX_BODY:
                raise ValueError(f"message of {length} bytes")
            if len(self.buf) < pos + length:
                return bodies
            bodies.append(bytes(self.buf[pos:pos + length]))
            del self.buf[:pos + length]
