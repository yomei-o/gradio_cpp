# RESUME — gradio_cpp の続きから

## 目標

* Gradio 相当を C++（cpp-httplib + nlohmann/json）で。ヘッダーオンリー。
* 最小構成から始めて、**本家 Gradio と API 互換**、**フル構成**を目指す。
* 直近のゴール：**llama.cpp と組み合わせて、LLM に画像とテキストを送り結果を表示するサンプル**。

## 決まったこと

* JSON は nlohmann/json。include はファイル名だけ（`"json.hpp"`, `"httplib.h"`）、場所は `-I` で渡す。
* ビルドは **Visual Studio 2022**（`-G "Visual Studio 17 2022" -A x64`）。mingw でビルドした exe は
  ウイルス対策ソフトに消される。ローカルの VS2022 は x64 ツールセットのみ（ARM64 は入っていない）。
* `<windows.h>` が `Yield()` マクロを定義するので、gradio.hpp で `#undef Yield` している。
* MSVC は、ラムダ内で関数ローカルの `constexpr` を暗黙にキャプチャできない（C3493）。
  そのため `make_handler` 内のものは `static constexpr` にしている。
* フロントは `frontend/index.html` が正本。`tools/embed_frontend.py` で 12KB ごとの raw 文字列に分けて埋め込む
  （MSVC の文字列リテラル長制限のため）。
* Gradio 互換の現状：`/config`、`/gradio_api/info`、`/gradio_api/call/*`（SSE）、`/gradio_api/upload`、
  `/gradio_api/file=` は実装済み。`gradio_client` が使う `/gradio_api/queue/join` と
  `/gradio_api/queue/data`（sse_v3）は未実装。

## 確認済み（2026-11-02）

* curl：count（複数出力・JSON）、stream（generating×N → complete）、引数不足 → error、
  許可外ファイル → 403 または error。
* Playwright（`tests/e2e.py`）：sepia（アップロード → Submit → 出力画像）、blocks（stream と count）。どちらも通過。

## 次にやること

1. **Chatbot コンポーネント**（メッセージ形式 `[{role, content}]`、画像付きメッセージ、ストリーミング）。
2. **MultimodalTextbox**（テキスト＋画像の添付）。
3. **llama.cpp サンプル**：まずは `llama-server`（OpenAI 互換 `/v1/chat/completions`、`stream: true`、
   画像は `image_url` に data URI）へ httplib::Client で投げる形。その後、libmtmd を直接リンクする版も検討。
4. `gr.update` 相当（visible や value の部分更新）、Tabs / Accordion、Dropdown / Radio。
5. `gradio_client` 互換（queue/join + sse_v3）。
