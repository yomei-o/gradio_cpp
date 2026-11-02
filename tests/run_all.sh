#!/usr/bin/env bash
# Run every test: starts each example, runs the browser e2e (Playwright) and, when
# GC_PYTHON points at a Python with gradio_client installed, the gradio_client tests.
#   tests/run_all.sh <some.png> [exe_dir] [out_dir]
#   GC_PYTHON=venv/Scripts/python LLM=http://127.0.0.1:8081 tests/run_all.sh x.png
#   GRADIO_CLIENT_DIR=<dir with node_modules/@gradio/client> also runs the JS client tests
set -u
PNG=${1:?usage: run_all.sh some.png [exe_dir] [out_dir]}
BIN=${2:-build/Release}
OUT=${3:-build}
export PYTHONIOENCODING=utf-8
fail=0

run() {  # run <exe> <exe args...> -- <command...>
  local exe=$1; shift
  local args=()
  while [ "$1" != "--" ]; do args+=("$1"); shift; done; shift
  "$BIN/$exe.exe" "${args[@]}" > "$OUT/$exe.log" 2>&1 &
  local pid=$!
  for _ in $(seq 1 50); do curl -s -o /dev/null http://127.0.0.1:7860/config && break; sleep 0.2; done
  if timeout 600 "$@"; then echo "PASS $exe: ${*:2:2}"; else echo "FAIL $exe: ${*:2:2}"; fail=1; fi
  kill $pid 2>/dev/null; wait $pid 2>/dev/null
}

E2E="python tests/e2e.py"
run sepia -- $E2E sepia "$PNG" "$OUT"
run blocks -- $E2E blocks "$OUT"
run components -- $E2E components "$OUT"
run hello -- $E2E hello
run extras -- $E2E extras "$OUT"
run extras -- $E2E mic
run vision -- $E2E vision "$OUT"
run studio -- $E2E studio "$OUT"
run login -- $E2E login
run components -- $E2E theme
run live -- $E2E live
run extras -- $E2E timer

# behind a reverse proxy under /gradio (LaunchOptions::root_path)
MSYS_NO_PATHCONV=1 python tests/prefix_proxy.py 8096 /gradio > "$OUT/proxy.log" 2>&1 &
PROXY=$!
PNGW=$(cygpath -m "$PNG" 2>/dev/null || echo "$PNG")
export MSYS_NO_PATHCONV=1
GRADIO_URL=http://127.0.0.1:8096/gradio/ run sepia --root-path /gradio -- $E2E sepia "$PNGW" "$OUT"
if [ -n "${GC_PYTHON:-}" ]; then
  GRADIO_URL=http://127.0.0.1:8096/gradio/ run sepia --root-path /gradio -- $GC_PYTHON tests/client_test.py sepia "$PNGW"
fi
unset MSYS_NO_PATHCONV
kill $PROXY 2>/dev/null
run chat_echo -- $E2E echo "$PNG" "$OUT"
run chat_echo -- $E2E stop

python tests/mock_openai.py 8090 > "$OUT/mock.log" 2>&1 &
MOCK=$!
run llama_chat --server http://127.0.0.1:8090 -- $E2E mock "$PNG" "$OUT"
if [ -n "${GC_PYTHON:-}" ]; then run llama_chat --server http://127.0.0.1:8090 -- $GC_PYTHON tests/client_test.py mockapi; fi
kill $MOCK 2>/dev/null

if [ -n "${LLM:-}" ]; then run llama_chat --server "$LLM" -- $E2E llm "$PNG" "$OUT"; fi

if [ -n "${GC_PYTHON:-}" ]; then
  GC="$GC_PYTHON tests/client_test.py"
  run hello -- $GC hello
  run blocks -- $GC blocks
  run sepia -- $GC sepia "$PNG"
  run components -- $GC components
  run extras -- $GC extras
  run vision -- $GC vision "$PNG"
  run studio -- $GC studio "$PNG"
  run login -- $GC login
  run chat_echo -- $GC chat "$PNG"
fi
if [ -n "${GRADIO_CLIENT_DIR:-}" ]; then  # a folder where `npm install @gradio/client` was run
  JS="node tests/js_client_test.mjs"
  PNGW=$(cygpath -m "$PNG" 2>/dev/null || echo "$PNG")
  run hello -- $JS hello
  run blocks -- $JS blocks
  run sepia -- $JS sepia "$PNGW"
  run login -- $JS login
fi
exit $fail
