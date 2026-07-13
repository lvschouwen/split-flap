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
"""

import argparse
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs


class FollowerState:
    """The epoch/seq/membership brain of one fake follower — the python
    twin of ClusterFollowerPolicy.h."""

    def __init__(self, name="fake-follower", rev="fake0000", width=16):
        self.name = name
        self.rev = rev
        self.width = width
        self.lock = threading.Lock()
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

    def join(self, leader_name, leader_host, row, epoch):
        with self.lock:
            self.clustered = True
            self.leader_name = leader_name
            self.leader_host = leader_host
            self.row = row
            self.epoch = epoch
            self.last_seq = 0  # the handshake re-sends the current segment

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
            self.renders.append(
                {"epoch": epoch, "seq": seq, "text": text, "speed": speed,
                 "commitAtMs": commit_at_ms}
            )
            return True, False

    def ping(self):
        with self.lock:
            return self.clustered

    def leave(self):
        with self.lock:
            self.reset()


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
        if st.muted:
            # Dead-member drill: hang until the leader's timeout fires.
            threading.Event().wait(10)
            return

        if self.path == "/cluster/join":
            form = self._form()
            missing = [k for k in ("leaderHost", "row", "epoch") if k not in form]
            if missing:
                return self._send(400, "Missing leaderHost/row/epoch", "text/plain")
            st.join(form.get("leaderName", form["leaderHost"]), form["leaderHost"],
                    int(form["row"]), int(form["epoch"]))
            if self.verbose:
                print(f"[{st.name}] joined by {st.leader_name} as row {st.row}")
            return self._send(200, json.dumps(
                {"name": st.name, "rev": st.rev, "width": st.width, "protocol": 1}))

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
            if not st.ping():
                return self._send(409, json.dumps({"error": "not clustered"}))
            return self._send(200, json.dumps(
                {"state": "clustered", "epoch": st.epoch or 0, "seq": st.last_seq}))

        if self.path == "/cluster/leave":
            st.leave()
            return self._send(200, "ok", "text/plain")

        return self._send(404, "not found", "text/plain")

    def do_GET(self):
        st = self.state
        if st.muted and not self.path.startswith("/drill/"):
            threading.Event().wait(10)
            return
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
                verbose=False):
    """One fake follower on localhost:port. Returns (server, state); run
    server.serve_forever() in a thread; state carries the assertions."""
    state = FollowerState(name=name, rev=rev, width=width)

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
    args = parser.parse_args()

    servers = []
    for i in range(args.count):
        port = args.base_port + i
        server, _ = make_server(port, name=f"fake-row-{i}", rev=args.rev,
                                width=args.width, verbose=True)
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
