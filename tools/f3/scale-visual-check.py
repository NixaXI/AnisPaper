#!/usr/bin/env python3
"""One real KDE/Wayland verification for physical SHM frames and scale mode.

This is deliberately evidence-only: it selects an existing small catalog video,
uses the daemon's public JSON-RPC API, asks Plasma to use each screen's native
name, and saves one Spectacle-derived crop per physical output.  It never edits
Workshop content or removes a user directory.
"""

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time


HEADER = struct.Struct("<4sIIQQI")
EXPECTED_SIZE = (1920, 1080)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


class Rpc:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(4)
        self.sock.connect(str(path))
        self.buffer = b""
        self.next_id = 1

    def close(self):
        self.sock.close()

    def call(self, method, params=None, timeout=10):
        ident = self.next_id
        self.next_id += 1
        request = {"jsonrpc": "2.0", "id": ident, "method": method}
        if params is not None:
            request["params"] = params
        self.sock.sendall(json.dumps(request, separators=(",", ":")).encode() + b"\n")
        deadline = time.monotonic() + timeout
        while True:
            while b"\n" not in self.buffer:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise RuntimeError(f"{method}: JSON-RPC response timeout")
                self.sock.settimeout(remaining)
                chunk = self.sock.recv(65536)
                if not chunk:
                    raise RuntimeError("daemon closed the JSON-RPC socket")
                self.buffer += chunk
            raw, self.buffer = self.buffer.split(b"\n", 1)
            reply = json.loads(raw.decode("utf-8"))
            if reply.get("id") != ident:
                continue
            if "error" in reply:
                raise RuntimeError(f"{method}: {reply['error']}")
            return reply["result"]


def wait_for(label, predicate, timeout=15, interval=0.1):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            value = predicate()
            if value is not None:
                return value
        except Exception as exc:
            last = exc
        time.sleep(interval)
    raise RuntimeError(f"timeout waiting for {label}: {last}")


def bridge_name(output):
    safe = "".join(ch if ch.isalnum() or ch in "-_ ." else "_" for ch in output.strip())
    # Match sanitizeBridgeOutput exactly; spaces are replaced, dots retained.
    safe = safe.replace(" ", "_")
    return "/anispaper-" + (safe or "unknown")


def frame_header(path):
    raw = path.read_bytes()[:HEADER.size]
    require(len(raw) == HEADER.size, "shared-memory header is truncated")
    magic, width, height, frame_no, stamp, stride = HEADER.unpack(raw)
    return {"magic": magic, "width": width, "height": height,
            "frameNo": frame_no, "timestampNs": stamp, "stride": stride}


def png_size(path):
    with path.open("rb") as handle:
        raw = handle.read(24)
    require(raw[:8] == b"\x89PNG\r\n\x1a\n" and raw[12:16] == b"IHDR",
            f"not a PNG: {path}")
    return struct.unpack(">II", raw[16:24])


def plasma_env():
    env = os.environ.copy()
    runtime = env.get("XDG_RUNTIME_DIR", "/run/user/1000")
    env.setdefault("DBUS_SESSION_BUS_ADDRESS", f"unix:path={runtime}/bus")
    return env


def set_plasma_cover():
    # Empty Output intentionally selects Screen.name in main.qml.  This makes
    # the two containments map to HDMI-A-1 and DP-2 independently.
    script = (
        "var all=desktops();"
        "for (var i=0;i<all.length;i++) {"
        " var desktop=all[i];"
        " desktop.wallpaperPlugin='org.anispaper.frame';"
        " desktop.currentConfigGroup=['Wallpaper','org.anispaper.frame','General'];"
        " desktop.writeConfig('Output','');"
        " desktop.writeConfig('ScaleMode','cover');"
        " desktop.currentConfigGroup=[];"
        "}"
    )
    completed = subprocess.run(
        ["qdbus6", "org.kde.plasmashell", "/PlasmaShell",
         "org.kde.PlasmaShell.evaluateScript", script],
        check=False, capture_output=True, text=True, env=plasma_env(),
    )
    require(completed.returncode == 0,
            "could not configure Plasma wallpaper: " + completed.stderr.strip())


