#!/usr/bin/env python3
"""A tiny reverse proxy for tests: serves http://127.0.0.1:<port>/gradio/... from
http://127.0.0.1:7860/... (prefix stripped, responses streamed so SSE keeps working).

    python tests/prefix_proxy.py [port] [prefix]      # default 8095 /gradio
Run the app with LaunchOptions::root_path = "/gradio".
"""
import http.client
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8095
PREFIX = sys.argv[2] if len(sys.argv) > 2 else "/gradio"
HOP = {"connection", "keep-alive", "transfer-encoding", "te", "trailer", "upgrade", "proxy-connection"}


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def forward(self):
        if self.path == PREFIX:
            self.send_response(301)
            self.send_header("Location", PREFIX + "/")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if not self.path.startswith(PREFIX + "/"):
            self.send_error(404)
            return
        body = self.rfile.read(int(self.headers.get("Content-Length", 0) or 0))
        up = http.client.HTTPConnection("127.0.0.1", 7860, timeout=600)
        headers = {k: v for k, v in self.headers.items() if k.lower() not in HOP}
        up.request(self.command, self.path[len(PREFIX):], body=body or None, headers=headers)
        r = up.getresponse()
        self.send_response(r.status)
        for k, v in r.getheaders():
            if k.lower() in HOP or k.lower() == "content-length":
                continue
            if k.lower() == "location" and v.startswith("/"):
                v = PREFIX + v
            self.send_header(k, v)
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        while True:
            chunk = r.read1(65536)
            if not chunk:
                break
            self.wfile.write(b"%x\r\n%s\r\n" % (len(chunk), chunk))
            self.wfile.flush()
        self.wfile.write(b"0\r\n\r\n")
        up.close()

    do_GET = do_POST = forward


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
