# gradio_cpp

[Gradio](https://www.gradio.app/) と同じ感覚で、C++ の関数にブラウザ UI を付けるヘッダーオンリーライブラリ。
[cpp-httplib](https://github.com/yhirose/cpp-httplib) と [nlohmann/json](https://github.com/nlohmann/json) だけで動く。

```cpp
#include "gradio.hpp"

int main() {
    gr::Interface demo(
        [](const std::string& name, double intensity) {
            return "Hello, " + name + std::string((size_t)intensity, '!');
        },
        {gr::Textbox("name"), gr::Slider("intensity", 1, 10, 3)},
        {gr::Textbox("greeting")});
    demo.launch();   // * Running on local URL:  http://127.0.0.1:7860
}
```

## LLM チャット（llama.cpp）

```sh
llama-server -m SmolVLM2-500M-Video-Instruct-Q8_0.gguf --mmproj mmproj-SmolVLM2-500M-Video-Instruct-Q8_0.gguf --port 8080
build/Release/llama_chat.exe --server http://127.0.0.1:8080
```

画像を「+」で添付（ドラッグ＆ドロップや貼り付けでも可）して質問すると、返事がストリーミングで表示される。
中身は `gr::ChatInterface` と `gr::openai::Client`（OpenAI 互換 API のクライアント。画像は data URI で送る）で、
本体は次の数行：

```cpp
gr::openai::Client llm("http://127.0.0.1:8080");
gr::ChatInterface chat(
    [&](const gr::MultimodalValue& message, const gr::History& history, const gr::Yield& yield) {
        llm.chat(gr::openai::to_messages(history, message),
                 [&](const std::string& so_far) { yield(so_far); });
    },
    "llama.cpp chat");
chat.launch();
```

Ollama や vLLM など、OpenAI 互換のサーバならそのまま使える（`--server`、`--model`）。

## できること

| | |
|---|---|
| `gr::Interface` | 入力・出力・Clear/Submit を自動配置 |
| `gr::Blocks` | `row()` / `column()` / `tabs()`+`tab()` / `accordion()` で自由レイアウト、`click` / `submit` / `change` イベント |
| `gr::ChatInterface` | `fn(message, history[, yield])` だけでチャット画面。Stop ボタン、`/chat` API 付き |
| コンポーネント | Textbox, Number, Slider, Checkbox, Image, File, Markdown, JSON, Button, ClearButton, Chatbot（messages 形式）, MultimodalTextbox, Dropdown, Radio, CheckboxGroup, Gallery, HTML |
| `gr::update()` | 値の代わりに返すと、そのコンポーネントの props を変える（`visible`、`choices`、`label` など） |
| イベント | `then()`（成功後に続けて実行）、`cancel()`（実行中のイベントを止める） |
| 型付きハンドラ | 引数の型（`std::string`, `double`, `int`, `bool`, `gr::FileData`, `gr::json`）に自動変換。複数出力は `std::tuple` で返す |
| ストリーミング | 最後の引数を `const gr::Yield&` にすると generator になり、`yield(...)` のたびにブラウザへ送る |
| 画像 | `gradio_image.hpp`（stb_image）で `gr::load_pixels()` / `gr::to_png()` |
| エラー | `throw gr::Error("...")` でブラウザにトースト表示 |
| 同時実行 | イベントごとに `concurrency_limit`（既定 1、Gradio と同じ） |

## HTTP API（Gradio と同じパス）

| | |
|---|---|
| `GET /config` | 画面構成（components / dependencies / layout） |
| `GET /gradio_api/info` | エンドポイントと引数の説明 |
| `POST /gradio_api/call/<api_name>` | `{"data": [...]}` → `{"event_id": ...}` |
| `GET /gradio_api/call/<api_name>/<event_id>` | SSE で `generating` / `complete` / `error` |
| `POST /gradio_api/queue/join` + `GET /gradio_api/queue/data` | gradio_client が使う sse_v3 の queue プロトコル |
| `POST /gradio_api/cancel` | queue で実行中のイベントを止める |
| `POST /gradio_api/upload` | multipart の `files` → サーバ側パスの配列 |
| `GET /gradio_api/file=<path>` | アップロード／出力ファイルだけを返す（それ以外は 403） |

Gradio のドキュメントにある curl の手順がそのまま使える：

```sh
curl -X POST http://127.0.0.1:7860/gradio_api/call/predict -s -H "Content-Type: application/json" \
     -d '{"data": ["World", 3]}' | awk -F'"' '{ print $4}' \
  | read EVENT_ID; curl -N http://127.0.0.1:7860/gradio_api/call/predict/$EVENT_ID
```

### Python の gradio_client から

本家と同じ queue プロトコル（sse_v3：`/gradio_api/queue/join`、`/gradio_api/queue/data`、`/gradio_api/cancel`）も
話すので、公式の `gradio_client` からそのまま呼べる：

```python
from gradio_client import Client, handle_file
c = Client("http://127.0.0.1:7860/")
c.predict("World", 3, api_name="/predict")                     # 位置引数でもキーワード引数でも
c.predict(handle_file("cat.png"), 50, api_name="/predict")     # ファイルは自動でアップロード、結果は自動でダウンロード
for partial in c.submit("hi", 4, api_name="/stream"): ...      # ストリーミング（差分で届き、client 側で組み立てられる）
c.predict({"text": "これは何？", "files": [handle_file("cat.png")]}, api_name="/chat")   # ChatInterface
```

gradio_client 2.7.2 で確認済み。`/config` の `version` は、話しているプロトコルの水準として "5.0.0" を名乗る
（gradio_cpp 自身の版は `gradio_cpp_version`）。

## ビルド

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build/Release/hello.exe
```

`include/` と `third_party/` を include パスに足し、Windows では `ws2_32` をリンクする。
MSVC では `/utf-8 /bigobj` を付ける（CMake の `gradio_cpp` ターゲットが付ける）。

フロントエンドは `frontend/index.html` を編集してから `python tools/embed_frontend.py` で
`include/gradio_frontend.hpp` を作り直す。

## サンプル

* `examples/hello.cpp` — Gradio のクイックスタート
* `examples/sepia.cpp` — 画像を入力してセピア化
* `examples/blocks.cpp` — Blocks レイアウト、複数出力、ストリーミング
* `examples/components.cpp` — Tabs / Accordion / Dropdown / Radio / CheckboxGroup / Gallery / `gr::update`
* `examples/chat_echo.cpp` — LLM なしの ChatInterface（受け取ったものを返す）
* `examples/llama_chat.cpp` — llama-server につなぐ画像付きチャット

## テスト

全部まとめて（各サンプルを起動 → Playwright で操作 → 停止）：

```sh
bash tests/run_all.sh some.png                                  # build/Release の exe を使う
LLM=http://127.0.0.1:8081 bash tests/run_all.sh some.png        # 本物の llama-server も
GC_PYTHON=venv/Scripts/python bash tests/run_all.sh some.png    # gradio_client のテストも（tests/client_test.py）
```

個別に、サンプルを起動してから：

```sh
python tests/e2e.py sepia some.png
python tests/e2e.py blocks
python tests/e2e.py components
python tests/e2e.py echo some.png      # chat_echo を起動しておく
python tests/e2e.py stop               # chat_echo：Stop で止まるか
python tests/mock_openai.py 8080 &     # 画像の中身まで返すモック LLM
python tests/e2e.py mock some.png      # llama_chat --server http://127.0.0.1:8080
python tests/e2e.py llm some.png       # 本物の llama-server につないだ llama_chat
```

## 同梱ライブラリ

`third_party/` に cpp-httplib 0.18.3（MIT）、nlohmann/json 3.11.3（MIT）、stb_image / stb_image_write（public domain / MIT）。
