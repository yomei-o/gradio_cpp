#!/usr/bin/env python3
"""Call the running example through the official Python gradio_client.

    pip install gradio_client
    python tests/client_test.py hello|blocks|sepia|components|chat [some.png]
"""
import sys
from pathlib import Path

import time

from gradio_client import Client, handle_file
from gradio_client.exceptions import AppError

URL = "http://127.0.0.1:7860/"


def main():
    which = sys.argv[1]
    png = sys.argv[2] if len(sys.argv) > 2 else None
    c = Client(URL, verbose=False)
    if which == "hello":
        r = c.predict("World", 3, api_name="/predict")
        assert r == "Hello, World!!!", r
        r = c.predict(name="名前", intensity=1, api_name="/predict")  # keyword args use parameter_name
        assert r == "Hello, 名前!", r
        print("hello ok:", r)
    elif which == "blocks":
        r = c.predict("a b c", True, api_name="/count")
        assert r[0] == "A B C" and r[1] == {"bytes": 5, "words": 3}, r
        job = c.submit("hi", 4, api_name="/stream")
        seen = list(job)  # every generating update, rebuilt from diffs
        final = job.result()
        assert final == "1: hi\n2: hi\n3: hi\n4: hi\n", final
        assert len(seen) >= 4 and seen[0] == "1: hi\n", seen
        # cancel a long generator: the job ends early and the app keeps serving
        job = c.submit("x", 20, api_name="/stream")
        while not job.outputs():
            time.sleep(0.05)
        job.cancel()
        time.sleep(1.0)
        assert job.done() and len(job.outputs()) < 20, len(job.outputs())
        assert c.predict("ok", False, api_name="/count")[0] == "ok"
        print("blocks ok:", len(seen), "updates; final", repr(final), "; cancelled after", len(job.outputs()))
    elif which == "sepia":
        img, size = c.predict(handle_file(png), 50, api_name="/predict")
        assert Path(img).exists() and Path(img).stat().st_size > 1000, img
        assert " x " in size, size
        try:
            c.predict(None, 50, api_name="/predict")
            raise AssertionError("expected an error")
        except AppError as e:  # gr::Error from the handler reaches the client
            assert "no image" in str(e), e
        print("sepia ok:", size, "->", img)
    elif which == "components":
        r = c.predict("vegetable", "leek", ["pepper"], api_name="/order")
        assert r == "vegetable: leek +pepper", r
        gal = c.predict(3, api_name="/swatches")
        assert len(gal) == 3 and Path(gal[0]["image"]).exists(), gal
        print("components ok:", r, "/", len(gal), "gallery images")
    elif which == "chat":
        r = c.predict({"text": "hello", "files": [handle_file(png)]}, api_name="/chat")
        assert "Got image" in r and "You said: hello" in r, r
        print("chat ok:", r.replace("\n", " ")[:80])
    else:
        raise SystemExit("unknown test " + which)
    c.close()


if __name__ == "__main__":
    main()
