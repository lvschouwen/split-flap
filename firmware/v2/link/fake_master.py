#!/usr/bin/env python3
"""A stand-in master for the bench: a row board dials it over the wall link.

    python fake_master.py --id split-flap-bench [--clock] [--commands FILE]

Needs the stock protobuf module generated from wall_link.proto next to this
file (wall_link_pb2.py):
    python -m grpc_tools.protoc --proto_path=. --python_out=. wall_link.proto

Every message a row sends is printed as one JSON line with a timestamp.
--clock shows HH:MM on the row at each minute change, sent 2 s ahead.
--commands names a file that is read as it grows, one command per line:
    show TEXT | quiet on|off | ping | restart | release
"""
from __future__ import annotations

import argparse
import asyncio
import json
import time

from google.protobuf.json_format import MessageToDict

import wall_link_io as io
import wall_link_pb2 as pb

PING_AFTER_IDLE_S = 10
CLOCK_LEAD_S = 2


def log(kind: str, **fields) -> None:
    print(json.dumps({"t": round(time.time(), 3), "kind": kind, **fields}), flush=True)


class Row:
    def __init__(self, reader, writer, args) -> None:
        self.reader, self.writer, self.args = reader, writer, args
        self.peer = writer.get_extra_info("peername")[0]
        self.render_id = 0
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
        elif word:
            log("bad-command", line=line)
            return
        if word and word != "show":
            log("sent", peer=self.peer, command=line.strip())

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
                    log("got", peer=self.peer, type=kind,
                        **MessageToDict(message, preserving_proto_field_name=True))
                    if kind == "hello" and not welcomed:
                        self.send(welcome=pb.Welcome(protocol=io.PROTOCOL, master_id=self.args.id))
                        welcomed = True
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
    args = parser.parse_args()

    async def on_row(reader, writer):
        await Row(reader, writer, args).run()

    server = await asyncio.start_server(on_row, "0.0.0.0", args.port)
    log("listening", port=args.port, id=args.id)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
