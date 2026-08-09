#!/usr/bin/env python3
"""F4 fallback integration for hosts without libwallpaperengine."""

import base64
import json
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import time


DAEMON = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else None


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def wait_for(label, callback, timeout=10, interval=0.08):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            value = callback()
            if value is not None:
                return value
        except Exception as exc:
            last = exc
        time.sleep(interval)
    raise RuntimeError(f"timeout waiting for {label}: {last}")


def unix_socket_available(root):
    path = root / "unix-probe.sock"
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.bind(str(path))
        return True
    except OSError:
        return False
    finally:
        sock.close()
        try:
            path.unlink()
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
    item = steam / "steamapps/workshop/content/431960/400"
    item.mkdir(parents=True)
    preview = item / "preview.jpg"
    subprocess.run(
        ["ffmpeg", "-y", "-f", "lavfi", "-i", "color=c=#00c2ff:size=640x360",
         "-frames:v", "1", str(preview)],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    # scene.json is deliberately absent: metadata is still safe/catalogued and
    # the F4 fallback must use preview.jpg rather than attempting extraction.
    (item / "project.json").write_text(json.dumps({
        "title": "F4 scene fallback",
        "type": "Scene",
        "file": "scene.json",
        "preview": "preview.jpg",
        "general": {"properties": {}},
    }), encoding="utf-8")
    vdf = steam / "steamapps/libraryfolders.vdf"
    vdf.parent.mkdir(parents=True, exist_ok=True)
    vdf.write_text(f'"libraryfolders" {{ "0" {{ "path" "{steam}" }} }}', encoding="utf-8")
    return vdf


def main():
    require(DAEMON and DAEMON.is_file(), "usage: f4_integration.py daemon")
    with tempfile.TemporaryDirectory(prefix="anispaper-f4-") as temp:
        root = pathlib.Path(temp)
        if not unix_socket_available(root):
            return 77
        daemon = root / "anis-paperd"
        shutil.copy2(DAEMON, daemon)
        daemon.chmod(0o700)
        runtime = root / "runtime"
        config = root / "config"
        runtime.mkdir()
        config.mkdir()
        env = os.environ.copy()
        env.update({
            "XDG_RUNTIME_DIR": str(runtime),
            "XDG_CONFIG_HOME": str(config),
            "ANISPAPER_STEAM_VDF": str(write_fixture(root)),
        })
        process = subprocess.Popen([str(daemon)], env=env, text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        rpc = None
        output = "F4-SCENE"
        try:
            wait_for("daemon socket", lambda: runtime / "anispaper.sock"
                     if (runtime / "anispaper.sock").exists() else None, timeout=6)
            rpc = Rpc(runtime / "anispaper.sock")

            def catalog_ready():
                status = rpc.call("status.get")
                return status if not status["catalog"]["scanning"] else None

            wait_for("catalog scan", catalog_ready)
            catalog = {item["id"]: item for item in rpc.call("catalog.list")}
            item = catalog.get("steam:400")
            require(item and item["type"] == "scene" and not pathlib.Path(item["file"]).exists(),
                    f"scene fixture was not catalogued as a missing-source scene: {item}")
            rpc.call("wallpaper.apply", {"id": "steam:400", "output": output})

            def ready_scene():
                status = rpc.call("status.get")
                entry = next((item for item in status["renderers"]
                              if item["output"] == output), None)
                if not entry or not entry.get("hasFrame"):
                    return None
                return status, entry

            status, entry = wait_for("scene fallback frame", ready_scene)
            require(entry["renderer"] == "scene-static" and entry["fallback"] is True and
                    entry["safeMode"] is False and entry["pid"] == 0,
                    f"scene did not use static fallback: {entry}")
            require(entry["sceneNativeSupported"] is False and
                    entry["badge"] == "scene sin soporte nativo",
                    f"scene capability badge is invalid: {entry}")
            require(entry["bridge"]["active"] is True and entry["bridge"]["frameNo"] >= 2,
                    f"scene did not publish a bridge frame: {entry}")
            preview = rpc.call("preview.frame", {"output": output})
            jpeg = base64.b64decode(preview["data"], validate=True)
            require(jpeg.startswith(b"\xff\xd8") and jpeg.endswith(b"\xff\xd9"),
                    "scene fallback preview is not JPEG")
            require(status["watchdog"]["count"] == 0 and
                    status["watchdog"]["safeMode"] is False,
                    f"scene fallback touched watchdog state: {status['watchdog']}")
            require(rpc.call("wallpaper.stop", {"output": output})["stopped"] is True,
                    "scene stop did not release the renderer")
            print("f4_integration: missing-scene-source/static-preview/badge/bridge/preview assertions passed")
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
        print(f"f4_integration: FAIL {exc}", file=sys.stderr)
        sys.exit(1)

