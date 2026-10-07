#!/usr/bin/env python3
"""Serves the page as built from web/ and passes everything else to a master.

For working on the page without flashing: the page is rebuilt on every load,
its requests go to the real master.

    python3 web/dev_server.py 192.168.15.88 [--port 8088] [--read-only]

The master takes changing requests from its own page only, so the Origin
header is not passed on; --read-only refuses every changing request here.
"""
import argparse
import http.client
import pathlib
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PROJECT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PROJECT))
import web_bundle  # noqa: E402


def handler(master, read_only):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def page(self):
            try:
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
            if read_only and self.command != "GET":
                self.send_error(403, "the dev server was started --read-only")
                return
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

        def do_GET(self):
            if self.path in ("/", "/console"):
                self.page()
            else:
                self.relay()

        do_POST = do_PUT = relay

        def log_message(self, *args):
            pass

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("master", help="the master's address")
    ap.add_argument("--port", type=int, default=8088)
    ap.add_argument("--read-only", action="store_true")
    args = ap.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", args.port), handler(args.master, args.read_only))
    print(f"http://127.0.0.1:{args.port}/ -> {args.master}")
    server.serve_forever()


if __name__ == "__main__":
    main()
