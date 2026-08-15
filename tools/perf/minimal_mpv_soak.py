#!/usr/bin/env python3
"""Bounded PSS sampler for tools/perf/mpv_render_repro.cpp."""

from __future__ import annotations

import argparse
import csv
import os
from pathlib import Path
import signal
import subprocess
import time


FIELDS = ("Rss", "Pss", "Private_Clean", "Private_Dirty", "Shared_Clean",
          "Shared_Dirty", "Anonymous", "Swap")


def rollup(pid: int) -> dict[str, int]:
    values = {key: 0 for key in FIELDS}
    for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines():
        key, _, rest = line.partition(":")
        if key in values:
            values[key] = int(rest.strip().split()[0])
    return values


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--file", required=True)
    parser.add_argument("--seconds", type=float, default=900.0)
    parser.add_argument("--interval", type=float, default=30.0)
    parser.add_argument("--fps", type=int, default=60)
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--skip-readback", action="store_true")
    parser.add_argument("--report-swap", action="store_true")
    parser.add_argument("--update-driven", action="store_true")
    parser.add_argument("--loop", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.seconds <= 0 or args.interval <= 0:
        raise SystemExit("seconds and interval must be positive")

    child_seconds = max(1, int(round(args.seconds)))
    command = [args.binary, "--file", args.file, "--seconds", str(child_seconds),
               "--fps", str(args.fps), "--width", str(args.width),
               "--height", str(args.height)]
    if args.skip_readback:
        command.append("--skip-readback")
    if args.report_swap:
        command.append("--report-swap")
    if args.update_driven:
        command.append("--update-driven")
    if args.loop:
        command.append("--loop")
    env = os.environ.copy()
    env.setdefault("QT_QPA_PLATFORM", "xcb")
    env.setdefault("DISPLAY", ":0")
    child = subprocess.Popen(command, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True, env=env)
    samples: list[dict[str, object]] = []
    started = time.monotonic()
    try:
        while time.monotonic() - started < args.seconds:
            if child.poll() is not None:
                break
            try:
                values = rollup(child.pid)
            except (FileNotFoundError, ProcessLookupError):
                break
            values["timestamp"] = time.time()
            values["pid"] = child.pid
            samples.append(values)
            time.sleep(args.interval)
    finally:
        if child.poll() is None:
            child.send_signal(signal.SIGTERM)
            try:
                child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=3)
        stdout, stderr = child.communicate()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=("timestamp", "pid", *FIELDS))
        writer.writeheader()
        writer.writerows(samples)
    print(f"child_rc={child.returncode} samples={len(samples)} output={args.output}")
    print(stdout.strip())
    if stderr.strip():
        print(stderr.strip())
    return 0 if child.returncode in (0, -signal.SIGTERM) else 1


if __name__ == "__main__":
    raise SystemExit(main())
