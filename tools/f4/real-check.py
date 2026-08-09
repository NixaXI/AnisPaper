#!/usr/bin/env python3
"""One-shot F4 evidence check for the explicit SceneRenderer fallback."""

import argparse
import base64
import hashlib
import json
import pathlib
import socket
import subprocess
import sys
import time


class Rpc:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(4)
        self.sock.connect(str(path))
        self.buffer = b""
        self.next_id = 1

    def close(self):
        self.sock.close()

    def call(self, method, params=None, timeout=8):
        identifier = self.next_id
        self.next_id += 1
        payload = {"jsonrpc": "2.0", "id": identifier, "method": method}
        if params is not None:
            payload["params"] = params
        self.sock.sendall(json.dumps(payload, separators=(",", ":")).encode() + b"\n")
        deadline = time.monotonic() + timeout
        while True:
            while b"\n" not in self.buffer:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise RuntimeError(f"{method}: JSON-RPC timeout")
                self.sock.settimeout(remaining)
                chunk = self.sock.recv(65536)
                if not chunk:
                    raise RuntimeError("daemon closed JSON-RPC socket")
                self.buffer += chunk
            raw, self.buffer = self.buffer.split(b"\n", 1)
            response = json.loads(raw.decode("utf-8"))
            if response.get("id") != identifier:
                continue
            if "error" in response:
                raise RuntimeError(f"{method}: {response['error']}")
            return response["result"]


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run_integration(binary):
    if not binary:
        return
    suite = pathlib.Path(__file__).resolve().parents[2] / "tests" / "f4_integration.py"
    completed = subprocess.run([sys.executable, str(suite), binary], check=False,
                               capture_output=True, text=True)
    if completed.stdout:
        print(completed.stdout, end="")
    require(completed.returncode == 0,
            "F4 integration suite failed:\n" + completed.stderr)
    print("f4.integration=PASS")


def entry_for(status, output):
    return next((entry for entry in status.get("renderers", [])
                 if entry.get("output") == output), None)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", required=True)
    parser.add_argument("--output", default="HDMI-A-1")
    parser.add_argument("--scene-id")
    parser.add_argument("--duration", type=float, default=5.0)
    parser.add_argument("--integration-binary")
    args = parser.parse_args()
    require(args.duration > 0, "--duration must be positive")

    rpc = None
    try:
        run_integration(args.integration_binary)
        rpc = Rpc(args.socket)
        catalog = rpc.call("catalog.list")
        selected = None
        if args.scene_id:
            selected = next((item for item in catalog if item.get("id") == args.scene_id), None)
        else:
            # Prefer a real preview so the live fallback is representative.
            scenes = [item for item in catalog if item.get("type") == "scene"]
            selected = next((item for item in scenes
                             if pathlib.Path(item.get("preview", "")).is_file()),
                            scenes[0] if scenes else None)
        require(selected and selected.get("type") == "scene",
                "no scene is available in the real catalog")
        outputs = {item.get("name") for item in rpc.call("monitor.list")}
        require(args.output in outputs, f"output is unavailable: {args.output}")
        rpc.call("wallpaper.apply", {"id": selected["id"], "output": args.output})

        deadline = time.monotonic() + 15
        entry = None
        preview = None
        while time.monotonic() < deadline:
            status = rpc.call("status.get")
            entry = entry_for(status, args.output)
            try:
                preview = rpc.call("preview.frame", {"output": args.output})
            except RuntimeError:
                preview = None
            if entry and preview and entry.get("hasFrame"):
                break
            time.sleep(0.15)
        require(entry and preview, "scene fallback did not expose a preview")
        require(entry.get("renderer") == "scene-static" and
                entry.get("fallback") is True and entry.get("safeMode") is False and
                entry.get("pid") == 0 and entry.get("sceneNativeSupported") is False and
                entry.get("badge") == "scene sin soporte nativo",
                f"scene fallback status is invalid: {entry}")
        require(entry.get("bridge", {}).get("active") is True and
                entry["bridge"].get("frameNo", 0) >= 2,
                f"scene fallback bridge is invalid: {entry}")
        jpeg = base64.b64decode(preview.get("data", ""), validate=True)
        require(jpeg.startswith(b"\xff\xd8") and jpeg.endswith(b"\xff\xd9"),
                "scene preview is not JPEG")

        samples = 0
        until = time.monotonic() + args.duration
        while time.monotonic() < until:
            time.sleep(min(1.0, max(0.0, until - time.monotonic())))
            status = rpc.call("status.get")
            entry = entry_for(status, args.output)
            require(entry and entry.get("renderer") == "scene-static" and
                    entry.get("hasFrame") and entry.get("safeMode") is False,
                    f"scene fallback stopped during real check: {entry}")
            watchdog = status.get("watchdog", {})
            require(watchdog.get("count") == 0 and watchdog.get("safeMode") is False,
                    f"scene fallback changed watchdog state: {watchdog}")
            samples += 1

        print(f"scene.id={selected['id']}")
        print(f"scene.preview={selected.get('preview', '')}")
        print("renderer=scene-static")
        print("fallback=true")
        print("scene.native_supported=false")
        print("scene.badge=scene sin soporte nativo")
        print(f"preview.sha256={hashlib.sha256(jpeg).hexdigest()}")
        print(f"duration.seconds={args.duration:g}")
        print(f"samples={samples}")
        print("watchdog.count=0")
        print("safe_mode=false")
        print("f4.real-check=PASS")
        return 0
    except Exception as exc:
        print(f"f4.real-check=FAIL: {exc}", file=sys.stderr)
        return 1
    finally:
        if rpc:
            rpc.close()


if __name__ == "__main__":
    sys.exit(main())

