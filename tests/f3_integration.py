#!/usr/bin/env python3
"""F3 bridge integration: daemon producer plus the real QML image provider."""

import base64
import json
import os
import pathlib
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time


DAEMON = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else None
PLUGIN_SMOKE = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else None
HEADER = struct.Struct("<4sIIQQI")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def wait_for(label, callback, timeout=12.0, interval=0.05):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            value = callback()
            if value is not None:
                return value
        except Exception as exc:  # transient daemon/renderer startup state
            last = exc
        time.sleep(interval)
    raise RuntimeError(f"timeout waiting for {label}: {last}")


def unix_socket_available(root):
    probe = root / "unix-probe.sock"
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.bind(str(probe))
        return True
    except OSError:
        return False
    finally:
        sock.close()
        try:
            probe.unlink()
        except FileNotFoundError:
            pass


class Rpc:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(3)
        self.sock.connect(str(path))
        self.buffer = b""
        self.next_id = 1

    def close(self):
        self.sock.close()

    def call(self, method, params=None, timeout=6):
        identifier = self.next_id
        self.next_id += 1
        request = {"jsonrpc": "2.0", "id": identifier, "method": method}
        if params is not None:
            request["params"] = params
        self.sock.sendall(json.dumps(request, separators=(",", ":")).encode() + b"\n")
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


