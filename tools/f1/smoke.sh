#!/bin/sh
set -eu
daemon="$1"
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME"
run="${TMPDIR:-/tmp}/anispaper-f1-smoke-$$"
cp "$daemon" "$run"
chmod 700 "$run"
trap 'rm -f "$run"' EXIT
"$run" >/dev/null 2>&1 &
pid=$!
trap 'kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; rm -f "$run"' EXIT
sleep .1
kill -0 "$pid"
kill "$pid"
wait "$pid" || true
trap - EXIT
