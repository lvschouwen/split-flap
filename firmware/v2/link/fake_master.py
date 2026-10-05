#!/usr/bin/env python3
"""A stand-in master for the bench: a row board dials it over the wall link.

    python fake_master.py --id split-flap-bench [--clock] [--commands FILE]
                          [--image FILE [--image-rev REV] [--http-port N]]

Needs the stock protobuf module generated from wall_link.proto next to this
file (wall_link_pb2.py):
    python -m grpc_tools.protoc --proto_path=. --python_out=. wall_link.proto

Every message a row sends is printed as one JSON line with a timestamp.
--clock shows HH:MM on the row at each minute change, sent 2 s ahead.
--commands names a file that is read as it grows, one command per line:
    show TEXT | quiet on|off | ping | restart | release
    config blank|time|date on|off [TZ]     (on|off: update units at start)
    op NAME ADDRESS [ARG]                  (NAME as in OpCode, without OPC_)
    update [REV]                           (offer --image; REV overrides its rev)
    update-bad size|md5|port               (an offer that must fail safely)

--image is the row image this master stores: it is served at GET /firmware/row
on --http-port and offered with `update`. Its rev is read from a file name
like follower-<rev>-gz.bin unless --image-rev names it. A row that says Hello
in rescue mode is offered it at once.

The unit facts and a boot dump arrive in pieces; they are printed once, whole.
"""
from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import re
import time
import zlib
from pathlib import Path

from google.protobuf.json_format import MessageToDict

import wall_link_io as io
import wall_link_pb2 as pb

PING_AFTER_IDLE_S = 10
CLOCK_LEAD_S = 2


def log(kind: str, **fields) -> None:
    print(json.dumps({"t": round(time.time(), 3), "kind": kind, **fields}), flush=True)


