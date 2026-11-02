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
    if which == "login":  # examples/login (admin / secret)
        from gradio_client.exceptions import AuthenticationError
        for bad in (None, ("admin", "nope")):
            try:
                Client(URL, verbose=False, auth=bad)
                raise AssertionError(f"auth={bad} should be refused")
            except (AuthenticationError, ValueError) as e:
                pass
        c = Client(URL, verbose=False, auth=("admin", "secret"))
        r = c.predict("Hanako", api_name="/predict")
        assert r.startswith("Hello, Hanako! (logged in as admin from "), r
        print("login ok: no auth / wrong password refused;", r)
        c.close()
        return
    c = Client(URL, verbose=False, download_files=which not in ("llm", "llm2"))
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
        img, size = c.predict(handle_file(png), 50, "png", api_name="/predict")
        assert Path(img).read_bytes()[:8] == bytes.fromhex("89504e470d0a1a0a"), img
        assert " x " in size, size
        # JPEG out, then that JPEG back in (stb_image reads it)
        jpg, size2 = c.predict(handle_file(png), 50, "jpeg", api_name="/predict")
        assert Path(jpg).suffix == ".jpg" and Path(jpg).read_bytes()[:2] == bytes.fromhex("ffd8"), jpg
        img3, size3 = c.predict(handle_file(jpg), 0, "png", api_name="/predict")
        assert size3 == size2 == size, (size, size2, size3)
        # WebP is refused with a clear message
        import tempfile
        webp = Path(tempfile.gettempdir()) / "gradio_cpp_test.webp"
        webp.write_bytes(b"RIFF\x1a\x00\x00\x00WEBPVP8 " + b"\x00" * 14)
        try:
            c.predict(handle_file(str(webp)), 50, "png", api_name="/predict")
            raise AssertionError("webp should fail")
        except AppError as e:
            assert "WebP is not supported" in str(e), e
        try:
            c.predict(None, 50, "png", api_name="/predict")
            raise AssertionError("expected an error")
        except AppError as e:  # gr::Error from the handler reaches the client
            assert "no image" in str(e), e
        print("sepia ok:", size, "-> png, jpeg, jpeg in; webp refused")
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
    elif which == "vision":
        ann, hist, top = c.predict(handle_file(png), 4, 3, api_name="/predict")
        assert Path(ann["image"]).exists() and len(ann["annotations"]) == 4, ann
        assert all(Path(a["image"]).exists() for a in ann["annotations"])
        assert hist["columns"] == ["luminance", "pixels"] and hist["mark"] == "line" and len(top["data"]) == 3, top
        print("vision ok:", [a["label"] for a in ann["annotations"]])
    elif which == "studio":
        f = handle_file(png)
        mask, info = c.predict({"background": f, "layers": [handle_file(png)], "composite": handle_file(png)},
                               api_name="/extract")
        assert Path(mask).exists() and "pixels" in info, info
        obj = c.predict(24, api_name="/torus")
        assert Path(obj).read_text().count("\nf ") == 24 * 12, obj
        plot = c.predict(50, api_name="/scatter")
        assert plot["mark"] == "point" and plot["columns"] == ["x", "y", "arm"] and len(plot["data"]) == 50, plot
        assert plot["datatypes"] == {"x": "quantitative", "y": "quantitative", "arm": "nominal"}, plot["datatypes"]
        print("studio ok:", info, "; torus faces ok; plot", plot["mark"], plot["datatypes"])
    elif which == "extras":
        # gr::State lives in the session: three calls count 1, 2, 3; a new client starts again
        assert [c.predict(api_name="/add_one") for _ in range(3)] == ["1", "2", "3"]
        c2 = Client(URL, verbose=False)
        assert c2.predict(api_name="/add_one") == "1"
        c2.close()
        job = c.submit(5, api_name="/slow")
        progress = []
        while not job.done():
            st = job.status()
            if st.progress_data:
                progress.append(st.progress_data[0].desc)
            time.sleep(0.05)
        assert job.result() == "done 5 steps", job.result()
        assert any(p and p.startswith("step") for p in progress), progress
        df, label = c.predict("def f():\n    import os\n", api_name="/analyze")
        assert label["label"] == "Python", label
        assert c.predict(df, api_name="/sum_counts") > 0
        import math, struct, tempfile, wave
        wav = Path(tempfile.gettempdir()) / "gradio_cpp_test.wav"
        with wave.open(str(wav), "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(8000)
            w.writeframes(b"".join(struct.pack("<h", int(8000 * math.sin(i / 8))) for i in range(8000)))
        back, info = c.predict(handle_file(str(wav)), api_name="/echo_audio")
        assert Path(back).read_bytes() == wav.read_bytes() and "audio/wav" in info, info
        print("extras ok: audio round trip", info, ";", end=" ")
        print("extras ok: state 1,2,3 then 1 in a new session; progress", sorted(set(progress))[:3], "; label", label["label"])
    elif which == "llm":  # a real model behind /chat: any non-empty answer, streamed
        q = sys.argv[3] if len(sys.argv) > 3 else "Describe this image in one sentence."
        job = c.submit({"text": q, "files": [handle_file(png)]}, api_name="/chat")
        parts = list(job)
        r = job.result()
        assert r.strip(), r
        print(f"llm ok ({len(parts)} streamed updates):", r)
    elif which == "llm2":  # two turns through the UI event (fn_index 0), so history is sent back
        t0 = time.time()
        _, hist = c.predict({"text": "Describe this image in one sentence.", "files": [handle_file(png)]}, [],
                            fn_index=0)
        t1 = time.time()
        _, hist = c.predict({"text": "What color is the background?", "files": []}, hist, fn_index=0)
        t2 = time.time()
        roles = [m["role"] for m in hist]
        assert roles == ["user", "user", "assistant", "user", "assistant"], roles
        assert hist[-1]["content"].strip(), hist
        print(f"llm2 ok: turn1 {t1 - t0:.1f}s, turn2 {t2 - t1:.1f}s")
        for m in hist:
            print("  ", m["role"], ":", m["content"] if isinstance(m["content"], str) else "[image]")
    else:
        raise SystemExit("unknown test " + which)
    c.close()


if __name__ == "__main__":
    main()
