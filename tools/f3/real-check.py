#!/usr/bin/env python3
"""Single F3 evidence run: integration, live bridge, Plasma package, capture."""

import argparse
import base64
import hashlib
import json
import os
import pathlib
import socket
import struct
import subprocess
import sys
import time


HEADER = struct.Struct("<4sIIQQI")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def shm_name(output):
    safe = "".join(c if c.isalnum() or c in "-_." else "_" for c in output.strip())
    return "/anispaper-" + (safe or "unknown")


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
        request = {"jsonrpc": "2.0", "id": identifier, "method": method}
        if params is not None:
            request["params"] = params
        self.sock.sendall(json.dumps(request, separators=(",", ":")).encode() + b"\n")
        deadline = time.monotonic() + timeout
        while True:
            while b"\n" not in self.buffer:
                remain = deadline - time.monotonic()
                if remain <= 0:
                    raise RuntimeError(f"{method}: JSON-RPC response timeout")
                self.sock.settimeout(remain)
                chunk = self.sock.recv(65536)
                if not chunk:
                    raise RuntimeError("daemon closed JSON-RPC socket")
                self.buffer += chunk
            raw, self.buffer = self.buffer.split(b"\n", 1)
            reply = json.loads(raw.decode("utf-8"))
            if reply.get("id") != identifier:
                continue
            if "error" in reply:
                raise RuntimeError(f"{method}: {reply['error']}")
            return reply["result"]


def wait_for(label, callback, timeout=15, interval=0.1):
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


def frame_header(path):
    raw = path.read_bytes()[:HEADER.size]
    require(len(raw) == HEADER.size, "shared-memory header is truncated")
    magic, width, height, frame_no, timestamp, stride = HEADER.unpack(raw)
    return {"magic": magic, "width": width, "height": height,
            "frameNo": frame_no, "timestampNs": timestamp, "stride": stride}


def png_dimensions(path):
    with path.open("rb") as handle:
        header = handle.read(24)
    require(header[:8] == b"\x89PNG\r\n\x1a\n" and header[12:16] == b"IHDR",
            "Spectacle did not write a PNG")
    return struct.unpack(">II", header[16:24])


def run_integration(args):
    if not args.integration_daemon:
        return
    require(args.integration_plugin, "--integration-plugin is required with --integration-daemon")
    suite = pathlib.Path(__file__).resolve().parents[2] / "tests" / "f3_integration.py"
    completed = subprocess.run(
        [sys.executable, str(suite), args.integration_daemon, args.integration_plugin],
        check=False, capture_output=True, text=True,
    )
    if completed.stdout:
        print(completed.stdout, end="")
    if completed.returncode != 0:
        raise RuntimeError("F3 integration suite failed:\n" + completed.stderr)
    print("f3.integration=PASS")


def install_path():
    data_home = pathlib.Path(os.environ.get("XDG_DATA_HOME", pathlib.Path.home() / ".local/share"))
    return data_home / "plasma/wallpapers/org.anispaper.frame"


def activate_plasma(output):
    runtime = os.environ.get("XDG_RUNTIME_DIR", "/run/user/1000")
    env = os.environ.copy()
    env.setdefault("DBUS_SESSION_BUS_ADDRESS", f"unix:path={runtime}/bus")
    # Explicit configuration avoids relying on a compositor-specific mapping
    # between containment indexes and connector names. Every containment sees
    # a valid bridge; the F3 evidence output is the requested HDMI connector.
    script = (
        "var all=desktops();"
        "for (var i=0;i<all.length;i++) {"
        " var desktop=all[i];"
        " desktop.wallpaperPlugin='org.anispaper.frame';"
        " desktop.currentConfigGroup=['Wallpaper','org.anispaper.frame','General'];"
        f" desktop.writeConfig('Output',{json.dumps(output)});"
        " desktop.currentConfigGroup=[];"
        "}"
    )
    completed = subprocess.run(
        ["qdbus6", "org.kde.plasmashell", "/PlasmaShell",
         "org.kde.PlasmaShell.evaluateScript", script],
        check=False, capture_output=True, text=True, env=env,
    )
    require(completed.returncode == 0,
            "could not activate Plasma wallpaper: " + completed.stderr.strip())


