#!/usr/bin/env python3
"""Browser end-to-end check with Playwright.

Start an example first, then:
    python tests/e2e.py sepia  <png> [outdir]
    python tests/e2e.py blocks [outdir]
"""
import sys
from pathlib import Path

from playwright.sync_api import sync_playwright

URL = "http://127.0.0.1:7860/"


def sepia(page, png, out):
    page.set_input_files("input[type=file]", png)
    page.wait_for_function("document.querySelector('.drop img') !== null")
    page.wait_for_timeout(500)  # let the upload finish
    page.get_by_role("button", name="Submit").click()
    page.wait_for_function(
        "[...document.querySelectorAll('.drop img')].some(i => i.src.includes('sepia.png'))", timeout=15000)
    size = page.locator("input[type=text]").last.input_value()
    page.screenshot(path=str(out / "e2e_sepia.png"), full_page=True)
    assert " x " in size, size
    print("sepia ok:", size)


def blocks(page, out):
    page.locator("textarea").first.fill("hello gradio_cpp")
    page.get_by_role("button", name="Stream").click()
    page.wait_for_function("document.querySelectorAll('textarea')[1].value.includes('5: hello')", timeout=15000)
    page.get_by_role("button", name="Count").click()
    page.wait_for_function("document.querySelector('pre.json').textContent.includes('words')", timeout=15000)
    page.screenshot(path=str(out / "e2e_blocks.png"), full_page=True)
    print("blocks ok:", page.locator("pre.json").text_content().replace("\n", " "))


def last_bot(page):
    return page.locator(".msg.assistant").last.text_content()


def wait_bot_done(page, needle, timeout=30000):
    page.wait_for_function(
        """needle => { const m = [...document.querySelectorAll('.msg.assistant')].pop();
                       return m && m.textContent.includes(needle) && !document.querySelector('.pending'); }""",
        arg=needle, timeout=timeout)


def chat(page, png, out, needle_img, needle_text):
    """Send image + text, then a text-only follow-up; checks history and image delivery."""
    page.set_input_files(".mm input[type=file]", png)
    page.wait_for_selector(".mm .thumb img")
    page.locator(".mm textarea").fill("この画像は何？")
    page.locator(".mm .send").click()
    page.wait_for_selector(".msg.user img")
    wait_bot_done(page, needle_img)
    first = last_bot(page)
    assert page.locator(".mm .thumb").count() == 0, "attachments not cleared"
    page.locator(".mm textarea").fill("second turn")
    page.keyboard.press("Enter")
    wait_bot_done(page, needle_text)
    second = last_bot(page)
    page.screenshot(path=str(out / "e2e_chat.png"), full_page=True)
    print("chat ok:\n  1:", first, "\n  2:", second)
    return first, second


def main():
    which = sys.argv[1]
    errors = []
    with sync_playwright() as p:
        b = p.chromium.launch()
        page = b.new_page(viewport={"width": 1280, "height": 800})
        page.on("pageerror", lambda e: errors.append(str(e)))
        page.goto(URL)
        page.wait_for_selector(".block")
        if which == "sepia":
            sepia(page, sys.argv[2], Path(sys.argv[3] if len(sys.argv) > 3 else "."))
        elif which == "echo":
            out = Path(sys.argv[3] if len(sys.argv) > 3 else ".")
            first, second = chat(page, sys.argv[2], out, "earlier messages", "second turn")
            assert "Got image" in first and "0 earlier" in first, first
            assert "3 earlier" in second, second  # user image + user text + assistant
        elif which == "stop":  # against chat_echo: Stop cuts a streaming reply, the app keeps working
            page.locator(".mm textarea").fill(" ".join(f"w{i}" for i in range(300)))
            page.keyboard.press("Enter")
            stop = page.get_by_role("button", name="Stop")
            stop.wait_for(state="visible")
            page.wait_for_timeout(500)
            stop.click()
            stop.wait_for(state="hidden")
            cut = last_bot(page)
            assert "earlier messages" not in cut, cut
            page.wait_for_timeout(500)
            assert last_bot(page) == cut, "kept streaming after Stop"
            page.locator(".mm textarea").fill("after stop")
            page.keyboard.press("Enter")
            wait_bot_done(page, "earlier messages")
            print("stop ok: cut at", len(cut), "chars; next reply:", last_bot(page)[-40:])
        elif which == "llm":  # against llama_chat + a real llama-server: just needs a non-empty answer
            out = Path(sys.argv[3] if len(sys.argv) > 3 else ".")
            question = sys.argv[4] if len(sys.argv) > 4 else "Describe this image in one sentence."
            page.set_input_files(".mm input[type=file]", sys.argv[2])
            page.wait_for_selector(".mm .thumb img")
            page.locator(".mm textarea").fill(question)
            page.keyboard.press("Enter")
            page.wait_for_selector(".msg.user img")
            page.wait_for_function(
                """() => { const m = [...document.querySelectorAll('.msg.assistant')].pop();
                           return m && m.textContent.trim().length > 0 && !document.querySelector('.pending'); }""",
                timeout=300000)
            page.screenshot(path=str(out / "e2e_llm.png"), full_page=True)
            print("llm answer:", last_bot(page))
        elif which == "mock":
            out = Path(sys.argv[3] if len(sys.argv) > 3 else ".")
            first, second = chat(page, sys.argv[2], out, "images=[PNG", "second turn")
            assert "messages=1" in first and "この画像は何？" in first, first
            assert "messages=3" in second and "user/assistant/user" in second, second
        else:
            blocks(page, Path(sys.argv[2] if len(sys.argv) > 2 else "."))
        toasts = page.locator(".toast").all_text_contents()
        b.close()
    assert not errors, errors
    assert not toasts, toasts


if __name__ == "__main__":
    main()
