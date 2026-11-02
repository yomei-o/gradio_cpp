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

## 確認済み

2026-11-02 v0.1
* curl：count（複数出力・JSON）、stream（generating×N → complete）、引数不足 → error、
  許可外ファイル → 403 または error。
* Playwright（`tests/e2e.py`）：sepia（アップロード → Submit → 出力画像）、blocks（stream と count）。どちらも通過。

2026-11-02 v0.2（Chat）
* Chatbot / MultimodalTextbox / ChatInterface / then / cancel、`gradio_openai.hpp`（OpenAI 互換クライアント）。
* e2e：echo（画像＋2ターン目の履歴）、stop（途中で止まり、その後も会話できる）、mock（`tests/mock_openai.py`。
  画像が PNG 1280x800 として届き、2ターン目の roles は user/assistant/user）、すべて通過。
* 本物：llama.cpp b12032 の win-cpu-arm64 版 llama-server と SmolVLM2-500M-Video-Instruct-Q8_0（＋mmproj）で、
  画像を説明する返事が返った。バイナリとモデルは `C:\prog\llama_cpp`（リポの外）に置いてある。

2026-11-02 v0.3（部品）
* Dropdown / Radio / CheckboxGroup / Gallery / HTML、Tabs / Accordion、`gr::update`。
  フロントでは、visible と value はその場で書き換え、それ以外の props は描き直す。イベントの購読はコンポーネント id ごとに
  持っているので、描き直しても消えない。
* `tests/run_all.sh` で7本（sepia, blocks, components, echo, stop, mock, llm）すべて PASS。

## 次にやること

1. `gradio_client` 互換（`/gradio_api/queue/join` と `/gradio_api/queue/data` の sse_v3、`/gradio_api/info`
   の細部）。Python の gradio_client から本当に呼んで確かめる。
2. libmtmd を直接リンクする llama.cpp サンプル（サーバを別に立てない版）。