def set_show_desktop(enabled):
    runtime = os.environ.get("XDG_RUNTIME_DIR", "/run/user/1000")
    env = os.environ.copy()
    env.setdefault("DBUS_SESSION_BUS_ADDRESS", f"unix:path={runtime}/bus")
    completed = subprocess.run(
        ["qdbus6", "org.kde.KWin", "/KWin", "org.kde.KWin.showDesktop",
         "true" if enabled else "false"],
        check=False, capture_output=True, text=True, env=env,
    )
    require(completed.returncode == 0,
            "could not toggle KWin show-desktop: " + completed.stderr.strip())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", required=True)
    parser.add_argument("--output", default="HDMI-A-1")
    parser.add_argument("--video-id")
    parser.add_argument("--integration-daemon")
    parser.add_argument("--integration-plugin")
    parser.add_argument("--capture")
    parser.add_argument("--show-desktop", action="store_true",
                        help="temporarily hide windows so desktop icons are visible in capture")
    args = parser.parse_args()
    capture = pathlib.Path(args.capture or f"/tmp/anispaper-f3-desktop-{os.getpid()}.png")
    require(not capture.exists(), f"refusing to overwrite existing capture: {capture}")
    require(install_path().joinpath("metadata.desktop").is_file() and
            install_path().joinpath("contents/ui/org/anispaper/frame/libanispaperframeprovider.so").is_file(),
            f"Plasma plugin is not installed under {install_path()}")

    rpc = None
    showing_desktop = False
    try:
        run_integration(args)
        rpc = Rpc(args.socket)
        catalog = rpc.call("catalog.list")
        selected = None
        if args.video_id:
            selected = next((item for item in catalog if item.get("id") == args.video_id), None)
        else:
            for item in catalog:
                file_path = pathlib.Path(item.get("file", ""))
                if item.get("type") == "video" and file_path.is_file() and \
                        0 < file_path.stat().st_size < 10 * 1024 * 1024:
                    selected = item
                    break
        require(selected and selected.get("type") == "video",
                "no real catalog video smaller than 10 MiB is available")
        monitors = rpc.call("monitor.list")
        require(args.output in {entry.get("name") for entry in monitors},
                f"requested output is unavailable: {args.output}")
        rpc.call("wallpaper.apply", {"id": selected["id"], "output": args.output})
        bridge_path = pathlib.Path("/dev/shm") / shm_name(args.output).lstrip("/")

        def bridge_ready():
            if not bridge_path.is_file():
                return None
            header = frame_header(bridge_path)
            if header["magic"] != b"ANIS" or header["frameNo"] < 2:
                return None
            return header

        header = wait_for("live bridge frame", bridge_ready)
        require(header["width"] > 0 and header["height"] > 0 and
                header["stride"] == header["width"] * 4 and header["timestampNs"] > 0,
                f"invalid bridge header: {header}")
        preview = rpc.call("preview.frame", {"output": args.output})
        jpeg = base64.b64decode(preview.get("data", ""), validate=True)
        require(jpeg.startswith(b"\xff\xd8") and jpeg.endswith(b"\xff\xd9"),
                "preview.frame did not return JPEG")
        activate_plasma(args.output)
        # Let Plasma discover the user package and render at least two timer ticks.
        time.sleep(2.2)
        if args.show_desktop:
            set_show_desktop(True)
            showing_desktop = True
            time.sleep(1.0)
        shot = subprocess.run(
            ["spectacle", "--background", "--nonotify", "--fullscreen",
             "--output", str(capture)],
            check=False, capture_output=True, text=True,
        )
        require(shot.returncode == 0 and capture.is_file(),
                "Spectacle capture failed (exit=%d, stdout=%r, stderr=%r)" %
                (shot.returncode, shot.stdout.strip(), shot.stderr.strip()))
        width, height = png_dimensions(capture)
        require(width > 0 and height > 0, "capture has invalid dimensions")
        status = rpc.call("status.get")
        entry = next((item for item in status.get("renderers", [])
                      if item.get("output") == args.output), None)
        require(entry and entry.get("bridge", {}).get("active") is True and
                entry["bridge"].get("frameNo", 0) >= header["frameNo"],
                f"renderer bridge status regressed: {entry}")
        print(f"video.id={selected['id']}")
        print(f"video.file_bytes={pathlib.Path(selected['file']).stat().st_size}")
        print(f"bridge.name={entry['bridge']['name']}")
        print(f"bridge.frame_no={entry['bridge']['frameNo']}")
        print(f"preview.sha256={hashlib.sha256(jpeg).hexdigest()}")
        print("plasma.wallpaper=org.anispaper.frame")
        print(f"capture.path={capture}")
        print(f"capture.dimensions={width}x{height}")
        print(f"capture.sha256={hashlib.sha256(capture.read_bytes()).hexdigest()}")
        print(f"capture.show_desktop={'true' if args.show_desktop else 'false'}")
        print("f3.real-check=PASS")
        return 0
    except Exception as exc:
        print(f"f3.real-check=FAIL: {exc}", file=sys.stderr)
        return 1
    finally:
        if showing_desktop:
            try:
                set_show_desktop(False)
            except Exception:
                pass
        if rpc:
            rpc.close()


if __name__ == "__main__":
    sys.exit(main())
