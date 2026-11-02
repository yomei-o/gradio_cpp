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
  `/gradio_api/file=`、queue（sse_v3：`queue/join`、`queue/data`、`cancel`）。heartbeat は
  `connect_heartbeat: false` にして使わせていない。
* queue の要点（gradio_client 2.7.2 を読んで合わせた）：join は `{data, fn_index, session_hash}` を受けて
  `{event_id}` を返す。data は session ごとに1本の SSE で、`data: {msg, event_id, ...}` を流す。
  generating は1回目が全体、2回目以降は出力ごとの diff（`[["append"|"replace", [], v]]`）。
  全イベントが終わったら `close_stream` を送って閉じる（sse_v3 の client は、自分からは閉じない）。
  `/config` の `version` は packaging で解析されるので、数値の版でなければならない（"5.0.0"）。
  4.29 より新しいと、client は `/gradio_api/cancel` を使う。

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

2026-11-02 v0.4（gradio_client 互換）
* `tests/client_test.py`：hello（位置引数・キーワード引数）、blocks（stream の差分の組み立て、cancel）、
  sepia（handle_file のアップロード、出力のダウンロード、gr::Error → AppError）、components、chat（/chat に画像付き）。
* `GC_PYTHON=... LLM=... tests/run_all.sh` で12本すべて PASS。

2026-11-02 v0.5（llama.cpp 直結）
* `examples/llama_local.cpp`：libllama と libmtmd を直接リンク（`-DGRADIO_LLAMA_CPP=...`。CMake は
  `LLAMA_BUILD_MTMD=ON`、tools/common/tests はオフ、static）。
* ビルド機（192.168.6.14）で、VS2022 x64、llama.cpp b12032 をビルド。PATH に cmake が無いので、vswhere で
  VS 付属の cmake を使う。SmolVLM2-500M で、gradio_client から画像付きの /chat を呼び、ストリーミングで答えが返った
  （「The background is blue.」など）。ブラウザでの確認は、同じ ChatInterface の llama_chat で済ませている。

2026-11-02 v0.6（KV キャッシュ再利用）
* llama_local：KV の中身を Unit（テキストのトークン1個、または画像1枚。画像は mtmd の chunk id
  ＝内容の SHA-256 と n_pos）の列で覚え、新しいプロンプトとの共通接頭辞を残して `llama_memory_seq_rm`。
  テキスト chunk の途中から一致しなくなった場合は、残りのトークンだけを自前の batch で decode する。
  生成した返事のトークンも列に積む。
* client_test.py の llm2（UI のイベント fn_index=0 に history を渡して2ターン）：2ターン目は
  reused 41 / 71、評価 107ms（1ターン目は 5580ms）。

2026-11-02 v0.7（部品その2）
* State / Progress / Examples(dataset) / Code / Dataframe / Label / Audio / Video。
* ハンドラは `Handler(args, CallContext{emit, progress})`。最後の引数が Yield なら generator、Progress なら進捗だけ。
  進捗は、/call では `event: progress`、queue では `msg: progress`（`progress_data` の配列）で送る。
* State：config で `skip_api: true`、info からも外す。ブラウザでは値をフロントが持って往復させる。
  gradio_client（queue）では、client が None を入れてくるので、Session::state から差し替え、終わったら書き戻す
  （同じセッションの並行イベントで競合しないよう、コピーで渡して後でマージする）。
* run_all：e2e 9本＋gradio_client 6本、計15本（LLM 本物を除く14本）PASS。

2026-11-02 v0.8（画像系）
* AnnotatedImage（`gradio_image.hpp` の AnnotatedImageValue。注釈1つずつ RGBA の重ね PNG を作る＝Gradio と同じ形式）、
  LinePlot / BarPlot（DataFrame を SVG で。Gradio の plot 形式とは互換でない：独自）、
  Image の Webcam、Audio の Microphone（MediaRecorder → webm をアップロード）。
