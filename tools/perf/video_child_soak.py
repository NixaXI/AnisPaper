#!/usr/bin/env python3
"""Bounded PSS soak for the integrated isolated video worker."""

from __future__ import annotations

import argparse
import csv
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
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
    parser.add_argument("--preview", required=True)
    parser.add_argument("--seconds", type=float, default=900.0)
    parser.add_argument("--interval", type=float, default=30.0)
    parser.add_argument("--fps", type=int, default=60)
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.seconds <= 0 or args.interval <= 0:
        raise SystemExit("seconds and interval must be positive")

    with tempfile.TemporaryDirectory(prefix="anispaper-video-child-") as temp:
        binary = Path(temp) / "anis-paperd"
        shutil.copy2(args.binary, binary)
        binary.chmod(0o700)
        command = [str(binary), "--renderer-child", "--type", "video",
                   "--file", args.file, "--preview", args.preview,
                   "--width", str(args.width), "--height", str(args.height),
                   "--fps", str(args.fps), "--volume", "0", "--speed", "1",
                   "--loop", "1"]
        env = os.environ.copy()
        env.setdefault("QT_QPA_PLATFORM", "xcb")
        env.setdefault("DISPLAY", ":0")
        child = subprocess.Popen(command, stdin=subprocess.PIPE,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 env=env)
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
                try:
                    child.stdin.write(b'{"command":"stop"}\n')
                    child.stdin.flush()
                except (BrokenPipeError, OSError):
                    pass
                try:
                    child.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    child.send_signal(signal.SIGTERM)
                    try:
                        child.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait(timeout=3)
        return_code = child.returncode

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=("timestamp", "pid", *FIELDS))
        writer.writeheader()
        writer.writerows(samples)
    print(f"child_rc={return_code} samples={len(samples)} output={args.output}")
    return 0 if return_code == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())

