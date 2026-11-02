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
        # fake camera / microphone for the webcam and recording tests
        b = p.chromium.launch(args=["--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream"])
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
        elif which == "components":
            out = Path(sys.argv[2] if len(sys.argv) > 2 else ".")
            # radio -> update(choices, label, value) on the dropdown
            page.get_by_label("vegetable").check()
            page.wait_for_function("[...document.querySelectorAll('select option')].some(o => o.textContent === 'leek')")
            page.locator("select").select_option(label="leek")
            page.get_by_label("pepper").check()
            page.get_by_role("button", name="Order").click()
            page.wait_for_function("[...document.querySelectorAll('input[type=text]')].some(i => i.value.includes('leek'))")
            summary = [i.input_value() for i in page.locator("input[type=text]").all() if "leek" in i.input_value()][0]
            assert summary == "vegetable: leek +salt +pepper", summary
            # gallery
            page.get_by_role("button", name="Gallery").click()
            page.get_by_role("button", name="Make swatches").click()
            page.wait_for_function("document.querySelectorAll('.gallery img').length === 4")
            page.wait_for_function("[...document.querySelectorAll('.gallery img')].every(i => i.complete && i.naturalWidth === 96)")
            page.screenshot(path=str(out / "e2e_gallery.png"), full_page=True)
            # visibility via update
            page.get_by_role("button", name="Visibility").click()
            secret = page.locator("input[type=text]").filter(has_text="")
            assert not page.get_by_text("secret", exact=True).is_visible()
            page.get_by_label("show the secret box").check()
            page.get_by_text("secret", exact=True).wait_for(state="visible")
            page.locator("summary", has_text="More").click()
            page.get_by_text("raw HTML").wait_for(state="visible")
            page.screenshot(path=str(out / "e2e_components.png"), full_page=True)
            print("components ok:", summary)
        elif which == "hello":  # Interface + Examples: clicking an example fills the inputs
            page.locator("table.ds tbody tr").nth(1).click()
            assert page.locator("input[type=text]").first.input_value() == "C++"
            page.get_by_role("button", name="Submit").click()
            page.wait_for_function("[...document.querySelectorAll('input[type=text]')].some(i => i.value === 'Hello, C++!!!!!!!')")
            print("hello ok: example row filled the inputs")
        elif which == "extras":
            out = Path(sys.argv[2] if len(sys.argv) > 2 else ".")
            add = page.get_by_role("button", name="Add one")
            for _ in range(3):
                add.click()
                page.wait_for_timeout(300)
            page.wait_for_function("[...document.querySelectorAll('input[type=text]')].some(i => i.value === '3')")
            page.get_by_role("button", name="Progress").click()
            page.get_by_role("button", name="Run slow job").click()
            page.wait_for_selector(".prog")
            page.wait_for_function("(document.querySelector('.timer')?.textContent || '').includes('step')")
            prog_text = page.locator(".timer").first.text_content()
            page.wait_for_function("[...document.querySelectorAll('input[type=text]')].some(i => i.value.startsWith('done 8'))")
            page.get_by_role("button", name="Text tools").click()
            page.locator("table.ds tbody tr").first.click()  # Python example
            page.get_by_role("button", name="Analyze", exact=True).click()
            page.wait_for_function("document.querySelector('.lbl-top')?.textContent === 'Python'")
            page.get_by_role("button", name="Sum the counts").click()
            page.wait_for_function("[...document.querySelectorAll('input[type=number]')].some(i => Number(i.value) > 10)")
            page.screenshot(path=str(out / "e2e_extras.png"), full_page=True)
            print("extras ok: state=3, progress shown as", repr(prog_text), ", label=Python")
        elif which == "vision":  # webcam capture -> AnnotatedImage + plots
            out = Path(sys.argv[2] if len(sys.argv) > 2 else ".")
            page.get_by_role("button", name="Webcam").click()
            page.wait_for_function("document.querySelector('.cam video')?.videoWidth > 0", timeout=15000)
            page.get_by_role("button", name="Capture").click()
            page.wait_for_selector(".drop img")
            page.wait_for_timeout(500)
            page.get_by_role("button", name="Submit").click()
            page.wait_for_function("document.querySelectorAll('.ann img.ov').length === 5", timeout=15000)
            page.wait_for_function("document.querySelector('svg.plot polyline') && document.querySelectorAll('svg.plot rect').length === 4")
            legend = page.locator(".legend span").all_text_contents()
            page.screenshot(path=str(out / "e2e_vision.png"), full_page=True)
            assert legend == ["brightest (2)", "bright (2)", "darkest (1)"], legend
            print("vision ok: webcam frame annotated; legend", legend)
        elif which == "mic":  # extras' Audio tab: record from the (fake) microphone, get it echoed back
            page.get_by_role("button", name="Audio", exact=True).click()
            page.get_by_role("button", name="Microphone").first.click()
            page.get_by_role("button", name="Record").click()
            page.wait_for_timeout(1200)
            page.get_by_role("button", name="Stop").click()
            page.wait_for_function("[...document.querySelectorAll('input[type=text]')].some(i => i.value.includes('recording.'))", timeout=15000)
            info = [i.input_value() for i in page.locator("input[type=text]").all() if "recording." in i.input_value()][0]
            assert page.locator(".drop audio").count() == 2, page.locator(".drop audio").count()
            print("mic ok:", info)
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
