#!/usr/bin/env python3
"""One-shot F2 evidence check against the already-running user service."""

import argparse
import base64
import hashlib
import json
import socket
import subprocess
import sys
import time
from pathlib import Path


class Rpc:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(3)
        self.sock.connect(path)
        self.buffer = b""
        self.next_id = 1

    def close(self):
        self.sock.close()

    def line(self, timeout=3):
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError("JSON-RPC response timeout")
            self.sock.settimeout(remaining)
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("daemon closed the socket")
            self.buffer += chunk
        raw, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(raw.decode("utf-8"))

    def call(self, method, params=None, timeout=5):
        identifier = self.next_id
        self.next_id += 1
        request = {"jsonrpc": "2.0", "id": identifier, "method": method}
        if params is not None:
            request["params"] = params
        self.sock.sendall(json.dumps(request, separators=(",", ":")).encode() + b"\n")
        deadline = time.monotonic() + timeout
        while True:
            response = self.line(max(0.01, deadline - time.monotonic()))
            if response.get("id") != identifier:
                continue
            if "error" in response:
                raise RuntimeError(f"{method}: {response['error']}")
            return response["result"]


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def find_entry(status, output):
    for entry in status.get("renderers", []):
        if entry.get("output") == output:
            return entry
    return None


def preview(rpc, output):
    response = rpc.call("preview.frame", {"output": output})
    require(response.get("mimeType") == "image/jpeg", f"{output}: preview MIME is invalid")
    raw = base64.b64decode(response.get("data", ""), validate=True)
    require(raw.startswith(b"\xff\xd8") and raw.endswith(b"\xff\xd9"),
            f"{output}: preview is not a complete JPEG")
    require(response.get("width", 0) > 0 and response.get("height", 0) > 0,
            f"{output}: preview dimensions are invalid")
    return response, raw


def wait_preview(rpc, output, timeout=15):
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            return preview(rpc, output)
        except Exception as exc:
            last_error = exc
            time.sleep(0.2)
    raise RuntimeError(f"{output}: preview did not arrive within {timeout}s: {last_error}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", required=True)
    parser.add_argument("--video-id", required=True)
    parser.add_argument("--web-id", required=True)
    parser.add_argument("--video-output", default="HDMI-A-1")
    parser.add_argument("--web-output", default="DP-2")
    parser.add_argument("--duration", type=float, default=30.0)
    parser.add_argument("--integration-binary",
                        help="run the /tmp F2 integration suite first using this binary")
    args = parser.parse_args()
    require(args.duration > 0, "--duration must be positive")

    rpc = None
    applied = []
    try:
        if args.integration_binary:
            binary = Path(args.integration_binary).resolve()
            require(binary.is_file(), "--integration-binary does not exist")
            suite = Path(__file__).resolve().parents[2] / "tests" / "f2_integration.py"
            completed = subprocess.run([sys.executable, str(suite), str(binary)], check=False)
            require(completed.returncode == 0,
                    f"F2 /tmp integration suite failed with exit {completed.returncode}")
            print("f2.integration=PASS")
        rpc = Rpc(args.socket)
        catalog = {item.get("id"): item for item in rpc.call("catalog.list")}
        video = catalog.get(args.video_id)
        web = catalog.get(args.web_id)
        require(video and video.get("type") == "video", "--video-id is not a catalog video")
        require(web and web.get("type") == "web", "--web-id is not a catalog web item")
        monitors = rpc.call("monitor.list")
        names = {monitor.get("name") for monitor in monitors}
        require(args.video_output in names and args.web_output in names,
                "requested real-test outputs are not available")
        require(args.video_output != args.web_output,
                "video and web must use distinct outputs for the simultaneous check")
        print("monitor.outputs=" + json.dumps(sorted(names), separators=(",", ":")))
        print(f"video.id={args.video_id}")
        print(f"web.id={args.web_id}")

        rpc.call("wallpaper.apply", {"id": args.video_id, "output": args.video_output})
        applied.append(args.video_output)
        rpc.call("wallpaper.apply", {"id": args.web_id, "output": args.web_output})
        applied.append(args.web_output)
        video_payload, video_jpeg = wait_preview(rpc, args.video_output)
        web_payload, web_jpeg = wait_preview(rpc, args.web_output)
        print("video.preview.initial=" + hashlib.sha256(video_jpeg).hexdigest())
        print("web.preview.initial=" + hashlib.sha256(web_jpeg).hexdigest())

        video_frames = {hashlib.sha256(video_jpeg).hexdigest()}
        web_frames = {hashlib.sha256(web_jpeg).hexdigest()}
        samples = 1
        deadline = time.monotonic() + args.duration
        while time.monotonic() < deadline:
            time.sleep(min(1.0, max(0.0, deadline - time.monotonic())))
            video_payload, video_jpeg = preview(rpc, args.video_output)
            web_payload, web_jpeg = preview(rpc, args.web_output)
            video_frames.add(hashlib.sha256(video_jpeg).hexdigest())
            web_frames.add(hashlib.sha256(web_jpeg).hexdigest())
            samples += 1
            status = rpc.call("status.get")
            watchdog = status.get("watchdog", {})
            require(watchdog.get("count") == 0,
                    f"watchdog.count is not zero: {watchdog}")
            require(watchdog.get("safeMode") is False,
                    f"watchdog entered safe mode: {watchdog}")
            for output, wallpaper_id, expected_type in (
                (args.video_output, args.video_id, "video"),
                (args.web_output, args.web_id, "web"),
            ):
                entry = find_entry(status, output)
                require(entry is not None, f"{output}: renderer disappeared")
                require(entry.get("wallpaperId") == wallpaper_id and
                        entry.get("renderer") == expected_type and
                        entry.get("safeMode") is False and entry.get("pid", 0) > 0,
                        f"{output}: renderer status is invalid: {entry}")
        # A real video must move; the static web fixture may legitimately remain
        # unchanged, so only its valid repeated JPEG is required.
        require(len(video_frames) >= 2, "video preview did not change over 30 seconds")
        print(f"duration.seconds={args.duration:g}")
        print(f"preview.samples={samples}")
        print(f"video.preview.unique_frames={len(video_frames)}")
        print(f"web.preview.unique_frames={len(web_frames)}")
        print("watchdog.count=0")
        print("safe_mode=false")
        print("f2.real-check=PASS")
        return 0
    except Exception as exc:
        print(f"f2.real-check=FAIL: {exc}", file=sys.stderr)
        return 1
    finally:
        if rpc:
            for output in reversed(applied):
                try:
                    rpc.call("wallpaper.stop", {"output": output})
                except Exception:
                    pass
            rpc.close()


if __name__ == "__main__":
    sys.exit(main())
