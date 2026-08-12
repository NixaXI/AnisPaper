#!/usr/bin/env python3
"""Evidence-only F1 service check; optional watcher probe writes one new workshop item."""
import argparse
import json
import os
from pathlib import Path
import socket
import sys
import time

# Keep this in sync with the daemon's kMaxResponse.  A real catalog can
# legitimately exceed 1 MiB once item properties are included.
MAX_LINE = 4 * 1024 * 1024
REQUIRED_ITEM_FIELDS = {"id", "title", "type", "file", "preview", "tags", "properties", "source", "root"}


class Rpc:
    def __init__(self, path: str):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(2)
        self.sock.connect(path)
        self.buffer = b""
        self.next_id = 1

    def close(self):
        self.sock.close()

    def read_line(self, timeout=2):
        old = self.sock.gettimeout()
        self.sock.settimeout(timeout)
        try:
            while b"\n" not in self.buffer:
                chunk = self.sock.recv(65536)
                if not chunk:
                    raise RuntimeError("daemon closed the socket")
                self.buffer += chunk
                if len(self.buffer) > MAX_LINE:
                    raise RuntimeError("daemon response exceeded 4 MiB")
            raw, self.buffer = self.buffer.split(b"\n", 1)
            return json.loads(raw.decode("utf-8"))
        finally:
            self.sock.settimeout(old)

    def call(self, method, params=None):
        ident = self.next_id
        self.next_id += 1
        request = {"jsonrpc": "2.0", "id": ident, "method": method}
        if params is not None:
            request["params"] = params
        self.sock.sendall(json.dumps(request, separators=(",", ":")).encode() + b"\n")
        while True:
            message = self.read_line()
            if message.get("id") == ident:
                if "error" in message:
                    raise RuntimeError(f"{method}: RPC error {message['error']}")
                return message["result"]

    def wait_catalog_event(self, project_id, field, timeout=2):
        end = time.monotonic() + timeout
        while True:
            remaining = end - time.monotonic()
            if remaining <= 0:
                raise RuntimeError(f"timed out waiting for catalog.changed {field} {project_id}")
            message = self.read_line(remaining)
            if message.get("method") != "catalog.changed":
                continue
            params = message.get("params", {})
            values = params.get(field, [])
            if project_id in values:
                return params


def require_invalid_params(rpc, method):
    ident = rpc.next_id
    rpc.next_id += 1
    rpc.sock.sendall(json.dumps({"jsonrpc": "2.0", "id": ident, "method": method, "params": {}}, separators=(",", ":")).encode() + b"\n")
    while True:
        response = rpc.read_line()
        if response.get("id") != ident:
            continue
        error = response.get("error", {})
        if error.get("code") != -32602:
            raise RuntimeError(f"{method}: expected invalid-params boundary, got {error}")
        print(f"{method}.invalid_params=-32602")
        return


def wait_ready(rpc):
    end = time.monotonic() + 10
    while time.monotonic() < end:
        status = rpc.call("status.get")
        if not status.get("catalog", {}).get("scanning", True):
            print(f"status.ready generation={status['catalog'].get('generation', '?')}")
            return
        time.sleep(0.05)
    raise RuntimeError("catalog.scanning remained true for 10s")


def check_stability(rpc, seconds):
    if seconds <= 0:
        return
    end = time.monotonic() + seconds
    generation = None
    watch_count = watch_failures = None
    while True:
        status = rpc.call("status.get")
        catalog, watch = status.get("catalog", {}), status.get("watch", {})
        current = catalog.get("generation")
        if catalog.get("scanning", True):
            raise RuntimeError("status stability failed: catalog.scanning=true")
        if generation is None:
            generation = current
        elif current != generation:
            raise RuntimeError(f"status stability failed: generation {generation}->{current}")
        watch_count, watch_failures = watch.get("count"), watch.get("failures")
        if time.monotonic() >= end:
            break
        time.sleep(min(0.1, max(0.0, end - time.monotonic())))
    print(f"status.stable generation={generation} seconds={seconds:g} watch_count={watch_count} watch_failures={watch_failures}")


def inspect_catalog(rpc, expected_count=None, real_mode=False):
    items = rpc.call("catalog.list")
    counts = {}
    missing_type = False
    unknown = 0
    ids = set()
    for item in items:
        if not isinstance(item, dict) or not REQUIRED_ITEM_FIELDS.issubset(item):
            raise RuntimeError("catalog item does not have the F1 schema")
        kind = item["type"]
        if item["id"] in ids:
            raise RuntimeError("catalog has duplicate IDs")
        ids.add(item["id"])
        if not isinstance(kind, str) or kind != kind.lower():
            raise RuntimeError("catalog has a non-normalized type")
        if not kind:
            missing_type = True
        if kind == "unknown":
            unknown += 1
        counts[kind] = counts.get(kind, 0) + 1
    print(f"catalog.count={len(items)}")
    print("catalog.types=" + json.dumps(counts, sort_keys=True, separators=(",", ":")))
    print(f"catalog.has_missing_type={'true' if missing_type else 'false'}")
    print(f"catalog.unknown_count={unknown}")
    expected = 545 if real_mode and expected_count is None else expected_count
    if expected is not None and len(items) != expected:
        raise RuntimeError(f"catalog expected {expected} unique items, got {len(items)}")
    if real_mode:
        target = {"scene": 312, "video": 219, "web": 12, "unknown": 2}
        if counts != target:
            raise RuntimeError("real catalog type counts differ from scene=312 video=219 web=12 unknown=2")


