#!/usr/bin/env python3
"""A stand-in row board for the bench: it dials a master over the wall link.

    python fake_row.py --master HOST --id row-bench [--width 16] [--rev REV]
                       [--rescue] [--units FILE] [--commands FILE]
    python fake_row.py --pair-port 80 --id row-bench ...    (unpaired: waits for a master)

Needs wall_link_pb2.py next to this file (see fake_master.py).

It behaves as the ESP-01 row does on the link: Hello, then Status every 10 s
and when its busy flag changes, the unit facts every 30 s and after a job,
Pong for Ping, Shown for Show once the "render" is over. While a job, a render
or a download runs it reads nothing from the connection, as the real row does.
An offered image is downloaded from the master and checked; the row then comes
back as a restarted board on that rev. Every message from the master is printed
as one JSON line with a timestamp.

--pair-port serves the row's POST /pair (form field `master`): the caller
becomes this row's master and is dialled from then on. A master of another
name is refused with 409 while this row is paired, as on the board. Without
--master the row starts unpaired and dials nobody until it is paired.

--units names a captured /units/health document to send as the unit facts;
without it a plain one for --width units is made up, in the keys of the shared
serializer (buildUnitHealthJson).
--commands names a file that is read as it grows, one command per line:
    busy SECONDS        a unit job the master did not ask for: silent that long
    drop                close the connection and dial again (same boot id)
    restart             come back as a restarted board (new boot id)
    rescue on|off       restart into, or out of, rescue mode
    event CODE [UNIT [A [B [AGE_S]]]]
    units               send the unit facts now
    log TEXT            a log line (sent only while the master asked for the log)
"""
from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import random
import time
import zlib
from pathlib import Path

import wall_link_io as io
import wall_link_pb2 as pb
from google.protobuf.json_format import MessageToDict

STATUS_EVERY_S = 10
UNITS_EVERY_S = 30
WELCOME_TIMEOUT_S = 5
BACKOFF_MIN_S, BACKOFF_MAX_S = 1, 8
PIECE = 480  # UnitsJson.data / OpState.data
BOOT_SECTION = bytes(range(256)) * 4

# How long each job holds the row, in seconds (the real row: section 9 of the spec).
JOB_SECONDS = {pb.OPC_SELF_TEST: 29, pb.OPC_UPDATE_UNITS: 12, pb.OPC_BOOT_DUMP: 6,
               pb.OPC_BOOT_UPDATE: 8, pb.OPC_HOME: 5, pb.OPC_PROBE: 3,
               pb.OPC_SET_ADDRESS: 4, pb.OPC_CLEAR_ADDRESS: 4, pb.OPC_HOME_ALL: 20}
JOB_RESULT = {pb.OPC_SELF_TEST: b'{"ok":true,"steps":2038}',
              pb.OPC_BOOT_INFO: b'{"crc":"e422a668","verdict":"current"}',
              pb.OPC_BOOT_DUMP: BOOT_SECTION}


def log(kind: str, **fields) -> None:
    print(json.dumps({"t": round(time.time(), 3), "kind": kind, **fields}), flush=True)


class Released(Exception):
    """The master let this row go: stop dialling."""


