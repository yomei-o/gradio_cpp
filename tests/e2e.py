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
        else:
            blocks(page, Path(sys.argv[2] if len(sys.argv) > 2 else "."))
        toasts = page.locator(".toast").all_text_contents()
        b.close()
    assert not errors, errors
    assert not toasts, toasts


if __name__ == "__main__":
    main()