def inspect_monitors(rpc, real_mode):
    monitors = rpc.call("monitor.list")
    compact = []
    for monitor in monitors:
        geometry = monitor.get("geometry")
        if not isinstance(monitor.get("name"), str) or not isinstance(geometry, dict):
            raise RuntimeError("monitor.list shape is invalid")
        if set(geometry) != {"x", "y", "width", "height"}:
            raise RuntimeError("monitor geometry shape is invalid")
        compact.append({"name": monitor["name"], "geometry": geometry,
                        "currentWallpaperId": monitor.get("currentWallpaperId")})
    if real_mode:
        names = {m["name"] for m in compact}
        required = {"HDMI-A-1", "DP-2"}
        if not required.issubset(names):
            raise RuntimeError("real monitor check expected HDMI-A-1 and DP-2")
    print("monitor.outputs=" + json.dumps(compact, separators=(",", ":")))


def watcher_probe(rpc, root_arg):
    root = Path(root_arg).resolve(strict=True)
    suffix = Path("steamapps/workshop/content/431960")
    if tuple(root.parts[-len(suffix.parts):]) != suffix.parts:
        raise RuntimeError("--watch-root must canonically end in /steamapps/workshop/content/431960")
    project_dir = None
    project_id = None
    try:
        # A high numeric ID is chosen deterministically from pid/time and mkdir
        # is exclusive; no pre-existing workshop directory is ever touched.
        base = 8_000_000_000_000 + (os.getpid() % 1_000_000) * 100 + (time.time_ns() % 100)
        for candidate in range(base, base + 100):
            possible = root / str(candidate)
            try:
                possible.mkdir()
                project_dir, project_id = possible, str(candidate)
                break
            except FileExistsError:
                continue
        if project_dir is None:
            raise RuntimeError("could not allocate a temporary numeric workshop id")
        media = project_dir / "f1-probe.webm"
        metadata = project_dir / "project.json"
        media.write_bytes(b"")
        metadata.write_text(json.dumps({"title": "F1 watcher probe", "file": "f1-probe.webm"}), encoding="utf-8")
        started = time.monotonic()
        added = rpc.wait_catalog_event("steam:" + project_id, "added", 2)
        elapsed = int((time.monotonic() - started) * 1000)
        print(f"watch.added_ms={elapsed} ids=" + json.dumps(added.get("added", []), separators=(",", ":")))
        if elapsed >= 2000:
            raise RuntimeError("watch add exceeded 2 seconds")
        media.unlink()
        metadata.unlink()
        project_dir.rmdir()
        started = time.monotonic()
        removed = rpc.wait_catalog_event("steam:" + project_id, "removed", 2)
        elapsed = int((time.monotonic() - started) * 1000)
        print(f"watch.removed_ms={elapsed} ids=" + json.dumps(removed.get("removed", []), separators=(",", ":")))
        if elapsed >= 2000:
            raise RuntimeError("watch remove exceeded 2 seconds")
    finally:
        if project_dir is not None:
            # Exact names only: do not recursively remove or inspect user content.
            for name in ("f1-probe.webm", "project.json"):
                candidate = project_dir / name
                if candidate.exists():
                    candidate.unlink()
            try:
                project_dir.rmdir()
            except FileNotFoundError:
                pass
            except OSError as exc:
                raise RuntimeError(f"temporary probe directory not empty: {project_dir}: {exc}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", required=True)
    parser.add_argument("--watch-root")
    parser.add_argument("--expected-count", type=int)
    parser.add_argument("--stability-seconds", type=float, default=0)
    args = parser.parse_args()
    rpc = None
    try:
        rpc = Rpc(args.socket)
        wait_ready(rpc)
        if args.stability_seconds < 0:
            raise RuntimeError("--stability-seconds must be non-negative")
        check_stability(rpc, args.stability_seconds)
        inspect_catalog(rpc, expected_count=args.expected_count, real_mode=args.watch_root is not None)
        inspect_monitors(rpc, real_mode=args.watch_root is not None)
        # F1's smoke check must remain non-mutating after F2/F3 implement the
        # renderer RPCs.  Missing required parameters are rejected before any
        # renderer can be created.
        require_invalid_params(rpc, "wallpaper.apply")
        require_invalid_params(rpc, "preview.frame")
        if rpc.call("events.subscribe") != {"subscribed": True}:
            raise RuntimeError("events.subscribe returned an unexpected result")
        print("events.subscribed=true")
        if args.watch_root:
            watcher_probe(rpc, args.watch_root)
        return 0
    except Exception as exc:
        print(f"real-check: FAIL: {exc}", file=sys.stderr)
        return 1
    finally:
        if rpc:
            rpc.close()


if __name__ == "__main__":
    sys.exit(main())
