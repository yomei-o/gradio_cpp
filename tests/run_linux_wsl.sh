#!/usr/bin/env bash
# Build the examples inside WSL (Linux g++) and run the same tests against the Linux binaries.
# Run from Git Bash on Windows:   bash tests/run_linux_wsl.sh <some.png> [distro]
#   GC_PYTHON=venv/Scripts/python bash tests/run_linux_wsl.sh x.png
set -u
PNG=${1:?usage: run_linux_wsl.sh some.png [distro]}
DISTRO=${2:-Ubuntu-20.04}
OUT=build
export PYTHONIOENCODING=utf-8
SRC=$(wsl -d "$DISTRO" -- wslpath -a "$(cygpath -w "$(pwd)")" | tr -d '\r')

wsl -d "$DISTRO" -- bash -lc "rm -rf ~/grcpp && mkdir -p ~/grcpp && cd '$SRC' && tar cf - --exclude=build --exclude=.git . | (cd ~/grcpp && tar xf -) && cd ~/grcpp && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null && nice -n 10 cmake --build build -j 2 2>&1 | grep -E 'error|warning'" && echo "(compiler output above)"

fail=0
run() {  # run <exe> -- <command...>
  local exe=$1; shift 2
  wsl -d "$DISTRO" -- bash -lc "cd ~/grcpp && exec build/$exe" > "$OUT/linux_$exe.log" 2>&1 &
  local pid=$!
  for _ in $(seq 1 50); do curl -s -o /dev/null http://127.0.0.1:7860/config && break; sleep 0.2; done
  if timeout 600 "$@"; then echo "PASS linux $exe: ${*:2:2}"; else echo "FAIL linux $exe: ${*:2:2}"; fail=1; fi
  wsl -d "$DISTRO" -- pkill -f "build/$exe" 2>/dev/null
  kill $pid 2>/dev/null; wait $pid 2>/dev/null
}

E2E="python tests/e2e.py"
run sepia -- $E2E sepia "$PNG" "$OUT"
run blocks -- $E2E blocks "$OUT"
run components -- $E2E components "$OUT"
run extras -- $E2E extras "$OUT"
run vision -- $E2E vision "$OUT"
run studio -- $E2E studio "$OUT"
run chat_echo -- $E2E echo "$PNG" "$OUT"
run chat_echo -- $E2E stop
run login -- $E2E login
if [ -n "${GC_PYTHON:-}" ]; then
  GC="$GC_PYTHON tests/client_test.py"
  run hello -- $GC hello
  run blocks -- $GC blocks
  run sepia -- $GC sepia "$PNG"
  run extras -- $GC extras
  run studio -- $GC studio "$PNG"
  run chat_echo -- $GC chat "$PNG"
  run login -- $GC login
fi
exit $fail
