"""Fake cluster follower (#278, epic #270) — a tiny HTTP server that
impersonates a v2 master follower so the LEADER side (#273) can be
exercised without hardware: 4-6-row layouts, degraded members, epoch/seq
abuse, rollout sequencing.

Contract mirrored from firmware/v2/Master/ClusterFollowerPolicy.h and the
/cluster/* endpoints in WebEndpoints.cpp (form-encoded requests, JSON
replies). Keep the two in sync — the pytest suite in
test_fake_follower.py pins the shared shape.

Bench use (this machine as N rows of a wall):

    python tests/fake_follower.py --count 4 --base-port 8801

then point the real leader's member table at <this-host>:8801..8804 and
watch the received segments print live.

Failure injection for degraded-member drills: POST /drill/mute (stop
answering anything but /drill/*), POST /drill/unmute.

Fleet-rollout drills (#276): the fake also accepts the leader's streamed
POST /firmware/master?md5=…&v=… (multipart field "firmware"), verifies the
MD5, then "reboots" (goes silent for --reboot-secs, drops epoch/seq like a
real boot) and comes back reporting the ?v= rev — so `--rev old0000`
against a real leader exercises the full converge → rejoin → next-member
sequence. `--rollback` keeps the old rev after reboot (native-rollback
drill: the leader must burn an attempt and eventually give up);
POST /drill/ota-busy / /drill/ota-free force the 409 transient path.
"""

import argparse
import hashlib
import json
import math
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse


class FollowerState:
    """The epoch/seq/membership brain of one fake follower — the python
    twin of ClusterFollowerPolicy.h."""

    def __init__(self, name="fake-follower", rev="fake0000", width=16,
                 rollback=False, reboot_secs=3.0, plat=""):
        self.name = name
        self.rev = rev
        self.width = width
        # #297 platform tag: "" = same platform as the leader (S3 twin);
        # "esp01" impersonates the #298 dumb row — join/ping replies gain
        # the plat key + heap/rssi/up vitals, and the leader must never
        # stream firmware at it.
        self.plat = plat
        self.rollback = rollback      # keep the old rev after a "flash"
        self.reboot_secs = reboot_secs
        # #294 health drills: the advertised fault bitmap (bit i = unit i).
        self.fault_mask = 0
        self.wear = False
        # #295 sticky leadership: the firmware's 25 s contact-fresh window.
        self.contact_fresh_secs = 25.0
        self.lock = threading.Lock()
        # Rollout drill state — survives leave() like real flash would.
        self.rebooting = False
        self.ota_busy = False
        self.ota_in_flight = False  # concurrent-upload guard (real: #191)
        self.uploads = []  # {"size", "md5", "v"} per accepted flash
        self.reset()

    def reset(self):
        self.clustered = False
        self.leader_name = ""
        self.leader_host = ""
        self.row = 0
        self.epoch = None
        self.last_seq = 0
        self.segment = ""
        self.renders = []  # every APPLIED render, for assertions/bench eyes
        self.muted = False
        self.last_contact = 0.0
        # #294 rung 2: the leader's ping-piggybacked digest.
        self.digest_raw = ""
        self.digest_received = 0.0
        self.self_index = -1

    def fault_mask_hex(self):
        return "%0*X" % (max(1, math.ceil(self.width / 4)), self.fault_mask)

    def health_keys(self):
        """The #294 additive health keys both the join and ping replies carry."""
        keys = {"detected": self.width, "faulty": bin(self.fault_mask).count("1"),
                "faultMask": self.fault_mask_hex(), "wear": self.wear}
        if self.plat:
            # #297/#298: foreign-platform rows also report their vitals.
            keys.update({"plat": self.plat, "heap": 28000, "rssi": -61,
                         "up": int(time.monotonic())})
        return keys

    def join_conflict(self, leader_host):
        """#295 sticky leadership — the python twin of
        clusterFollowerJoinConflicts(): reject a DIFFERENT leader only while
        the current one is demonstrably alive."""
        with self.lock:
            if not self.clustered or leader_host == self.leader_host:
                return False
            return time.monotonic() - self.last_contact < self.contact_fresh_secs

    def join(self, leader_name, leader_host, row, epoch):
        with self.lock:
            self.last_contact = time.monotonic()
            self.clustered = True
            self.leader_name = leader_name
            self.leader_host = leader_host
            self.row = row
            if self.epoch != epoch:
                # Seq tracking resets only on a NEW epoch — a same-epoch
                # rejoin must keep rejecting delayed retries of old renders.
                self.epoch = epoch
                self.last_seq = 0

    def accept_render(self, epoch, seq, text, speed, commit_at_ms):
        """Returns (applied, not_clustered) per ClusterFollowerPolicy."""
        with self.lock:
            if not self.clustered:
                return False, True
            if self.epoch is not None and epoch == self.epoch and seq <= self.last_seq:
                return False, False  # duplicate: contact only
            self.epoch = epoch
            self.last_seq = seq
            self.segment = text
            self.last_contact = time.monotonic()
            self.renders.append(
                {"epoch": epoch, "seq": seq, "text": text, "speed": speed,
                 "commitAtMs": commit_at_ms}
            )
            return True, False

    def ping(self, digest="", you_index=-1):
        with self.lock:
            if not self.clustered:
                return False
            self.last_contact = time.monotonic()
            if digest:
                self.digest_raw = digest
                self.digest_received = time.monotonic()
                self.self_index = you_index
            return True

    def leave(self):
        with self.lock:
            self.reset()

    def flash(self, size, md5, leader_rev):
        """A verified /firmware/master upload landed: record it and mimic
        the reboot — silent for reboot_secs, epoch/seq forgotten (fresh
        boot), membership kept (the real follower persists it in NVS),
        rev switched to the leader's unless the rollback drill is on."""
        with self.lock:
            self.uploads.append({"size": size, "md5": md5, "v": leader_rev})
            self.rebooting = True

        def finish():
            with self.lock:
                if not self.rollback and leader_rev:
                    self.rev = leader_rev
                self.epoch = None
                self.last_seq = 0
                self.rebooting = False

        threading.Timer(self.reboot_secs, finish).start()


