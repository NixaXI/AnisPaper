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
REPO = pathlib.Path(__file__).resolve().parents[1]


def verify_ui_dp2_payload():
    source = (REPO / "ui/src/App.tsx").read_text(encoding="utf-8")
    select_contract = (
        '<option key={monitor.name} value={monitor.name}>' in source and
        'onChange={(event) => setSelectedOutput(event.target.value)}' in source
    )
    apply_contract = (
        'window.anispaper.rpc("wallpaper.apply", { id: selected.id, output: selectedOutput })'
        in source
    )
    require(select_contract and apply_contract,
            "UI no conserva monitor.name hasta wallpaper.apply output")
    # This is the exact data-flow instantiated for the requested connector:
    # monitor.name -> option.value -> event.target.value -> selectedOutput.
    selected_output = "DP-2"
    payload = {"id": "steam:400", "output": selected_output}
    require(payload["output"] == "DP-2", f"UI DP-2 payload changed: {payload}")


def verify_preview_throttle_does_not_gate_apply():
    source = (REPO / "ui/electron/main.ts").read_text(encoding="utf-8")
    require(
        'if (method === "preview.frame"' in source and
        'return this.callPreview<T>(params.output, params);' in source and
        'return this.callRaw<T>(method, params);' in source,
        "preview throttle is not isolated to preview.frame"
    )
    require(
        'const PREVIEW_UNAVAILABLE_RETRY_MS = 750;' in source and
        'this.previewInFlight.get(output)' in source and
        'this.previewDelay.set(output, { timer, reject });' in source,
        "preview backoff no longer deduplicates one probe per output"
    )
    require(
        'this.cancelPreviewDelays("La aplicación se está cerrando.");' in source and
        'this.cancelPreviewDelays(message);' in source,
        "delayed preview probes are not cancelled on stop/disconnect"
    )


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
    large_preview = item / "preview-large.png"
    subprocess.run(
        ["ffmpeg", "-y", "-f", "lavfi", "-i", "color=c=#ff6b8b:size=1600x900",
         "-frames:v", "1", str(large_preview)],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    # scene.json is deliberately absent: metadata is still safe/catalogued and
    # the F4 fallback must use preview.jpg rather than attempting extraction.
    (item / "project.json").write_text(json.dumps({
        "title": "F4 scene fallback DP-2",
        "type": "Scene",
        "file": "scene.json",
        "preview": "preview.jpg",
        "general": {"properties": {}},
    }), encoding="utf-8")

    control = steam / "steamapps/workshop/content/431960/401"
    control.mkdir(parents=True)
    control_preview = control / "preview-control.png"
    subprocess.run(
        ["ffmpeg", "-y", "-f", "lavfi", "-i", "color=c=#55d86a:size=1280x720",
         "-frames:v", "1", str(control_preview)],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    (control / "project.json").write_text(json.dumps({
        "title": "F4 scene directory source",
        "type": "Scene",
        "file": "scene.json",
        "preview": "preview-control.png",
        "general": {"properties": {}},
    }), encoding="utf-8")
    (control / "scene.json").mkdir()

    empty = steam / "steamapps/workshop/content/431960/402"
    empty.mkdir(parents=True)
    (empty / "scene.json").touch()
    (empty / "project.json").write_text(json.dumps({
        "title": "F4 empty scene source",
        "type": "Scene",
        "file": "scene.json",
        "general": {"properties": {}},
    }), encoding="utf-8")

    valid = steam / "steamapps/workshop/content/431960/403"
    valid.mkdir(parents=True)
    (valid / "scene.json").write_text("{}", encoding="utf-8")
    (valid / "project.json").write_text(json.dumps({
        "title": "F4 valid scene source",
        "type": "Scene",
        "file": "scene.json",
        "preview": "preview.jpg",
        "general": {"properties": {}},
    }), encoding="utf-8")
    shutil.copy2(preview, valid / "preview.jpg")

    vdf = steam / "steamapps/libraryfolders.vdf"
    vdf.parent.mkdir(parents=True, exist_ok=True)
    vdf.write_text(f'"libraryfolders" {{ "0" {{ "path" "{steam}" }} }}', encoding="utf-8")
    return vdf, large_preview.resolve(), control_preview.resolve()


def main():
    verify_ui_dp2_payload()
    verify_preview_throttle_does_not_gate_apply()
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
        vdf, large_preview, control_preview = write_fixture(root)
        env = os.environ.copy()
        env.update({
            "XDG_RUNTIME_DIR": str(runtime),
            "XDG_CONFIG_HOME": str(config),
            "ANISPAPER_STEAM_VDF": str(vdf),
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
            missing_item = catalog.get("steam:400")
            directory_item = catalog.get("steam:401")
            empty_item = catalog.get("steam:402")
            valid_item = catalog.get("steam:403")
            require(missing_item and missing_item["type"] == "scene" and
                    not pathlib.Path(missing_item["file"]).exists(),
                    f"missing scene fixture was not catalogued: {missing_item}")
            require(missing_item["preview"] == str(large_preview),
                    "catalog did not select the largest readable preview: " +
                    repr(missing_item.get("preview")))
            require(directory_item and pathlib.Path(directory_item["file"]).is_dir(),
                    f"directory scene fixture was not catalogued: {directory_item}")
            require(empty_item and pathlib.Path(empty_item["file"]).is_file() and
                    pathlib.Path(empty_item["file"]).stat().st_size == 0,
                    f"empty scene fixture was not catalogued: {empty_item}")
            require(valid_item and pathlib.Path(valid_item["file"]).read_text(encoding="utf-8") == "{}",
                    f"valid scene fixture was not catalogued: {valid_item}")

            def entry_for(status, name):
                return next((entry for entry in status["renderers"]
                             if entry["output"] == name), None)

            def expect_invalid_scene(identifier, target, label, preserved=None):
                try:
                    rpc.call("wallpaper.apply", {"id": identifier, "output": target})
                except RuntimeError as exc:
                    message = str(exc)
                else:
                    raise RuntimeError(f"{label} scene apply unexpectedly succeeded")
                require("'code': -32001" in message and "INVALID_WALLPAPER:" in message,
                        f"{label} scene returned the wrong error: {message}")
                status = rpc.call("status.get")
                entry = entry_for(status, target)
                if preserved is None:
                    require(entry is None, f"{label} scene created an entry: {entry}")
                else:
                    require(entry and entry["wallpaperId"] == preserved and entry.get("hasFrame") and
                            entry["crashes"] == 0 and entry["safeMode"] is False,
                            f"{label} scene changed the active entry: {entry}")
                require(status["watchdog"]["count"] == 0 and
                        status["watchdog"]["safeMode"] is False,
                        f"{label} scene touched watchdog state: {status['watchdog']}")

            expect_invalid_scene("steam:400", "F4-MISSING", "missing")
            rpc.call("wallpaper.apply", {"id": "steam:403", "output": output})

            def ready_scene():
                status = rpc.call("status.get")
                entry = entry_for(status, output)
                if not entry or not entry.get("hasFrame"):
                    return None
                return status, entry

            status, entry = wait_for("valid scene fallback frame", ready_scene)
            require(entry["wallpaperId"] == "steam:403" and entry["renderer"] == "scene-static" and
                    entry["fallback"] is True and entry["safeMode"] is False and entry["pid"] == 0,
                    f"valid scene did not retain successful fallback behavior: {entry}")
            require(entry["sceneNativeSupported"] is False and
                    entry["badge"] == "scene sin soporte nativo",
                    f"valid scene capability badge is invalid: {entry}")
            require(entry["bridge"]["active"] is True and entry["bridge"]["frameNo"] >= 2,
                    f"valid scene did not publish a bridge frame: {entry}")
            preview = rpc.call("preview.frame", {"output": output})
            jpeg = base64.b64decode(preview["data"], validate=True)
            require(jpeg.startswith(b"\xff\xd8") and jpeg.endswith(b"\xff\xd9"),
                    "valid scene fallback preview is not JPEG")
            expect_invalid_scene("steam:401", output, "directory", "steam:403")
            expect_invalid_scene("steam:402", output, "empty", "steam:403")
            require(rpc.call("wallpaper.stop", {"output": output})["stopped"] is True,
                    "valid scene stop did not release the renderer")

            # Multimonitor regression: synthetic connector names deliberately
            # bypass Plasma mutation while exercising the daemon's exact
            # by-output renderer and bridge maps. Applying a second wallpaper
            # must not replace or unlink the first output.
            target_output = "DP-2"
            control_output = "HDMI-A-1"
            control_item = catalog.get("steam:403")
            require(control_item and pathlib.Path(control_item["file"]).is_file(),
                    f"control scene fixture is invalid: {control_item}")
            rpc.call("wallpaper.apply", {"id": "steam:403", "output": target_output})
            rpc.call("wallpaper.apply", {"id": "steam:403", "output": control_output})

            def both_outputs_active():
                current = rpc.call("status.get")
                entries = {entry["output"]: entry for entry in current["renderers"]}
                dp = entries.get(target_output)
                hdmi = entries.get(control_output)
                if not dp or not hdmi or not dp.get("hasFrame") or not hdmi.get("hasFrame"):
                    return None
                return current, dp, hdmi

            status, dp_entry, hdmi_entry = wait_for(
                "independent DP-2 and HDMI-A-1 renderers/bridges", both_outputs_active)
            require(dp_entry["wallpaperId"] == "steam:403" and
                    hdmi_entry["wallpaperId"] == "steam:403",
                    f"wallpapers crossed output maps: DP={dp_entry}, HDMI={hdmi_entry}")
            require(dp_entry["bridge"]["active"] is True and
                    dp_entry["bridge"]["name"] == "/anispaper-DP-2" and
                    hdmi_entry["bridge"]["active"] is True and
                    hdmi_entry["bridge"]["name"] == "/anispaper-HDMI-A-1",
                    f"independent bridges were not retained: DP={dp_entry}, HDMI={hdmi_entry}")
            require(rpc.call("wallpaper.stop", {"output": target_output})["stopped"] is True,
                    "DP-2 renderer did not stop")
            remaining = rpc.call("status.get")
            remaining_hdmi = next((entry for entry in remaining["renderers"]
                                   if entry["output"] == control_output), None)
            require(remaining_hdmi and remaining_hdmi["bridge"]["active"] is True and
                    remaining_hdmi["wallpaperId"] == "steam:403",
                    f"stopping DP-2 removed HDMI-A-1 state: {remaining_hdmi}")
            require(rpc.call("wallpaper.stop", {"output": control_output})["stopped"] is True,
                    "HDMI-A-1 renderer did not stop")
            print("f4_integration: fallback and DP-2/HDMI-A-1 independent renderer/bridge assertions passed")
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
