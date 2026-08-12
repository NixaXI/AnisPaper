#!/usr/bin/env python3
"""Exercise particle scenes whose material reference is absent or invalid.

Usage (requires an active desktop session):
  tests/particle_missing_material_smoke.py --binary /path/to/anis-paper-scene-engine --root /path/to/workshop-item
"""

from __future__ import annotations

import argparse
import select
import subprocess
import sys
import time


def exercise(binary: str, root: str, timeout: float) -> bool:
    command = [binary, "--file", root, "--width", "640", "--height", "360", "--fps", "5", "--scaling", "fill"]
    child = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    assert child.stdin is not None
    assert child.stdout is not None
    output: list[str] = []
    ready = False
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and child.poll() is None:
        readable, _, _ = select.select([child.stdout], [], [], 0.2)
        if not readable:
            continue
        line = child.stdout.readline()
        if not line:
            continue
        output.append(line)
        if '"event":"ready"' in line:
            ready = True
            child.stdin.write('{"event":"stop"}\n')
            child.stdin.flush()
            break

    try:
        child.wait(timeout=5)
    except subprocess.TimeoutExpired:
        child.terminate()
        child.wait(timeout=5)

    transcript = "".join(output)
    if ready and child.returncode == 0:
        print(f"particle_missing_material_smoke: PASS ({root})")
        return True
    print(transcript, file=sys.stderr, end="")
    print(
        f"particle_missing_material_smoke: FAIL root={root} ready={ready} exit={child.returncode}",
        file=sys.stderr,
    )
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--root", required=True, action="append")
    parser.add_argument("--timeout", type=float, default=20.0)
    args = parser.parse_args()
    return 0 if all(exercise(args.binary, root, args.timeout) for root in args.root) else 1


if __name__ == "__main__":
    raise SystemExit(main())