def multipart_payload(body, boundary):
    """Extract the (single) part payload from a multipart/form-data body —
    the shape the leader's streaming upload produces. First-match semantics
    on the closing delimiter, like the device parser (ESPAsyncWebServer ends
    the file field at the FIRST in-body boundary hit) — a payload containing
    its own boundary truncates here exactly like on hardware (#292).
    None on framing mismatch."""
    delim = ("--" + boundary).encode()
    if not body.startswith(delim):
        return None
    head = body.find(b"\r\n\r\n")
    if head < 0:
        return None
    tail = body.find(b"\r\n" + delim, head + 4)
    if tail < 0:
        return None
    return body[head + 4:tail]


class FakeFollowerHandler(BaseHTTPRequestHandler):
    # Injected per-server via functools-partial-style subclassing (make_server).
    state = None
    verbose = False

    def log_message(self, fmt, *args):  # silence the default request spam
        if self.verbose:
            super().log_message(fmt, *args)

    def _send(self, status, body, content_type="application/json"):
        payload = body.encode()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def _form(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length).decode() if length else ""
        return {k: v[0] for k, v in parse_qs(raw, keep_blank_values=True).items()}

    def do_POST(self):
        st = self.state
        if self.path == "/drill/mute":
            st.muted = True
            return self._send(200, "muted", "text/plain")
        if self.path == "/drill/unmute":
            st.muted = False
            return self._send(200, "unmuted", "text/plain")
        if self.path == "/drill/ota-busy":
            st.ota_busy = True
            return self._send(200, "ota busy", "text/plain")
        if self.path == "/drill/ota-free":
            st.ota_busy = False
            return self._send(200, "ota free", "text/plain")
        if self.path == "/drill/fault":
            form = self._form()
            st.fault_mask = int(form.get("mask", "0"), 16)
            st.wear = form.get("wear", "0") in ("1", "true")
            return self._send(200, "fault mask set", "text/plain")
        if st.muted or st.rebooting:
            # Dead-member / mid-reboot drill: hang until the leader's
            # timeout fires.
            threading.Event().wait(10)
            return

        if self.path.startswith("/firmware/master"):
            return self._handle_master_ota()

        if self.path == "/cluster/join":
            form = self._form()
            missing = [k for k in ("leaderHost", "row", "epoch") if k not in form]
            if missing:
                return self._send(400, "Missing leaderHost/row/epoch", "text/plain")
            if st.join_conflict(form["leaderHost"]):
                # #295 sticky leadership: the current leader is alive — the
                # joiner must demote on this marker.
                return self._send(409, json.dumps(
                    {"error": "other-leader", "leaderHost": st.leader_host,
                     "leaderName": st.leader_name}))
            st.join(form.get("leaderName", form["leaderHost"]), form["leaderHost"],
                    int(form["row"]), int(form["epoch"]))
            if self.verbose:
                print(f"[{st.name}] joined by {st.leader_name} as row {st.row}")
            reply = {"name": st.name, "rev": st.rev, "width": st.width,
                     "protocol": 1}
            reply.update(st.health_keys())
            return self._send(200, json.dumps(reply))

        if self.path == "/cluster/render":
            form = self._form()
            if any(k not in form for k in ("epoch", "seq", "text")):
                return self._send(400, "Missing epoch/seq/text", "text/plain")
            applied, not_clustered = st.accept_render(
                int(form["epoch"]), int(form["seq"]), form["text"],
                int(form.get("speed", 80)), int(form.get("commitAtMs", 0)))
            if not_clustered:
                return self._send(409, json.dumps({"error": "not clustered"}))
            if self.verbose and applied:
                print(f"[{st.name}] row {st.row} shows: “{form['text']}”")
            return self._send(200, json.dumps(
                {"applied": applied, "seq": int(form["seq"])}))

        if self.path == "/cluster/ping":
            form = self._form()
            if not st.ping(form.get("digest", ""),
                           int(form.get("you", "-1"))):
                return self._send(409, json.dumps({"error": "not clustered"}))
            reply = {"state": "clustered", "epoch": st.epoch or 0,
                     "seq": st.last_seq, "width": st.width,
                     "rev": st.rev}
            reply.update(st.health_keys())
            # Key order matches the firmware reply (state/epoch/seq, then
            # width/detected/faulty/faultMask/wear/rev, then the #297
            # additive plat/vitals block when foreign-platform).
            keys = ["state", "epoch", "seq", "width", "detected",
                    "faulty", "faultMask", "wear", "rev"]
            keys += [k for k in ("plat", "heap", "rssi", "up") if k in reply]
            ordered = {k: reply[k] for k in keys}
            return self._send(200, json.dumps(ordered))

        if self.path == "/cluster/leave":
            st.leave()
            return self._send(200, "ok", "text/plain")

        return self._send(404, "not found", "text/plain")

    def _handle_master_ota(self):
        """The real /firmware/master contract the leader's rollout streams
        into (WebEndpoints.cpp): multipart field "firmware", mandatory
        ?md5=, 409 while another flash owns the target, 200 then reboot."""
        st = self.state
        # Concurrent-upload guard: the real endpoint's otaOwnerRequest
        # rejects an overlapping POST with 409 — this ThreadingHTTPServer
        # must too, or the harness hides overlap bugs the ESP32 would 409.
        with st.lock:
            overlapped = st.ota_in_flight
            st.ota_in_flight = True
        try:
            return self._handle_master_ota_locked(overlapped)
        finally:
            if not overlapped:  # only the owning request releases the slot
                with st.lock:
                    st.ota_in_flight = False

    def _handle_master_ota_locked(self, overlapped):
        st = self.state
        query = {k: v[0] for k, v in
                 parse_qs(urlparse(self.path).query).items()}
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length else b""
        if st.ota_busy or overlapped:
            return self._send(
                409, "Another master OTA upload is already in progress "
                     "— retry when it finishes", "text/plain")
        md5_param = query.get("md5", "").lower()
        if not md5_param:
            return self._send(400, "md5 query parameter is required",
                              "text/plain")
        content_type = self.headers.get("Content-Type", "")
        if "boundary=" not in content_type:
            return self._send(500, "Master OTA failed: not multipart",
                              "text/plain")
        boundary = content_type.split("boundary=")[-1].strip()
        payload = multipart_payload(body, boundary)
        if payload is None:
            return self._send(500, "Master OTA failed: bad multipart",
                              "text/plain")
        received_md5 = hashlib.md5(payload).hexdigest()
        if received_md5 != md5_param:
            return self._send(500, "Master OTA failed: MD5 mismatch",
                              "text/plain")
        self._send(200, "Master firmware flashed; rebooting…", "text/plain")
        if self.verbose:
            print(f"[{st.name}] flashed {len(payload)} bytes "
                  f"(md5 {received_md5}) — rebooting")
        st.flash(len(payload), received_md5, query.get("v", ""))
        return None

    def do_GET(self):
        st = self.state
        if (st.muted or st.rebooting) and not self.path.startswith("/drill/"):
            threading.Event().wait(10)
            return
        if self.path == "/cluster/digest":
            with st.lock:
                digest, received = st.digest_raw, st.digest_received
            if not digest:
                return self._send(404, json.dumps({"error": "no digest"}))
            age_ms = int((time.monotonic() - received) * 1000)
            return self._send(
                200, '{"ageMs":%d,"digest":%s}' % (age_ms, digest))
        if self.path == "/cluster/health":
            return self._send(200, json.dumps({
                "state": "clustered" if st.clustered else "standalone",
                "leaderName": st.leader_name, "leaderHost": st.leader_host,
                "row": st.row, "epoch": st.epoch or 0, "seq": st.last_seq,
                "segment": st.segment, "rev": st.rev, "width": st.width,
                "detected": 0, "faulty": 0,
            }))
        return self._send(404, "not found", "text/plain")