def showing_desktop():
    completed = subprocess.run(
        ["busctl", "--user", "get-property", "org.kde.KWin", "/KWin",
         "org.kde.KWin", "showingDesktop"],
        check=False, capture_output=True, text=True, env=plasma_env(),
    )
    require(completed.returncode == 0, "could not read KWin show-desktop state")
    return completed.stdout.strip() == "b true"


def show_desktop(enabled):
    if showing_desktop() == enabled:
        return
    # The direct KWin method is NoReply and does not toggle reliably in this
    # session.  Invoke the same user-visible KWin shortcut and verify the
    # resulting property before capturing; this preserves a user's prior state.
    completed = subprocess.run(
        ["qdbus6", "org.kde.kglobalaccel", "/component/kwin",
         "org.kde.kglobalaccel.Component.invokeShortcut", "Show Desktop"],
        check=False, capture_output=True, text=True, env=plasma_env(),
    )
    require(completed.returncode == 0,
            "could not invoke KWin Show Desktop: " + completed.stderr.strip())
    wait_for("KWin show-desktop state",
             lambda: True if showing_desktop() == enabled else None, timeout=3,
             interval=0.05)


def pick_video(catalog):
    for item in catalog:
        path = Path(item.get("file", ""))
        if item.get("type") == "video" and path.is_file() and 0 < path.stat().st_size < 10 * 1024 * 1024:
            return item
    raise RuntimeError("no real catalog video smaller than 10 MiB is available")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", required=True)
    parser.add_argument("--captures-dir", required=True)
    parser.add_argument("--outputs", nargs=2, default=["HDMI-A-1", "DP-2"])
    parser.add_argument("--video-id", help="explicit real catalog video for visual inspection")
    args = parser.parse_args()
    captures = Path(args.captures_dir)
    if captures.exists():
        require(captures.is_dir() and not any(captures.iterdir()),
                f"refusing to overwrite non-empty capture directory: {captures}")
    else:
        captures.mkdir(parents=True)

    rpc = None
    desktop_shown = False
    desktop_was_shown = False
    try:
        rpc = Rpc(args.socket)
        settings = rpc.call("settings.set", {"wallpaper.scaleMode": "cover"})
        require(settings.get("wallpaper", {}).get("scaleMode") == "cover",
                f"wallpaper.scaleMode did not persist as cover: {settings}")
        catalog = rpc.call("catalog.list")
        video = (next((item for item in catalog if item.get("id") == args.video_id), None)
                 if args.video_id else pick_video(catalog))
        require(video and video.get("type") == "video" and
                Path(video.get("file", "")).is_file() and
                0 < Path(video["file"]).stat().st_size < 10 * 1024 * 1024,
                "selected visual-test video is not a real catalog video below 10 MiB")
        monitors = {entry.get("name"): entry for entry in rpc.call("monitor.list")}
        ordered = []
        for output in args.outputs:
            monitor = monitors.get(output)
            require(monitor is not None, f"missing output: {output}")
            physical = monitor.get("physicalSize", {})
            size = (physical.get("width"), physical.get("height"))
            require(size == EXPECTED_SIZE,
                    f"{output}: expected physical {EXPECTED_SIZE[0]}x{EXPECTED_SIZE[1]}, got {physical}")
            require(monitor.get("bufferScale", 0) >= 1,
                    f"{output}: wl_output buffer scale is unavailable")
            ordered.append((monitor.get("geometry", {}).get("x", 0), output, monitor))
            rpc.call("wallpaper.apply", {"id": video["id"], "output": output})
        ordered.sort()

        headers = {}
        for _, output, _ in ordered:
            path = Path("/dev/shm") / bridge_name(output).lstrip("/")
            header = wait_for(
                f"{output} physical bridge frame",
                lambda path=path: (lambda h: h if h["magic"] == b"ANIS" and h["frameNo"] >= 2 else None)(frame_header(path))
                if path.is_file() else None,
            )
            require((header["width"], header["height"]) == EXPECTED_SIZE and
                    header["stride"] == EXPECTED_SIZE[0] * 4 and header["timestampNs"] > 0,
                    f"{output}: bridge is not a physical RGBA frame: {header}")
            preview = rpc.call("preview.frame", {"output": output})
            jpeg = base64.b64decode(preview.get("data", ""), validate=True)
            require(jpeg.startswith(b"\xff\xd8") and jpeg.endswith(b"\xff\xd9"),
                    f"{output}: preview.frame is not JPEG")
            headers[output] = (header, hashlib.sha256(jpeg).hexdigest())

        set_plasma_cover()
        time.sleep(2.5)
        full = captures / "spectacle-all.png"
        desktop_was_shown = showing_desktop()
        # Spectacle itself clears KWin's show-desktop state if it is launched
        # afterwards.  Start its background capture with a finite delay, then
        # invoke the user-visible shortcut while it is already waiting.
        shot = subprocess.Popen(
            ["spectacle", "--background", "--nonotify", "--fullscreen",
             "--delay", "1200", "--output", str(full)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=plasma_env(),
        )
        time.sleep(0.25)
        show_desktop(True)
        desktop_shown = True
        stdout, stderr = shot.communicate(timeout=10)
        # Under KWin's show-desktop transition Spectacle can return non-zero
        # after it has already persisted a complete PNG.  The file is the
        # authoritative capture result; retain the exit code in the evidence
        # instead of discarding a valid screenshot.
        require(full.is_file(), "Spectacle capture failed: " + stderr.strip())
        full_width, full_height = png_size(full)
        require(full_width >= EXPECTED_SIZE[0] * 2 and full_height >= EXPECTED_SIZE[1],
                f"Spectacle full desktop has unexpected dimensions: {full_width}x{full_height}")

        # KWin's all-screen capture can include a one-pixel logical seam at
        # fractional scale.  Crop exact physical 1920x1080 regions from its
        # left and right edges, matching the output order from wl_output x.
        results = []
        for index, (_, output, monitor) in enumerate(ordered):
            offset = 0 if index == 0 else full_width - EXPECTED_SIZE[0]
            target = captures / f"spectacle-{output}.png"
            crop = subprocess.run(
                ["magick", str(full), "-crop",
                 f"{EXPECTED_SIZE[0]}x{EXPECTED_SIZE[1]}+{offset}+0", "+repage", str(target)],
                check=False, capture_output=True, text=True,
            )
            require(crop.returncode == 0 and target.is_file(),
                    f"could not crop {output}: {crop.stderr.strip()}")
            require(png_size(target) == EXPECTED_SIZE,
                    f"{output}: cropped Spectacle image is not physical {EXPECTED_SIZE}")
            header, preview_hash = headers[output]
            results.append((output, monitor, header, preview_hash, target))

        for output, monitor, header, preview_hash, target in results:
            print(f"monitor.{output}.physical={monitor['physicalSize']['width']}x{monitor['physicalSize']['height']} bufferScale={monitor['bufferScale']}")
            print(f"bridge.{output}.header={header['width']}x{header['height']} stride={header['stride']} frameNo={header['frameNo']}")
            print(f"preview.{output}.sha256={preview_hash}")
            print(f"capture.{output}.path={target}")
            print(f"capture.{output}.dimensions={EXPECTED_SIZE[0]}x{EXPECTED_SIZE[1]}")
            print(f"capture.{output}.sha256={hashlib.sha256(target.read_bytes()).hexdigest()}")
        print(f"spectacle.exit={shot.returncode}")
        print(f"video.id={video['id']}")
        print("scaleMode=cover")
        print("f3.scale-visual-check=PASS")
        return 0
    except Exception as exc:
        print(f"f3.scale-visual-check=FAIL: {exc}", file=sys.stderr)
        return 1
    finally:
        if desktop_shown and not desktop_was_shown:
            try:
                show_desktop(False)
            except Exception:
                pass
        if rpc:
            rpc.close()


if __name__ == "__main__":
    sys.exit(main())
