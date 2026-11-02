#!/usr/bin/env bash
# Run every browser e2e test: starts each example, runs tests/e2e.py, stops it.
#   tests/run_all.sh <some.png> [exe_dir] [out_dir]
# LLM=http://127.0.0.1:8081 tests/run_all.sh x.png   also runs the real llama-server test.
set -u
PNG=${1:?usage: run_all.sh some.png [exe_dir] [out_dir]}
BIN=${2:-build/Release}
OUT=${3:-build}
export PYTHONIOENCODING=utf-8
fail=0

run() {  # run <exe> <exe args...> -- <e2e args...>
  local exe=$1; shift
  local args=()
  while [ "$1" != "--" ]; do args+=("$1"); shift; done; shift
  "$BIN/$exe.exe" "${args[@]}" > "$OUT/$exe.log" 2>&1 &
  local pid=$!
  for _ in $(seq 1 50); do curl -s -o /dev/null http://127.0.0.1:7860/config && break; sleep 0.2; done
  if python tests/e2e.py "$@"; then echo "PASS $exe $1"; else echo "FAIL $exe $1"; fail=1; fi
  kill $pid 2>/dev/null; wait $pid 2>/dev/null
}

run sepia -- sepia "$PNG" "$OUT"
run blocks -- blocks "$OUT"
run components -- components "$OUT"
run chat_echo -- echo "$PNG" "$OUT"
run chat_echo -- stop

python tests/mock_openai.py 8090 > "$OUT/mock.log" 2>&1 &
MOCK=$!
run llama_chat --server http://127.0.0.1:8090 -- mock "$PNG" "$OUT"
kill $MOCK 2>/dev/null

if [ -n "${LLM:-}" ]; then run llama_chat --server "$LLM" -- llm "$PNG" "$OUT"; fi
exit $fail