def make_server(port, name="fake-follower", rev="fake0000", width=16,
                verbose=False, rollback=False, reboot_secs=3.0, plat=""):
    """One fake follower on localhost:port. Returns (server, state); run
    server.serve_forever() in a thread; state carries the assertions."""
    state = FollowerState(name=name, rev=rev, width=width, rollback=rollback,
                          reboot_secs=reboot_secs, plat=plat)

    class Handler(FakeFollowerHandler):
        pass

    Handler.state = state
    Handler.verbose = verbose
    server = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    return server, state


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--count", type=int, default=1, help="followers to run")
    parser.add_argument("--base-port", type=int, default=8801)
    parser.add_argument("--width", type=int, default=16)
    parser.add_argument("--rev", default="fake0000")
    parser.add_argument("--plat", default="",
                        help="platform tag (#297): esp01 impersonates the "
                             "dumb row — the leader must never stream "
                             "firmware at it")
    parser.add_argument("--rollback", action="store_true",
                        help="keep the old rev after a flash (rollback drill)")
    parser.add_argument("--reboot-secs", type=float, default=3.0,
                        help="silent-reboot window after a flash")
    args = parser.parse_args()

    servers = []
    for i in range(args.count):
        port = args.base_port + i
        server, _ = make_server(port, name=f"fake-row-{i}", rev=args.rev,
                                width=args.width, verbose=True,
                                rollback=args.rollback,
                                reboot_secs=args.reboot_secs, plat=args.plat)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        servers.append(server)
        print(f"fake follower “fake-row-{i}” listening on :{port}")

    print("Ctrl-C to stop. Point the leader's member table at these hosts.")
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        for server in servers:
            server.shutdown()


if __name__ == "__main__":
    main()
