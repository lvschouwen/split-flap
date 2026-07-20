"""Fake cluster leader (#298, epic #270) — the counterpart of the v2
master's tests/fake_follower.py: a tiny HTTP *client* that speaks the S3
leader's exact outbound wire shapes (ClusterLeader.cpp: form-encoded
join/render/ping/leave, the #294 digest piggyback, and the #276-style
streamed /firmware/master upload) so the ESP-01 follower can be exercised
without an S3 on the bench.

Bench use (drive a real ESP-01 row):

    python tests/fake_leader.py 192.168.15.95 --row 0 --text "HELLO ROW"

joins the board, renders the text with a commitAt ~400 ms out, then keeps
pinging at the leader's 10 s cadence, printing the health replies.

The pytest suite (test_fake_leader.py) runs this driver against the python
twin of the follower (fake_follower.py, plat=esp01 variant) to pin the
shared wire contract from the LEADER side; the real firmware paths are
bench tier.
"""

import argparse
import hashlib
import json
import time
import urllib.error
import urllib.request
from urllib.parse import urlencode


class FakeLeader:
    """Speaks the leader's outbound /cluster wire (ClusterLeaderPolicy.h
    shapes). Seq is minted per render POST like the real leader."""

    def __init__(self, leader_host="10.0.0.9", leader_name="fake-leader",
                 epoch=7):
        self.leader_host = leader_host
        self.leader_name = leader_name
        self.epoch = epoch
        self.seq = 0

    def _post(self, base, path, fields=None, timeout=5):
        data = urlencode(fields or {}).encode()
        request = urllib.request.Request(base + path, data=data,
                                         method="POST")
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return response.status, response.read().decode()
        except urllib.error.HTTPError as error:
            return error.code, error.read().decode()

    def join(self, base, row=0, tz=""):
        fields = {
            "leaderName": self.leader_name, "leaderHost": self.leader_host,
            "row": row, "epoch": self.epoch,
        }
        if tz:
            # #342 additive: the leader's POSIX zone — the row persists it
            # for the dead-leader clock fallback.
            fields["tz"] = tz
        return self._post(base, "/cluster/join", fields)

    def render(self, base, text, speed=80, commit_lead_ms=400, seq=None):
        """commitAtMs = leader-now + lead, exactly like submitGrid()."""
        if seq is None:
            self.seq += 1
            seq = self.seq
        commit_at = int(time.time() * 1000) + commit_lead_ms
        return self._post(base, "/cluster/render", {
            "epoch": self.epoch, "seq": seq, "text": text, "speed": speed,
            "commitAtMs": commit_at,
        })

    def ping(self, base, digest="", you=-1):
        fields = {}
        if digest:
            # The #294 piggyback — the ESP-01 ignores it by design.
            fields = {"digest": digest, "you": you}
        return self._post(base, "/cluster/ping", fields)

    def leave(self, base):
        return self._post(base, "/cluster/leave")


def rollout_boundary(image):
    """Python twin of clusterRolloutBoundary() (#292)."""
    return "sfr-" + hashlib.md5(image).hexdigest()


def stream_firmware(base, image, v="", md5=None, timeout=30):
    """POST /firmware/master exactly the way the leader's rollout frames it
    (multipart field "firmware", md5-derived boundary, ?md5=&v=). The #297
    platform guard means the REAL leader never sends this at an esp01
    member — this helper exists for ota-flash.sh-style bench drills and the
    contract pytest."""
    boundary = rollout_boundary(image)
    body = (
        (f"--{boundary}\r\n"
         'Content-Disposition: form-data; name="firmware"; '
         'filename="firmware.bin"\r\n'
         "Content-Type: application/octet-stream\r\n\r\n").encode()
        + image + f"\r\n--{boundary}--\r\n".encode()
    )
    digest = md5 if md5 is not None else hashlib.md5(image).hexdigest()
    url = f"{base}/firmware/master?md5={digest}"
    if v:
        url += f"&v={v}"
    request = urllib.request.Request(
        url, data=body, method="POST",
        headers={"Content-Type": f"multipart/form-data; boundary={boundary}"})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status, response.read().decode()
    except urllib.error.HTTPError as error:
        return error.code, error.read().decode()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("host", help="follower IP/hostname[:port]")
    parser.add_argument("--row", type=int, default=0)
    parser.add_argument("--text", default="", help="render once after join")
    parser.add_argument("--speed", type=int, default=80)
    parser.add_argument("--leader-host", default="",
                        help="leaderHost to claim (default: local IP guess)")
    parser.add_argument("--epoch", type=int, default=int(time.time()) & 0xFFFF)
    parser.add_argument("--leave", action="store_true",
                        help="send /cluster/leave and exit")
    args = parser.parse_args()

    base = args.host if args.host.startswith("http") else f"http://{args.host}"
    leader_host = args.leader_host
    if not leader_host:
        import socket
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect(("8.8.8.8", 80))
            leader_host = s.getsockname()[0]
        finally:
            s.close()

    leader = FakeLeader(leader_host=leader_host, epoch=args.epoch)
    if args.leave:
        print("leave:", *leader.leave(base))
        return

    status, body = leader.join(base, row=args.row)
    print(f"join: HTTP {status} {body}")
    if status != 200:
        return
    if args.text:
        status, body = leader.render(base, args.text, speed=args.speed)
        print(f"render: HTTP {status} {body}")

    print("pinging every 10 s (Ctrl-C to stop; the row blanks ~2 min after)")
    try:
        while True:
            time.sleep(10)
            status, body = leader.ping(base)
            print(f"ping: HTTP {status} {body}")
            if status == 409:
                print("membership lost — rejoining")
                print("join:", *leader.join(base, row=args.row))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