def write_fixture(root):
    steam = root / "steam"
    item = steam / "steamapps/workshop/content/431960/300"
    item.mkdir(parents=True)
    clip = item / "bridge.mp4"
    subprocess.run(
        ["ffmpeg", "-y", "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30",
         "-t", "4", "-an", "-pix_fmt", "yuv420p", str(clip)],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    subprocess.run(
        ["ffmpeg", "-y", "-i", str(clip), "-frames:v", "1", str(item / "preview.jpg")],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    (item / "project.json").write_text(json.dumps({
        "title": "F3 bridge video",
        "type": "Video",
        "file": "bridge.mp4",
        "preview": "preview.jpg",
        "general": {"properties": {"loop": True}},
    }), encoding="utf-8")
    vdf = steam / "steamapps/libraryfolders.vdf"
    vdf.parent.mkdir(parents=True, exist_ok=True)
    vdf.write_text(f'"libraryfolders" {{ "0" {{ "path" "{steam}" }} }}', encoding="utf-8")
    return vdf


def read_header(path):
    raw = path.read_bytes()[:HEADER.size]
    require(len(raw) == HEADER.size, "bridge header is truncated")
    magic, width, height, frame_no, stamp, stride = HEADER.unpack(raw)
    return {"magic": magic, "width": width, "height": height,
            "frameNo": frame_no, "timestampNs": stamp, "stride": stride}


def main():
    require(DAEMON and DAEMON.is_file(), "usage: f3_integration.py daemon plugin-smoke")
    require(PLUGIN_SMOKE and PLUGIN_SMOKE.is_file(), "plugin smoke executable is missing")
    with tempfile.TemporaryDirectory(prefix="anispaper-f3-") as temp:
        root = pathlib.Path(temp)
        if not unix_socket_available(root):
            return 77
        smoke_binary = root / "anispaper-f3-plugin-smoke"
        daemon_binary = root / "anis-paperd"
        shutil.copy2(PLUGIN_SMOKE, smoke_binary)
        shutil.copy2(DAEMON, daemon_binary)
        smoke_binary.chmod(0o700)
        daemon_binary.chmod(0o700)
        smoke = subprocess.run([str(smoke_binary)], check=False,
                               capture_output=True, text=True)
        require(smoke.returncode == 0,
                "QQuickView plugin smoke failed:\n" + smoke.stdout + smoke.stderr)
        vdf = write_fixture(root)
        runtime = root / "runtime"
        config = root / "config"
        runtime.mkdir()
        config.mkdir()
        env = os.environ.copy()
        env.update({
            "XDG_RUNTIME_DIR": str(runtime),
            "XDG_CONFIG_HOME": str(config),
            "ANISPAPER_STEAM_VDF": str(vdf),
            "QTWEBENGINE_CHROMIUM_FLAGS": "--disable-gpu",
        })
        parent_runtime = os.environ.get("XDG_RUNTIME_DIR")
        if parent_runtime:
            env["ANISPAPER_RENDERER_RUNTIME_DIR"] = parent_runtime
        process = subprocess.Popen([str(daemon_binary)], env=env, text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        rpc = None
        output = "F3/Bridge: Test"
        shm_path = pathlib.Path("/dev/shm") / "anispaper-F3_Bridge__Test"
        try:
            wait_for("daemon socket", lambda: runtime.joinpath("anispaper.sock")
                     if runtime.joinpath("anispaper.sock").exists() else None, timeout=6)
            rpc = Rpc(runtime / "anispaper.sock")
            wait_for("catalog scan", lambda: rpc.call("status.get")
                     if not rpc.call("status.get")["catalog"]["scanning"] else None, timeout=10)
            catalog = {item["id"]: item for item in rpc.call("catalog.list")}
            require(catalog.get("steam:300", {}).get("type") == "video",
                    "fixture video is absent from the catalog")
            rpc.call("wallpaper.apply", {"id": "steam:300", "output": output})

            def bridge_ready():
                if not shm_path.exists():
                    return None
                header = read_header(shm_path)
                if header["magic"] != b"ANIS" or header["frameNo"] == 0:
                    return None
                return header

            initial = wait_for("initial fallback bridge frame", bridge_ready, timeout=8)
            require(initial["width"] == 640 and initial["height"] == 360 and
                    initial["stride"] == 2560 and initial["timestampNs"] > 0,
                    f"bridge header contract mismatch: {initial}")

            sequence = []
            deadline = time.monotonic() + 12
            while time.monotonic() < deadline:
                header = read_header(shm_path)
                require(header["magic"] == b"ANIS", "bridge magic changed")
                # frameNo == 0 is the producer's write-in-progress marker. A
                # consumer must retain its last verified frame and retry on a
                # later poll rather than treating the single payload as stable.
                if header["frameNo"] == 0:
                    time.sleep(0.001)
                    continue
                require(header["frameNo"] >= (sequence[-1] if sequence else 0),
                        f"bridge frame sequence regressed: {sequence[-1:]} -> {header}")
                sequence.append(header["frameNo"])
                if header["frameNo"] >= 100:
                    break
                time.sleep(0.025)
            require(sequence[-1] >= 100,
                    f"daemon did not publish 100 frames: final={sequence[-1]}")
            require(len(set(sequence)) >= 20,
                    f"bridge sequence did not advance enough: {len(set(sequence))}")

            preview = rpc.call("preview.frame", {"output": output})
            jpeg = base64.b64decode(preview["data"], validate=True)
            require(jpeg.startswith(b"\xff\xd8") and jpeg.endswith(b"\xff\xd9"),
                    "preview.frame is not a valid JPEG")
            status = rpc.call("status.get")
            entry = next((x for x in status["renderers"] if x["output"] == output), None)
            require(entry and entry["bridge"]["active"] is True and
                    entry["bridge"]["name"] == "/anispaper-F3_Bridge__Test" and
                    entry["bridge"]["frameNo"] >= 100,
                    f"bridge status is invalid: {entry}")
            stopped = rpc.call("wallpaper.stop", {"output": output})
            require(stopped["stopped"] is True, "wallpaper.stop did not own the bridge")
            wait_for("bridge unlink", lambda: True if not shm_path.exists() else None,
                     timeout=3)
            print("f3_integration: 100-frame shm sequence/QQuickView/fallback/reset/multi-output assertions passed")
            return 0
        finally:
            if rpc:
                try:
                    rpc.call("wallpaper.stop", {"output": output})
                except Exception:
                    pass
                rpc.close()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=4)
            stdout, stderr = process.communicate(timeout=1)
            if process.returncode not in (0, -15):
                print("daemon stdout:\n" + stdout, file=sys.stderr)
                print("daemon stderr:\n" + stderr, file=sys.stderr)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"f3_integration: FAIL {exc}", file=sys.stderr)
        sys.exit(1)
