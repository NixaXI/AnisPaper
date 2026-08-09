#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
binary="$root_dir/build/f0/anispaper-layer-shell-probe"
timeout_ms=8000
output_name=""
capture_tool=""
artifact_dir="$root_dir/artifacts/probe"

usage() {
  echo "Usage: $0 [--binary PATH] [--timeout-ms N] [--output NAME] [--capture grim|spectacle] [--artifacts DIR]" >&2
}

while (($#)); do
  case "$1" in
    --binary|--timeout-ms|--output|--capture|--artifacts)
      (($# >= 2)) || { usage; exit 64; }
      case "$1" in
        --binary) binary=$2 ;;
        --timeout-ms) timeout_ms=$2 ;;
        --output) output_name=$2 ;;
        --capture) capture_tool=$2 ;;
        --artifacts) artifact_dir=$2 ;;
      esac
      shift 2 ;;
    --help) usage; exit 0 ;;
    *) usage; exit 64 ;;
  esac
done

mkdir -p "$artifact_dir"
python3 "$root_dir/tools/probe/inventory.py" > "$artifact_dir/inventory.txt"

arguments=(--timeout-ms "$timeout_ms")
if [[ -n "$output_name" ]]; then arguments+=(--output "$output_name"); fi
"$binary" "${arguments[@]}" > "$artifact_dir/layer-shell.txt" 2>&1 &
probe_pid=$!

# The capture deliberately happens while the finite probe is still alive.
sleep 2
capture_status=0
case "$capture_tool" in
  "") ;;
  grim)
    if ! command -v grim >/dev/null; then
      echo "grim not found" >&2
      capture_status=127
    elif grim "$artifact_dir/desktop.png"; then
      :
    else
      capture_status=$?
    fi ;;
  spectacle)
    if ! command -v spectacle >/dev/null; then
      echo "spectacle not found" >&2
      capture_status=127
    elif spectacle --background --nonotify --output "$artifact_dir/desktop.png"; then
      :
    else
      capture_status=$?
    fi ;;
  *)
    echo "Unsupported capture tool: $capture_tool" >&2
    capture_status=64 ;;
esac

probe_status=0
if wait "$probe_pid"; then
  :
else
  probe_status=$?
fi
echo "Evidence written to $artifact_dir"
if ((probe_status != 0)); then exit "$probe_status"; fi
exit "$capture_status"
