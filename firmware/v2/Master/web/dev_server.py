#!/usr/bin/env python3
"""Serves the page as built from web/ and passes everything else to a master.

For working on the page without flashing: the page is rebuilt on every load,
its requests go to the real master.

    python3 web/dev_server.py 192.168.15.88 [--port 8088] [--read-only] [--minutes 120]

    python3 web/dev_server.py --fixtures faults [--port 8088]

With --fixtures no master is asked: the API is answered from the documents of
a scenario in web/fixtures.py (a wall with every fault, the states of the
Firmware page), and every changing request is refused.

It stops by itself after --minutes, so a forgotten one does not stay open.

The master takes changing requests from its own page only, so this server
applies that rule in its place before passing a request on without its
Origin: a changing request must come from the page served here, and every
request must be addressed to this server by its loopback name (a web page
elsewhere cannot use it to reach the master). --read-only refuses every
changing request.
"""
import argparse
import http.client
import importlib
import json
import pathlib
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PROJECT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PROJECT))
sys.path.insert(0, str(PROJECT / "web"))
import fixtures as fixture_documents  # noqa: E402
import web_bundle  # noqa: E402


def own_hosts(port):
    return {f"127.0.0.1:{port}", f"localhost:{port}"}


def refusal(command, headers, port, read_only, fixtures=False):
    """Why this request is not served, or None."""
    hosts = own_hosts(port)
    if headers.get("Host") not in hosts:
        return "this server answers to its loopback name only"
    if command == "GET":
        return None
    if fixtures:
        return "the dev server answers from saved documents: nothing can be changed"
    if read_only:
        return "the dev server was started --read-only"
    if headers.get("Origin") not in {"http://" + host for host in hosts}:
        return "changing requests are taken from the page served here only"
    return None


def fixture_answer(docs, path):
    """(status, body, content type) for a GET of `path` from a scenario's documents."""
    doc = docs.get(urllib.parse.urlsplit(path).path)
    if doc is None:
        return 404, b'{"error":"no such document in this scenario"}', "application/json"
    if isinstance(doc, str):
        return 200, doc.encode(), "text/plain; charset=utf-8"
    return 200, json.dumps(doc).encode(), "application/json"


def handler(master, read_only, port, docs=None):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def saved(self):
            if urllib.parse.urlsplit(self.path).path == "/api/v2/stream":
                return self.saved_stream()
            status, body, kind = fixture_answer(docs, self.path)
            self.send_response(status)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(body)))
            if kind.startswith("text/plain"):
                self.send_header("X-Log-Board", "a board")
                self.send_header("X-Log-Kind", "ram")
            self.end_headers()
            self.wfile.write(body)

        # Every topic once, then silence: the page stays connected.
        def saved_stream(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            try:
                for topic, doc in fixture_documents.stream_events(docs):
                    self.wfile.write(f"event: {topic}\ndata: {json.dumps(doc)}\n\n".encode())
                self.wfile.flush()
                while True:
                    time.sleep(15)
                    self.wfile.write(b": still here\n\n")
                    self.wfile.flush()
            except OSError:
                pass

        def page(self):
            try:
                # The module list is part of what is being worked on.
                importlib.reload(web_bundle)
                body = web_bundle.build_page(PROJECT)
                status = 200
            except ValueError as error:
                body, status = f"the page does not build: {error}".encode(), 500
            self.send_response(status)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def relay(self):
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length) if length else None
            headers = {k: v for k, v in self.headers.items()
                       if k.lower() not in ("host", "origin", "referer", "connection")}
            upstream = http.client.HTTPConnection(master, timeout=30)
            try:
                upstream.request(self.command, self.path, body=body, headers=headers)
                reply = upstream.getresponse()
                self.send_response(reply.status)
                for key, value in reply.getheaders():
                    if key.lower() not in ("transfer-encoding", "connection", "content-length"):
                        self.send_header(key, value)
                self.send_header("Connection", "close")
                self.end_headers()
                # read1 hands on what has arrived: an event stream must not wait
                # for a full buffer.
                while chunk := reply.read1(4096):
                    self.wfile.write(chunk)
                    self.wfile.flush()
            except (OSError, http.client.HTTPException):
                pass
            finally:
                upstream.close()
                self.close_connection = True

        def serve(self):
            why = refusal(self.command, self.headers, port, read_only, fixtures=docs is not None)
            if why:
                self.send_error(403, why)
            elif self.command == "GET" and self.path == "/":
                self.page()
            elif docs is not None:
                self.saved()
            else:
                self.relay()

        do_GET = do_POST = do_PUT = serve

        def log_message(self, *args):
            pass

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("master", nargs="?", help="the master's address")
    ap.add_argument("--fixtures", metavar="SCENARIO", choices=fixture_documents.SCENARIOS,
                    help="answer from saved documents instead of a master: "
                         + ", ".join(fixture_documents.SCENARIOS))
    ap.add_argument("--port", type=int, default=8088)
    ap.add_argument("--read-only", action="store_true")
    ap.add_argument("--minutes", type=float, default=120, help="stop after this long")
    args = ap.parse_args()
    if bool(args.master) == bool(args.fixtures):
        ap.error("give a master's address or --fixtures, one of the two")
    docs = fixture_documents.documents(args.fixtures) if args.fixtures else None
    server = ThreadingHTTPServer(("127.0.0.1", args.port),
                                 handler(args.master, args.read_only, args.port, docs))
    server.daemon_threads = True
    print(f"http://127.0.0.1:{args.port}/ -> {args.master or 'fixtures: ' + args.fixtures}")
    stop = threading.Timer(args.minutes * 60, server.shutdown)
    stop.daemon = True
    stop.start()
    server.serve_forever()


if __name__ == "__main__":
    main()
