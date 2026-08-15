#!/usr/bin/env python3
"""Read-only AnisPaper memory/SHM soak sampler.

It never changes settings, wallpapers, services, or processes.  The caller
chooses a bounded duration; each row is flushed so an interrupted soak still
leaves usable evidence.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import pathlib
import re
import socket
import subprocess
import time
from typing import Any


def processes() -> dict[str, tuple[int, str]]:
    result: dict[str, tuple[int, str]] = {}
    try:
        lines = subprocess.check_output(["ps", "-eo", "pid=,args="], text=True).splitlines()
    except (OSError, subprocess.SubprocessError):
        return result
    for line in lines:
        line = line.strip()
        if not line:
            continue
        try:
            pid_text, command = line.split(None, 1)
            pid = int(pid_text)
        except (ValueError, IndexError):
            continue
        if "anis-paper-scene-engine" in command:
            result.setdefault("scene", (pid, command))
        elif "--renderer-child --type video" in command:
            result.setdefault("video", (pid, command))
        elif command.endswith("/anis-paperd") or command == "anis-paperd":
            result.setdefault("daemon", (pid, command))
        elif "plasmashell" in command and "pgrep" not in command:
            result.setdefault("plasmashell", (pid, command))
        elif "QtWebEngineProcess" in command or "ffmpeg" in command:
            # Keep helpers separate by PID: a WebEngine renderer can spawn
            # several processes and collapsing them would hide real growth.
            result[f"helper-{pid}"] = (pid, command)
    return result


def proc_stats(pid: int) -> dict[str, Any]:
    values: dict[str, Any] = {"pid": pid}
    try:
        text = pathlib.Path(f"/proc/{pid}/smaps_rollup").read_text(encoding="ascii")
        for key in ("Rss", "Pss", "Private_Clean", "Private_Dirty", "Shared_Clean", "Shared_Dirty", "Swap"):
            match = re.search(rf"^{re.escape(key)}:\s+(\d+) kB", text, re.MULTILINE)
            values[key.lower()] = int(match.group(1)) if match else None
    except (OSError, UnicodeError):
        values.update({key.lower(): None for key in ("Rss", "Pss", "Private_Clean", "Private_Dirty", "Shared_Clean", "Shared_Dirty", "Swap")})
    try:
        stat = pathlib.Path(f"/proc/{pid}/stat").read_text(encoding="ascii")
        fields = stat.rsplit(") ", 1)[-1].split()
        values["cpu_ticks"] = int(fields[11]) + int(fields[12])
    except (OSError, ValueError, IndexError):
        values["cpu_ticks"] = None
    return values


def rpc_status(socket_path: pathlib.Path) -> dict[str, Any] | None:
    request = {"jsonrpc": "2.0", "id": 1, "method": "status.get", "params": {}}
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(4.0)
            sock.connect(str(socket_path))
            sock.sendall((json.dumps(request, separators=(",", ":")) + "\n").encode())
            data = b""
            while b"\n" not in data:
                data += sock.recv(65536)
        response = json.loads(data.split(b"\n", 1)[0])
        return response.get("result") if isinstance(response.get("result"), dict) else None
    except (OSError, ValueError, json.JSONDecodeError):
        return None


def shm_stats() -> tuple[int, int, list[str]]:
    names: list[str] = []
    total = 0
    try:
        for entry in pathlib.Path("/dev/shm").iterdir():
            if not entry.name.startswith("anispaper-"):
                continue
            try:
                total += entry.stat().st_size
                names.append(entry.name)
            except OSError:
                continue
    except OSError:
        pass
    return len(names), total, sorted(names)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", type=pathlib.Path, default=pathlib.Path(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}")) / "anispaper.sock")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--duration-seconds", type=float, default=6 * 60 * 60)
    parser.add_argument("--interval-seconds", type=float, default=30.0)
    args = parser.parse_args()
    if args.duration_seconds <= 0 or args.interval_seconds <= 0:
        parser.error("duration and interval must be positive")
    return args


def main() -> int:
    args = parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    fields = ["time", "elapsed_s", "mode", "wallpaper_ids", "renderers", "watchdog_count", "safe_mode", "has_frame", "frame_nos", "shm_count", "shm_bytes", "shm_names", "process_stats"]
    started = time.monotonic()
    with args.output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        stream.flush()
        while time.monotonic() - started < args.duration_seconds:
            status = rpc_status(args.socket) or {}
            renderers = status.get("renderers", []) if isinstance(status.get("renderers"), list) else []
            process_values = {name: proc_stats(pid) for name, (pid, _command) in processes().items()}
            shm_count, shm_bytes, shm_names = shm_stats()
            row = {
                "time": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                "elapsed_s": round(time.monotonic() - started, 3),
                "mode": status.get("gaming", {}).get("mode") if isinstance(status.get("gaming"), dict) else None,
                "wallpaper_ids": json.dumps({x.get("output"): x.get("wallpaperId") for x in renderers}, sort_keys=True),
                "renderers": len(renderers),
                "watchdog_count": status.get("watchdog", {}).get("count") if isinstance(status.get("watchdog"), dict) else None,
                "safe_mode": status.get("watchdog", {}).get("safeMode") if isinstance(status.get("watchdog"), dict) else None,
                "has_frame": json.dumps({x.get("output"): x.get("hasFrame") for x in renderers}, sort_keys=True),
                "frame_nos": json.dumps({x.get("output"): x.get("bridge", {}).get("frameNo") for x in renderers}, sort_keys=True),
                "shm_count": shm_count,
                "shm_bytes": shm_bytes,
                "shm_names": json.dumps(shm_names),
                "process_stats": json.dumps(process_values, sort_keys=True),
            }
            writer.writerow(row)
            stream.flush()
            os.fsync(stream.fileno())
            remaining = args.duration_seconds - (time.monotonic() - started)
            if remaining > 0:
                time.sleep(min(args.interval_seconds, remaining))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
