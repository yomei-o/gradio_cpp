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

## できること（v0.1）

| | |
|---|---|
| `gr::Interface` | 入力・出力・Clear/Submit を自動配置 |
| `gr::Blocks` | `row()` / `column()` で自由レイアウト、`click` / `submit` / `change` イベント |
| コンポーネント | Textbox, Number, Slider, Checkbox, Image, File, Markdown, JSON, Button, ClearButton |
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
| `POST /gradio_api/upload` | multipart の `files` → サーバ側パスの配列 |
| `GET /gradio_api/file=<path>` | アップロード／出力ファイルだけを返す（それ以外は 403） |

Gradio のドキュメントにある curl の手順がそのまま使える：

```sh
curl -X POST http://127.0.0.1:7860/gradio_api/call/predict -s -H "Content-Type: application/json" \
     -d '{"data": ["World", 3]}' | awk -F'"' '{ print $4}' \
  | read EVENT_ID; curl -N http://127.0.0.1:7860/gradio_api/call/predict/$EVENT_ID
```

Python の `gradio_client` から呼ぶための queue プロトコル（`/gradio_api/queue/join` ほか）はまだない。

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

## テスト

サンプルを起動してから Playwright でブラウザ操作を確かめる：

```sh
python tests/e2e.py sepia some.png
python tests/e2e.py blocks
```

## 同梱ライブラリ

`third_party/` に cpp-httplib 0.18.3（MIT）、nlohmann/json 3.11.3（MIT）、stb_image / stb_image_write（public domain / MIT）。