class Image:
    """The row image this master stores."""

    def __init__(self, path: str, rev: str | None, http_port: int) -> None:
        self.data = Path(path).read_bytes()
        named = re.fullmatch(r"follower-(.+?)(-gz)?\.bin", Path(path).name)
        if not rev and not named:
            raise SystemExit("--image-rev is needed: the file name does not carry the rev")
        self.rev = rev or named.group(1)
        self.md5 = hashlib.md5(self.data).digest()
        self.packed = self.data[:1] == b"\x1f"
        self.http_port = http_port

    def offer(self, rev: str | None = None, bad: str | None = None):
        return pb.Update(rev=rev or self.rev,
                         size=len(self.data) + (1 if bad == "size" else 0),
                         md5=bytes(16 * [0xAA]) if bad == "md5" else self.md5,
                         packed=self.packed,
                         http_port=self.http_port + (1 if bad == "port" else 0))

    async def serve(self, reader, writer) -> None:
        peer = writer.get_extra_info("peername")[0]
        try:
            request = (await reader.readuntil(b"\r\n\r\n")).split(b"\r\n")[0].decode()
            ok = request.split()[:2] == ["GET", "/firmware/row"]
            body = self.data if ok else b"not found\n"
            writer.write((f"HTTP/1.0 {'200 OK' if ok else '404 Not Found'}\r\n"
                          f"Content-Type: application/octet-stream\r\n"
                          f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n").encode())
            started = time.monotonic()
            writer.write(body)
            await writer.drain()
            log("http", peer=peer, request=request, bytes=len(body),
                seconds=round(time.monotonic() - started, 1))
        except (ConnectionError, asyncio.IncompleteReadError, asyncio.LimitOverrunError) as error:
            log("http-error", peer=peer, error=repr(error))
        finally:
            writer.close()


class Row:
    def __init__(self, reader, writer, args) -> None:
        self.reader, self.writer, self.args = reader, writer, args
        self.peer = writer.get_extra_info("peername")[0]
        self.render_id = 0
        self.op_id = 0
        self.units = bytearray()
        self.result = bytearray()
        self.busy = False
        self.last_sent = time.monotonic()

    def send(self, **body) -> None:
        self.writer.write(io.frame(pb.ToRow(**body)))
        self.last_sent = time.monotonic()

    def show(self, text: str, commit_at_ms: int = 0) -> None:
        self.render_id += 1
        self.send(show=pb.Show(render_id=self.render_id, commit_at_ms=commit_at_ms,
                               speed=self.args.speed, text=text))
        log("sent", peer=self.peer, show=text, render_id=self.render_id, commit_at_ms=commit_at_ms)

    def command(self, line: str) -> None:
        try:
            self.run_command(line)
        except (ValueError, KeyError, IndexError) as error:
            log("bad-command", line=line.strip(), error=str(error))

    def run_command(self, line: str) -> None:
        word, _, rest = line.strip().partition(" ")
        if word == "show":
            self.show(rest.upper())
        elif word == "quiet":
            self.send(quiet=pb.Quiet(on=rest == "on"))
        elif word == "ping":
            self.send(ping=pb.Ping())
        elif word == "restart":
            self.send(restart=pb.Restart())
        elif word == "release":
            self.send(release=pb.Release())
        elif word == "config":
            fallback, units_at_start, *tz = rest.split(maxsplit=2)
            self.send(config=pb.Config(fallback=pb.Fallback.Value("FALLBACK_" + fallback.upper()),
                                       update_units_at_start=units_at_start == "on",
                                       tz=tz[0] if tz else ""))
        elif word == "op":
            name, address, *arg = rest.split()
            self.op_id += 1
            self.send(op=pb.Op(op_id=self.op_id, opcode=pb.OpCode.Value("OPC_" + name.upper()),
                               address=int(address), arg=int(arg[0]) if arg else 0))
        elif word in ("update", "update-bad"):
            if not self.args.image:
                raise ValueError("no --image to offer")
            bad = rest if word == "update-bad" else None
            self.send(update=self.args.image.offer(rev=None if bad else rest or None, bad=bad))
        elif word:
            log("bad-command", line=line)
            return
        if word and word != "show":
            log("sent", peer=self.peer, command=line.strip())

    def got(self, message) -> None:
        kind = message.WhichOneof("body") or "unknown"
        if kind == "units_json":
            piece = message.units_json
            if piece.offset == 0:
                self.units.clear()
            if piece.offset != len(self.units):
                log("units-out-of-order", peer=self.peer, offset=piece.offset, have=len(self.units))
                return
            self.units += piece.data
            if len(self.units) == piece.total:
                log("got", peer=self.peer, type="units", doc_id=piece.doc_id, bytes=piece.total,
                    units=json.loads(self.units))
            return
        fields = MessageToDict(message, preserving_proto_field_name=True)
        if kind == "op_state":
            state = message.op_state
            if state.data_offset == 0:
                self.result.clear()
            self.result += state.data
            fields = fields["op_state"]
            fields.pop("data", None)
            if state.phase != pb.OP_RUNNING and self.result:
                try:
                    fields["result"] = json.loads(self.result)
                except ValueError:
                    fields["result_bytes"] = len(self.result)
                    fields["result_crc32"] = f"{zlib.crc32(self.result):08x}"
        log("got", peer=self.peer, type=kind, **fields)

    async def run(self) -> None:
        splitter = io.Splitter()
        welcomed = False
        tasks = [asyncio.create_task(self.housekeeping())]
        log("connected", peer=self.peer)
        try:
            while data := await self.reader.read(1024):
                for body in splitter.feed(data):
                    message = pb.ToMaster.FromString(body)
                    kind = message.WhichOneof("body") or "unknown"
                    self.got(message)
                    if kind == "hello" and not welcomed:
                        self.send(welcome=pb.Welcome(protocol=io.PROTOCOL, master_id=self.args.id))
                        welcomed = True
                        if message.hello.rescue and self.args.image:
                            self.send(update=self.args.image.offer())
                            log("sent", peer=self.peer, command="update (row is in rescue mode)")
                    elif kind == "status":
                        self.busy = message.status.busy
        except (ConnectionError, ValueError) as error:
            log("error", peer=self.peer, error=str(error))
        finally:
            for task in tasks:
                task.cancel()
            self.writer.close()
            log("disconnected", peer=self.peer)

    async def housekeeping(self) -> None:
        commands_at = 0
        shown_minute = None
        while True:
            await asyncio.sleep(0.2)
            if self.args.commands:
                try:
                    with open(self.args.commands) as handle:
                        handle.seek(commands_at)
                        for line in handle:
                            self.command(line)
                        commands_at = handle.tell()
                except FileNotFoundError:
                    pass
            if self.args.clock:
                upcoming = (int(time.time() + CLOCK_LEAD_S) // 60) * 60
                if upcoming != shown_minute and time.time() + CLOCK_LEAD_S >= upcoming:
                    shown_minute = upcoming
                    self.show(time.strftime("%H:%M", time.localtime(upcoming)), upcoming * 1000)
            idle = time.monotonic() - self.last_sent
            if idle >= PING_AFTER_IDLE_S and not self.busy:
                self.send(ping=pb.Ping())


async def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--id", required=True, help="the master name the row is paired with")
    parser.add_argument("--port", type=int, default=io.PORT)
    parser.add_argument("--speed", type=int, default=80)
    parser.add_argument("--clock", action="store_true")
    parser.add_argument("--commands")
    parser.add_argument("--image", help="the row image to serve and offer")
    parser.add_argument("--image-rev")
    parser.add_argument("--http-port", type=int, default=7480)
    args = parser.parse_args()
    http = None
    if args.image:
        args.image = Image(args.image, args.image_rev, args.http_port)
        http = await asyncio.start_server(args.image.serve, "0.0.0.0", args.http_port)
        log("image", rev=args.image.rev, bytes=len(args.image.data), packed=args.image.packed,
            md5=args.image.md5.hex(), http_port=args.http_port)

    async def on_row(reader, writer):
        await Row(reader, writer, args).run()

    server = await asyncio.start_server(on_row, "0.0.0.0", args.port)
    log("listening", port=args.port, id=args.id)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
