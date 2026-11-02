#!/usr/bin/env python3
"""A tiny OpenAI-compatible /v1/chat/completions stand-in for tests.

Streams back a description of what it received: the number of messages, the
text of the last user turn and, for each image, its format and pixel size read
from the decoded data URI. That proves images really arrive as images.

    python tests/mock_openai.py [port]      # default 8080
"""
import base64
import json
import struct
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def image_info(data_uri):
    head, b64 = data_uri.split(",", 1)
    raw = base64.b64decode(b64)
    if raw[:8] == b"\x89PNG\r\n\x1a\n":
        w, h = struct.unpack(">II", raw[16:24])
        return f"PNG {w}x{h}"
    if raw[:2] == b"\xff\xd8":
        return f"JPEG {len(raw)} bytes"
    return f"{head} {len(raw)} bytes"


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        if self.path != "/v1/chat/completions":
            self.send_error(404)
            return
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        msgs = body["messages"]
        last = msgs[-1]
        texts, images = [], []
        content = last["content"]
        if isinstance(content, str):
            texts.append(content)
        else:
            for part in content:
                if part["type"] == "text":
                    texts.append(part["text"])
                elif part["type"] == "image_url":
                    images.append(image_info(part["image_url"]["url"]))
        reply = f"messages={len(msgs)} roles={'/'.join(m['role'] for m in msgs)} text={' | '.join(texts)!r}"
        if images:
            reply += " images=[" + ", ".join(images) + "]"
        if body.get("stream"):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            for i in range(0, len(reply), 8):
                chunk = {"choices": [{"index": 0, "delta": {"content": reply[i:i + 8]}}]}
                self.wfile.write(f"data: {json.dumps(chunk)}\n\n".encode())
                self.wfile.flush()
                time.sleep(0.01)
            self.wfile.write(b"data: [DONE]\n\n")
        else:
            out = json.dumps({"choices": [{"message": {"role": "assistant", "content": reply}}]}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(out)))
            self.end_headers()
            self.wfile.write(out)


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