class Row:
    def __init__(self, args) -> None:
        self.args = args
        self.rev = args.rev
        self.rescue = args.rescue
        self.boot_id = random.getrandbits(32)
        self.started = time.monotonic()
        self.busy = False
        self.busy_sent = False
        self.log_on = False
        self.doc_id = 0
        self.pending_update_state = None
        self.writer = None
        self.master = args.master        # the address this row dials; None = unpaired
        self.master_id = args.paired_with
        try:
            self.commands_at = Path(args.commands).stat().st_size if args.commands else 0
        except FileNotFoundError:
            self.commands_at = 0

    # ---- sending ----

    def send(self, **body) -> None:
        self.writer.write(io.frame(pb.ToMaster(**body)))

    def send_status(self) -> None:
        self.send(status=pb.Status(up_s=int(time.monotonic() - self.started), heap=33000,
                                   min_heap=28500, max_block=20000, rssi=-58, tx_power=34,
                                   busy=self.busy, image_size=470000, time_synced=True,
                                   heap2=15600))
        self.busy_sent = self.busy
        self.last_status = time.monotonic()

    def send_units(self) -> None:
        self.last_units = time.monotonic()
        if self.rescue:
            return
        if self.args.units:
            doc = Path(self.args.units).read_bytes().strip()
        else:
            doc = json.dumps({"width": self.args.width, "faulty": 0, "units": [
                {"i": i, "a": i + 1, "st": 1, "v": 0, "ofs": 0, "age": 1000, "hs2": 2}
                for i in range(self.args.width)]}, separators=(",", ":")).encode()
        self.doc_id += 1
        for offset in range(0, len(doc), PIECE):
            self.send(units_json=pb.UnitsJson(doc_id=self.doc_id, offset=offset, total=len(doc),
                                              data=doc[offset:offset + PIECE]))
        self.last_units = time.monotonic()

    def restart(self) -> None:
        self.boot_id = random.getrandbits(32)
        self.started = time.monotonic()
        raise ConnectionResetError("restarting")

    # ---- what holds the row: nothing is read from the connection meanwhile ----

    async def hold(self, seconds: float) -> None:
        self.busy = True
        self.send_status()
        await self.writer.drain()
        await asyncio.sleep(seconds)
        self.busy = False

    async def run_op(self, op) -> None:
        def state(phase, **more):
            self.send(op_state=pb.OpState(op_id=op.op_id, phase=phase, **more))

        if self.rescue:
            return state(pb.OP_REFUSED, reason=pb.REFUSAL_RESCUE)
        if op.opcode not in pb.OpCode.values() or op.opcode == pb.OPC_NONE:
            return state(pb.OP_REFUSED, reason=pb.REFUSAL_UNKNOWN_OP)
        whole_row = op.opcode in (pb.OPC_PROBE, pb.OPC_HOME_ALL) or (
            op.opcode == pb.OPC_UPDATE_UNITS and op.address == 0)
        if not whole_row and not 1 <= op.address <= self.args.width:
            return state(pb.OP_REFUSED, reason=pb.REFUSAL_NO_UNIT)
        if op.opcode == pb.OPC_SET_ADDRESS and op.arg != op.address and (
                1 <= op.arg <= self.args.width):
            return state(pb.OP_REFUSED, reason=pb.REFUSAL_ADDRESS_TAKEN)
        state(pb.OP_RUNNING)
        await self.hold(self.args.job_scale * JOB_SECONDS.get(op.opcode, 1))
        result = JOB_RESULT.get(op.opcode, b"")
        pieces = [result[i:i + PIECE] for i in range(0, len(result), PIECE)] or [b""]
        for n, piece in enumerate(pieces):
            last = n == len(pieces) - 1
            state(pb.OP_OK if last else pb.OP_RUNNING, data_offset=n * PIECE, data=piece)
        self.send_status()
        self.send_units()

    async def run_update(self, offer) -> None:
        def state(phase, reason=pb.UPDATE_REASON_NONE, detail=0):
            self.pending_update_state = None
            self.send(update_state=pb.UpdateState(rev=offer.rev, phase=phase, reason=reason,
                                                  detail=detail))

        if not offer.rev or not offer.size or len(offer.md5) != 16:
            return state(pb.UPDATE_REFUSED, pb.UPDATE_BAD_OFFER)
        if offer.rev == self.rev and not self.rescue:
            return state(pb.UPDATE_REFUSED, pb.UPDATE_CURRENT)
        if self.busy:
            return state(pb.UPDATE_REFUSED, pb.UPDATE_UNITS_BUSY)
        state(pb.UPDATE_DOWNLOADING)
        await self.writer.drain()
        port = offer.http_port or 80
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(self.master, port), 5)
        except (OSError, asyncio.TimeoutError):
            return state(pb.UPDATE_FAILED, pb.UPDATE_UNREACHABLE)
        try:
            writer.write(f"GET /firmware/row HTTP/1.0\r\nHost: {self.master}\r\n\r\n".encode())
            head = (await reader.readuntil(b"\r\n\r\n")).decode(errors="replace")
            status = int(head.split()[1])
            if status != 200:
                return state(pb.UPDATE_FAILED, pb.UPDATE_HTTP_STATUS, status)
            lengths = [int(line.split(":")[1]) for line in head.split("\r\n")
                       if line.lower().startswith("content-length:")]
            if lengths != [offer.size]:
                return state(pb.UPDATE_FAILED, pb.UPDATE_SIZE_DIFFERS, lengths[0] if lengths else 0)
            image = await reader.read(-1)
        finally:
            writer.close()
        if len(image) != offer.size:
            return state(pb.UPDATE_FAILED, pb.UPDATE_STALLED, len(image))
        if (image[:1] == b"\x1f") != offer.packed:
            return state(pb.UPDATE_FAILED, pb.UPDATE_IMAGE, 101)
        if hashlib.md5(image).digest() != offer.md5:
            return state(pb.UPDATE_FAILED, pb.UPDATE_FLASH)
        log("installed", rev=offer.rev, bytes=len(image), crc32=f"{zlib.crc32(image):08x}")
        self.rev, self.rescue = offer.rev, False
        # As on the row: the result is delivered on the connection after the restart
        # if this one closes first.
        self.pending_update_state = pb.UpdateState(rev=offer.rev, phase=pb.UPDATE_INSTALLED)
        self.restart()

    # ---- the master's messages ----

    async def got(self, message) -> None:
        kind = message.WhichOneof("body") or "unknown"
        log("got", type=kind, **MessageToDict(message, preserving_proto_field_name=True).get(kind, {}))
        if kind == "ping":
            self.send(pong=pb.Pong())
        elif kind == "show":
            show = message.show
            now_ms = int(time.time() * 1000)
            if show.commit_at_ms > now_ms:
                await asyncio.sleep((show.commit_at_ms - now_ms) / 1000)
            late = max(0, int(time.time() * 1000) - show.commit_at_ms) if show.commit_at_ms else 0
            await asyncio.sleep(self.args.render_s)
            self.send(shown=pb.Shown(render_id=show.render_id, late_ms=late))
        elif kind == "op":
            await self.run_op(message.op)
        elif kind == "update":
            await self.run_update(message.update)
        elif kind == "log_ctl":
            self.log_on = message.log_ctl.on
            if self.log_on:
                self.send(log_line=pb.LogLine(text=f"[{int(time.monotonic() - self.started)}] "
                                                   f"fake row {self.args.id} log on".encode()))
        elif kind == "restart":
            self.restart()
        elif kind == "release":
            self.master = self.master_id = None
            raise Released()

    async def command(self, line: str) -> None:
        word, _, rest = line.strip().partition(" ")
        log("command", line=line.strip())
        if word == "busy":
            await self.hold(float(rest))
            self.send_status()
        elif word == "drop":
            raise ConnectionResetError("dropped on command")
        elif word == "restart":
            self.restart()
        elif word == "rescue":
            self.rescue = rest == "on"
            self.restart()
        elif word == "event":
            code, unit, a, b, age = ([int(x) for x in rest.split()] + [0, 0, 0, 0])[:5]
            up = int(time.monotonic() - self.started)
            self.send(event=pb.Event(code=code, unit=unit, a=a, b=b,
                                     up_s=max(0, up - age), age_s=age))
        elif word == "units":
            self.send_units()
        elif word == "log" and self.log_on:
            self.send(log_line=pb.LogLine(
                text=f"[{int(time.monotonic() - self.started)}] {rest}".encode()))

    async def read_commands(self) -> None:
        if not self.args.commands:
            return
        try:
            with open(self.args.commands) as handle:
                handle.seek(self.commands_at)
                lines = handle.readlines()
                self.commands_at = handle.tell()
        except FileNotFoundError:
            return
        for line in lines:
            if line.strip():
                await self.command(line)

    # ---- one connection ----

    async def connection(self) -> None:
        reader, self.writer = await asyncio.wait_for(
            asyncio.open_connection(self.master, self.args.port), 1)
        log("connected", master=self.master)
        try:
            self.send(hello=pb.Hello(protocol=self.args.protocol, id=self.args.id, rev=self.rev,
                                     boot_id=self.boot_id, rescue=self.rescue,
                                     width=0 if self.rescue else self.args.width))
            splitter = io.Splitter()
            welcomed = False
            connected = time.monotonic()
            while True:
                try:
                    data = await asyncio.wait_for(reader.read(1024), 0.2)
                except asyncio.TimeoutError:
                    data = None
                if data == b"":
                    raise ConnectionResetError("closed by the master")
                for body in splitter.feed(data or b""):
                    message = pb.ToRow.FromString(body)
                    if not welcomed:
                        if message.WhichOneof("body") != "welcome":
                            raise ConnectionResetError("no Welcome first")
                        log("got", type="welcome", master_id=message.welcome.master_id)
                        if self.master_id and message.welcome.master_id != self.master_id:
                            raise ConnectionResetError("not the master this row is paired with")
                        welcomed = True
                        self.send_status()
                        self.send_units()
                        if self.pending_update_state:
                            self.send(update_state=self.pending_update_state)
                            self.pending_update_state = None
                        continue
                    await self.got(message)
                if not welcomed:
                    if time.monotonic() - connected > WELCOME_TIMEOUT_S:
                        raise ConnectionResetError("no Welcome")
                    continue
                await self.read_commands()
                now = time.monotonic()
                if self.busy != self.busy_sent or now - self.last_status >= STATUS_EVERY_S:
                    self.send_status()
                if now - self.last_units >= UNITS_EVERY_S:
                    self.send_units()
                await self.writer.drain()
        finally:
            self.busy = False
            self.writer.close()

    # ---- pairing ----

    async def serve_pair(self, reader, writer) -> None:
        caller = writer.get_extra_info("peername")[0]
        try:
            head = (await reader.readuntil(b"\r\n\r\n")).decode(errors="replace")
            length = next((int(line.split(":")[1]) for line in head.split("\r\n")
                           if line.lower().startswith("content-length:")), 0)
            form = (await reader.readexactly(length)).decode(errors="replace")
            master = dict(pair.split("=", 1) for pair in form.split("&") if "=" in pair).get("master", "")
            if head.split()[:2] != ["POST", "/pair"]:
                status, answer = "404 Not Found", {"error": "not found"}
            elif not master:
                status, answer = "400 Bad Request", {"error": "master"}
            elif self.master_id and master != self.master_id:
                status, answer = "409 Conflict", {"error": "paired", "master": self.master_id}
            else:
                self.master, self.master_id = caller, master
                status = "200 OK"
                answer = {"name": self.args.id, "version": self.rev, "plat": "esp01",
                          "width": self.args.width, "rescue": self.rescue, "master": master,
                          "masterHost": caller, "linked": False}
            log("pair", caller=caller, master=master, status=status)
            body = json.dumps(answer).encode()
            writer.write((f"HTTP/1.0 {status}\r\nContent-Type: application/json\r\n"
                          f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n").encode() + body)
            await writer.drain()
        except (ConnectionError, asyncio.IncompleteReadError, asyncio.LimitOverrunError, ValueError) as error:
            log("pair-error", caller=caller, error=repr(error))
        finally:
            writer.close()

    async def run(self) -> None:
        if self.args.pair_port:
            await asyncio.start_server(self.serve_pair, "0.0.0.0", self.args.pair_port)
            log("pairing", port=self.args.pair_port)
        backoff = 0
        while True:
            if self.master is None:
                await asyncio.sleep(0.2)
                continue
            try:
                await self.connection()
            except Released:
                log("released")
                if not self.args.pair_port:
                    return
                continue
            except (OSError, asyncio.TimeoutError, ValueError) as error:
                reason = str(error) or type(error).__name__
                log("disconnected", reason=reason)
            # A connection that was asked to end is not a failing one.
            deliberate = reason in ("restarting", "dropped on command")
            backoff = BACKOFF_MIN_S if deliberate else min(max(backoff * 2, BACKOFF_MIN_S),
                                                           BACKOFF_MAX_S)
            await asyncio.sleep(backoff * self.args.backoff_scale)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--master", help="the master's address; without it the row is unpaired")
    parser.add_argument("--pair-port", type=int, default=0, help="serve POST /pair on this port")
    parser.add_argument("--port", type=int, default=io.PORT)
    parser.add_argument("--id", required=True, help="this row's name")
    parser.add_argument("--paired-with", help="close the connection to a master of another name")
    parser.add_argument("--rev", default="fakerow1")
    parser.add_argument("--width", type=int, default=16)
    parser.add_argument("--protocol", type=int, default=io.PROTOCOL)
    parser.add_argument("--rescue", action="store_true")
    parser.add_argument("--units")
    parser.add_argument("--commands")
    parser.add_argument("--render-s", type=float, default=4.0, help="how long a render holds the row")
    parser.add_argument("--job-scale", type=float, default=1.0, help="multiplies every job's duration")
    parser.add_argument("--backoff-scale", type=float, default=1.0)
    args = parser.parse_args()
    if not args.master and not args.pair_port:
        parser.error("give --master, or --pair-port to wait for one")
    asyncio.run(Row(args).run())


if __name__ == "__main__":
    main()