* e2e は Chromium のフェイクデバイス（`--use-fake-device-for-media-stream`）で、カメラ撮影と録音まで通す。
* run_all：e2e 10本＋gradio_client 7本 PASS。

2026-11-02 v0.9（残りの部品）
* JPEG 書き出し（`to_jpeg`）。WebP はユーザー判断でやらない（読もうとすると明示的なエラー）。
* グラフを本家の `nativeplot`（値 `{columns, data, datatypes, mark}`）に作り直した。ハンドラが DataFrame を返すと
  `Blocks::postprocess` が変換する（generating 中も）。mark は props の `_mark`。
* Flagging（`Blocks::flagging`、Interface は既定で Flag ボタン）。本家 CSVLogger と同じ列（ラベル…, timestamp）。
* ImageEditor（背景＋透明レイヤ＋合成を、それぞれ PNG でアップロードして `{background, layers, composite}`）、
  Model3D（OBJ / STL を canvas 2D で描く。画家のアルゴリズム、法線で陰影）。
* `Blocks::on_raw`：型なしハンドラ（引数は json の配列）。
* run_all：e2e 11本＋gradio_client 8本 PASS。

2026-11-02 v0.10（認証）
* `LaunchOptions::auth`（`gr::auth_users(map)` は定数時間比較）。httplib の pre-routing handler で、
  `/`・`/login`・`/logout`・`/login_check` 以外は未ログインなら 401。本家と同じく、失敗した /login は 400、
  Cookie 名は `access-token-<起動ごとの乱数>`、logout はそのユーザーのトークンを全部消す。
* `/config` に `auth_required` と `auth_message`。フロントは /config が 401 ならログイン画面を出す。
* curl（401 / 400 / Cookie 付きで 200）、e2e login、gradio_client の `auth=`（無し・誤りは拒否）を確認。run_all 21本 PASS。

2026-11-02 v0.11（gr::Request）
* Yield / Progress / Request は「特殊引数」で、引数のどこにあってもよい。入力値は、特殊でない引数に順に割り当てる
  （`count_plain` で、引数 I より前にある普通の引数の数を数え、それを args の添字にする）。
* Request は /call では POST 時に、queue では join 時に作る（Cookie ヘッダは入れない）。

2026-11-02 v0.12（root_path、テーマ）
* `LaunchOptions::root_path`：`file_url()` と `/logout` のリダイレクトに前置、/config の `root`。
  フロントは元から相対 URL なので、ページを `/gradio/` で開けば動く。
* テスト用プロキシ `tests/prefix_proxy.py`（先頭のパスを剥がし、レスポンスを chunked で流すので SSE も通る）。
  Git Bash は `/gradio` のような引数を Windows パスに書き換えるので、`MSYS_NO_PATHCONV=1` が要る。
* `?__theme=dark|light`。run_all 24本 PASS（プロキシ経由の e2e と gradio_client を含む）。

2026-11-02 v0.13（ChatInterface の additional_inputs）
* ChatInterface を on_raw で作り直し：ユーザーの fn は make_handler で包み、`[message, history, extras...]` を渡す。
  これで Request / Progress / Yield も、ChatInterface の fn でそのまま使える。`/chat` は message + extras。
* llama_chat / llama_local に System prompt / Temperature / Max new tokens。llama_local はビルド機で、
  2ターン目 117ms（KV 再利用は維持）。run_all 25本 PASS。

2026-11-02 v0.14（大きなファイル）
* アップロードは ContentReader で、受信しながら `Cache::create()` のファイルへ書く。ダウンロードは
  set_content_provider で 64KB ずつ（httplib が Range を処理するので 206 も返る）。
* 200MB の往復で、サーバのメモリは 15.8MB → 17.0MB のまま。sha256 一致、Range 1000-1999 も一致。

## 次にやること

1. Linux（g++ / clang）でのビルドとテスト（Kaggle bridge かビルド機の WSL）。
2. JS クライアント（@gradio/client）からの呼び出し確認。
