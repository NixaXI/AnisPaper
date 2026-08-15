#!/usr/bin/env python3
"""Measure renderer memory across repeated real-output replacements.

The test deliberately uses the two physical outputs and restores their
original wallpapers in a finally block.  It records PSS and SHM after every
stable replacement; it does not change settings or restart Plasma.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import socket
import subprocess
import time
from typing import Any


class Rpc:
    def __init__(self, path: pathlib.Path) -> None:
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(5.0)
        self.sock.connect(str(path))
        self.buf = b""
        self.ident = 1

    def close(self) -> None:
        self.sock.close()

    def call(self, method: str, params: dict[str, Any] | None = None) -> Any:
        ident = self.ident
        self.ident += 1
        request = {"jsonrpc": "2.0", "id": ident, "method": method,
                   "params": params or {}}
        self.sock.sendall(json.dumps(request, separators=(",", ":")).encode() + b"\n")
        deadline = time.monotonic() + 8.0
        while time.monotonic() < deadline:
            while b"\n" not in self.buf:
                self.sock.settimeout(max(0.05, deadline - time.monotonic()))
                chunk = self.sock.recv(65536)
                if not chunk:
                    raise RuntimeError("daemon closed RPC socket")
                self.buf += chunk
            line, self.buf = self.buf.split(b"\n", 1)
            message = json.loads(line)
            if message.get("id") != ident:
                continue
            if "error" in message:
                raise RuntimeError(f"{method}: {message['error']}")
            return message.get("result")
        raise TimeoutError(f"RPC timeout: {method}")


def processes() -> dict[str, tuple[int, str]]:
    result: dict[str, tuple[int, str]] = {}
    for line in subprocess.check_output(["ps", "-eo", "pid=,args="], text=True).splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            pid_text, command = line.split(None, 1)
            pid = int(pid_text)
        except (ValueError, IndexError):
            continue
        if command.endswith("/anis-paperd") or command == "anis-paperd":
            result.setdefault("daemon", (pid, command))
        elif "--renderer-child --type video" in command:
            result.setdefault("video", (pid, command))
        elif "--renderer-child --type web" in command:
            result.setdefault("web", (pid, command))
        elif "anis-paper-scene-engine" in command:
            result.setdefault("scene", (pid, command))
    return result


def stats(pid: int) -> dict[str, int | None]:
    values: dict[str, int | None] = {"pid": pid}
    try:
        text = pathlib.Path(f"/proc/{pid}/smaps_rollup").read_text(encoding="ascii")
    except (OSError, UnicodeError):
        text = ""
    for key in ("Rss", "Pss", "Private_Clean", "Private_Dirty",
                "Shared_Clean", "Shared_Dirty", "Anonymous", "Swap"):
        match = re.search(rf"^{key}:\s+(\d+) kB", text, re.MULTILINE)
        values[key.lower()] = int(match.group(1)) if match else None
    return values


def shm() -> dict[str, Any]:
    names: list[str] = []
    total = 0
    for entry in pathlib.Path("/dev/shm").glob("anispaper-*"):
        try:
            total += entry.stat().st_size
            names.append(entry.name)
        except OSError:
            pass
    return {"count": len(names), "bytes": total, "names": sorted(names)}


def entry_for(status: dict[str, Any], output: str) -> dict[str, Any] | None:
    return next((x for x in status.get("renderers", []) if x.get("output") == output), None)


def wait_ready(rpc: Rpc, output: str, wallpaper_id: str, timeout: float = 30.0) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last: dict[str, Any] | None = None
    while time.monotonic() < deadline:
        status = rpc.call("status.get")
        last = entry_for(status, output)
        if (last and last.get("wallpaperId") == wallpaper_id and
                last.get("hasFrame") and not last.get("safeMode") and
                last.get("crashes", 0) == 0):
            return last
        time.sleep(0.25)
    raise RuntimeError(f"renderer did not stabilize output={output} id={wallpaper_id} last={last}")


def sample(output: str, entry: dict[str, Any]) -> dict[str, Any]:
    procs = processes()
    renderer_name = entry.get("renderer")
    process_name = "scene" if renderer_name == "scene" else renderer_name
    process_values = {}
    for name in ("daemon", process_name):
        if name and name in procs:
            process_values[name] = stats(procs[name][0])
    return {"output": output, "id": entry.get("wallpaperId"),
            "renderer": renderer_name, "renderer_pid": entry.get("pid"),
            "fps": entry.get("fps"), "has_frame": entry.get("hasFrame"),
            "safe_mode": entry.get("safeMode"), "crashes": entry.get("crashes"),
            "processes": process_values, "shm": shm(),
            "time": time.strftime("%Y-%m-%dT%H:%M:%S%z")}


def choose(catalog: list[dict[str, Any]], kind: str, original: str) -> list[str]:
    values = [x.get("id") for x in catalog
              if x.get("type") == kind and isinstance(x.get("id"), str)]
    values = [x for x in values if x != original]
    if len(values) < 3:
        raise RuntimeError(f"catalog has fewer than three {kind} items")
    return values[:3]


def run_phase(rpc: Rpc, catalog: list[dict[str, Any]], kind: str,
              output: str, cycles: int, settle_seconds: float) -> dict[str, Any]:
    status = rpc.call("status.get")
    original_entry = entry_for(status, output)
    if not original_entry or not original_entry.get("wallpaperId"):
        raise RuntimeError(f"no original wallpaper on {output}")
    original = original_entry["wallpaperId"]
    ids = choose(catalog, kind, original)
    sequence = [ids[index % 3] for index in range(cycles)]
    records: list[dict[str, Any]] = []
    try:
        for index, wallpaper_id in enumerate(sequence, 1):
            rpc.call("wallpaper.apply", {"id": wallpaper_id, "output": output})
            entry = wait_ready(rpc, output, wallpaper_id)
            # hasFrame is intentionally published early; allow decoder/OpenGL
            # queues to reach their steady plateau before taking PSS.
            time.sleep(settle_seconds)
            entry = wait_ready(rpc, output, wallpaper_id)
            record = sample(output, entry)
            record["cycle"] = index
            records.append(record)
            print(f"{kind} cycle={index:02d} id={wallpaper_id} renderer={record['renderer']} "
                  f"pid={record['renderer_pid']} pss="
                  f"{record['processes'].get(kind, {}).get('pss')} shm={record['shm']['count']}" ,
                  flush=True)
    finally:
        rpc.call("wallpaper.apply", {"id": original, "output": output})
        wait_ready(rpc, output, original)
    return {"output": output, "kind": kind, "original": original,
            "sequence": sequence, "records": records}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", type=pathlib.Path,
                        default=pathlib.Path(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}")) / "anispaper.sock")
    parser.add_argument("--video-output", default="HDMI-A-1")
    parser.add_argument("--scene-output", default="DP-2")
    parser.add_argument("--cycles", type=int, default=20)
    parser.add_argument("--settle-seconds", type=float, default=3.0)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.cycles < 20:
        parser.error("--cycles must be at least 20")
    if args.settle_seconds < 0:
        parser.error("--settle-seconds must be non-negative")
    rpc = Rpc(args.socket)
    try:
        catalog = rpc.call("catalog.list")
        result = {
            "started": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "video": run_phase(rpc, catalog, "video", args.video_output, args.cycles, args.settle_seconds),
            "scene": run_phase(rpc, catalog, "scene", args.scene_output, args.cycles, args.settle_seconds),
        }
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"replacement_memory_test: PASS output={args.output}")
        return 0
    finally:
        rpc.close()


if __name__ == "__main__":
    raise SystemExit(main())
