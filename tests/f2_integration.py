#!/usr/bin/env python3
"""End-to-end F2 assertions against a disposable daemon and Steam fixture."""

import base64
import errno
import json
import os
import pathlib
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time


DAEMON = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else None


def assert_true(condition, message):
    if not condition:
        raise AssertionError(message)


def unix_socket_available(root):
    probe = root / "unix-probe.sock"
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.bind(str(probe))
    except OSError as exc:
        if exc.errno in (errno.EPERM, errno.EACCES, errno.EOPNOTSUPP):
            print(f"f2_integration: SKIP AF_UNIX unavailable: {exc}")
            return False
        raise
    finally:
        sock.close()
        probe.unlink(missing_ok=True)
    return True


class Rpc:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(0.5)
        self.sock.connect(str(path))
        self.buffer = b""
        self.next_id = 1
        self.notifications = []

    def close(self):
        self.sock.close()

    def _line(self, timeout):
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("JSON-RPC response timeout")
            self.sock.settimeout(remaining)
            data = self.sock.recv(65536)
            if not data:
                raise RuntimeError("daemon closed JSON-RPC socket")
            self.buffer += data
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line.decode("utf-8"))

    def call(self, method, params=None, timeout=5):
        identifier = self.next_id
        self.next_id += 1
        request = {"jsonrpc": "2.0", "id": identifier, "method": method}
        if params is not None:
            request["params"] = params
        self.sock.sendall(json.dumps(request, separators=(",", ":")).encode() + b"\n")
        deadline = time.monotonic() + timeout
        while True:
            message = self._line(max(0.01, deadline - time.monotonic()))
            if message.get("id") == identifier:
                return message
            if "method" in message:
                self.notifications.append(message)


def wait_for(label, predicate, timeout=8, interval=0.05):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = predicate()
        if last:
            return last
        time.sleep(interval)
    raise AssertionError(f"timeout waiting for {label}; last={last!r}")


def write_fixture(root):
    steam = root / "steam"
    content = steam / "steamapps/workshop/content/431960"
    video = content / "10"
    web = content / "20"
    video.mkdir(parents=True)
    web.mkdir(parents=True)
    clip = video / "clip.mp4"
    subprocess.run(
        [
            "ffmpeg",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "testsrc=size=320x180:rate=30",
            "-t",
            "3",
            "-pix_fmt",
            "yuv420p",
            str(clip),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    subprocess.run(
        ["ffmpeg", "-y", "-i", str(clip), "-frames:v", "1", str(video / "preview.jpg")],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    shutil.copy2(video / "preview.jpg", web / "preview.jpg")
    (video / "project.json").write_text(
        json.dumps(
            {
                "title": "F2 no-audio video",
                "type": "Video",
                "file": "clip.mp4",
                "preview": "preview.jpg",
                "general": {"properties": {"volume": 0.2, "speed": 1.0, "loop": True}},
            }
        ),
        encoding="utf-8",
    )
    (web / "index.html").write_text(
        "<!doctype html><html><body style='margin:0;background:#101622;color:#ffd000'>"
        "<h1 id='f2-web'>ANISPAPER F2 WEB</h1>"
        "<iframe src='https://example.com/' title='cross-origin'></iframe>"
        "<script>document.body.dataset.js='enabled'</script></body></html>",
        encoding="utf-8",
    )
    (web / "project.json").write_text(
        json.dumps(
            {
                "title": "F2 web with cross-origin iframe",
                "type": "Web",
                "file": "index.html",
                "preview": "preview.jpg",
                "general": {"properties": {}},
            }
        ),
        encoding="utf-8",
    )
    vdf = steam / "steamapps/libraryfolders.vdf"
    vdf.write_text(f'"libraryfolders" {{ "0" {{ "path" "{steam}" }} }}', encoding="utf-8")
    return vdf


def result(response):
    assert_true("error" not in response, f"unexpected RPC error: {response}")
    return response["result"]


def jpeg_bytes(rpc, output):
    response = rpc.call("preview.frame", {"output": output})
    payload = result(response)
    assert_true(payload["mimeType"] == "image/jpeg", f"wrong mime type: {payload}")
    data = base64.b64decode(payload["data"], validate=True)
    assert_true(data.startswith(b"\xff\xd8") and data.endswith(b"\xff\xd9"),
                "preview.frame did not return a complete JPEG")
    assert_true(payload["width"] > 0 and payload["height"] > 0,
                "preview dimensions are invalid")
    return payload, data


def renderer_status(rpc, output):
    status = result(rpc.call("status.get"))
    for entry in status.get("renderers", []):
        if entry.get("output") == output:
            return status, entry
    return status, None


def wait_preview(rpc, output, label):
    def ready():
        try:
            return jpeg_bytes(rpc, output)
        except (RuntimeError, TimeoutError, KeyError, AssertionError):
            return None

    return wait_for(label, ready, timeout=12, interval=0.08)


def wait_running_pid(rpc, output, old_pid=0, timeout=8):
    def running():
        _, entry = renderer_status(rpc, output)
        if not entry:
            return None
        pid = entry.get("pid", 0)
        if entry.get("safeMode") or pid <= 0 or pid == old_pid:
            return None
        return entry

    return wait_for(f"renderer {output} restart", running, timeout=timeout)


def main():
    assert_true(DAEMON and DAEMON.is_file(), "usage: f2_integration.py /path/to/anis-paperd")
    with tempfile.TemporaryDirectory(prefix="anispaper-f2-") as temp:
        root = pathlib.Path(temp)
        if not unix_socket_available(root):
            return 77
        binary = root / "anis-paperd"
        shutil.copy2(DAEMON, binary)
        binary.chmod(0o700)
        vdf = write_fixture(root)
        runtime = root / "runtime"
        config = root / "config"
        runtime.mkdir()
        config.mkdir()
        env = os.environ.copy()
        env.update(
            {
                "XDG_RUNTIME_DIR": str(runtime),
                "XDG_CONFIG_HOME": str(config),
                "ANISPAPER_STEAM_VDF": str(vdf),
                # Test-only scaling preserves production's literal 1/3/9
                # second policy while keeping this integration test bounded.
                "ANISPAPER_TEST_WATCHDOG_SCALE": "0.05",
                "QTWEBENGINE_CHROMIUM_FLAGS": "--disable-gpu",
            }
        )
        parent_runtime = os.environ.get("XDG_RUNTIME_DIR")
        if parent_runtime:
            env["ANISPAPER_RENDERER_RUNTIME_DIR"] = parent_runtime
        process = subprocess.Popen(
            [str(binary)], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )
        rpc = None
        try:
            socket_path = runtime / "anispaper.sock"
            wait_for("daemon socket", lambda: socket_path.exists(), timeout=5)
            rpc = Rpc(socket_path)
            result(rpc.call("events.subscribe"))

            def catalog_ready():
                status = result(rpc.call("status.get"))
                return status if not status["catalog"]["scanning"] else None

            ready = wait_for("catalog scan", catalog_ready, timeout=8)
            catalog = result(rpc.call("catalog.list"))
            by_id = {item["id"]: item for item in catalog}
            assert_true(by_id["steam:10"]["type"] == "video", "video type was not normalized")
            assert_true(by_id["steam:20"]["type"] == "web", "web type was not normalized")
            assert_true(ready["watchdog"]["count"] == 0, "fresh watchdog must start at zero")

            # Lifecycle + no-audio video: ffmpeg fixture contains only a video
            # stream.  A valid JPEG proves mpv's FBO/read-pixels path ran.
            result(rpc.call("wallpaper.apply", {"id": "steam:10", "output": "F2-VIDEO"}))
            _, video_jpeg = wait_preview(rpc, "F2-VIDEO", "video preview")
            assert_true(len(video_jpeg) > 200, "video JPEG is unexpectedly tiny")
            _, video_entry = renderer_status(rpc, "F2-VIDEO")
            assert_true(video_entry and video_entry["renderer"] == "video" and
                        not video_entry["safeMode"], f"video not active: {video_entry}")
            assert_true(video_entry["pid"] > 0, "isolated video child has no PID")

            # Replacing an active output and a rapid apply->stop->apply use the
            # same serialized manager path; only the newest worker may survive.
            result(rpc.call("wallpaper.apply", {"id": "steam:20", "output": "F2-VIDEO"}))
            _, web_replacement = wait_preview(rpc, "F2-VIDEO", "web replacement preview")
            assert_true(len(web_replacement) > 200, "replacement web JPEG is tiny")
            result(rpc.call("wallpaper.stop", {"output": "F2-VIDEO"}))
            result(rpc.call("wallpaper.apply", {"id": "steam:10", "output": "F2-VIDEO"}))
            wait_preview(rpc, "F2-VIDEO", "rapid video reapply")
            result(rpc.call("wallpaper.stop", {"output": "F2-VIDEO"}))
            assert_true(result(rpc.call("wallpaper.stop", {"output": "F2-NONE"}))[
                            "stopped"] is False,
                        "stopping a non-existent renderer must be a no-op")

            # Web is sampled at 30 fps in the child.  The external iframe may
            # load or use the explicit local fallback, but it must never crash
            # the daemon and preview must always stay a real JPEG.
            result(rpc.call("wallpaper.apply", {"id": "steam:20", "output": "F2-WEB"}))
            _, web_jpeg = wait_preview(rpc, "F2-WEB", "web preview")
            assert_true(len(web_jpeg) > 200, "web JPEG is unexpectedly tiny")
            _, web_entry = renderer_status(rpc, "F2-WEB")
            assert_true(web_entry and web_entry["renderer"] == "web" and
                        not web_entry["safeMode"], f"web not active: {web_entry}")

            # Crash immediately after the child is observed (before requiring a
            # preview): this covers a start-time crash.  Subsequent SIGKILLs
            # prove the 1s/3s/9s watchdog path and safe static replacement.
            result(rpc.call("wallpaper.apply", {"id": "steam:10", "output": "F2-CRASH"}))
            entry = wait_running_pid(rpc, "F2-CRASH")
            first_pid = entry["pid"]
            os.kill(first_pid, signal.SIGKILL)
            entry = wait_running_pid(rpc, "F2-CRASH", old_pid=first_pid, timeout=5)
            assert_true(entry["crashes"] == 1, f"first crash count mismatch: {entry}")
            second_pid = entry["pid"]
            os.kill(second_pid, signal.SIGKILL)
            entry = wait_running_pid(rpc, "F2-CRASH", old_pid=second_pid, timeout=5)
            assert_true(entry["crashes"] == 2, f"second crash count mismatch: {entry}")
            os.kill(entry["pid"], signal.SIGKILL)

            def safe_mode():
                status, candidate = renderer_status(rpc, "F2-CRASH")
                if not candidate or not candidate.get("safeMode"):
                    return None
                return status, candidate

            status, safe_entry = wait_for("safe mode after three crashes", safe_mode, timeout=6)
            assert_true(safe_entry["renderer"] == "static-image" and safe_entry["pid"] == 0,
                        f"safe fallback did not replace child: {safe_entry}")
            assert_true(status["watchdog"]["count"] == 3,
                        f"watchdog count mismatch: {status['watchdog']}")
            safe_payload, safe_jpeg = wait_preview(rpc, "F2-CRASH", "safe preview")
            assert_true(safe_payload["safeMode"] is True and len(safe_jpeg) > 200,
                        "safe renderer did not expose a static JPEG")
            safe_error = rpc.call("wallpaper.apply", {"id": "steam:10", "output": "F2-CRASH"})
            assert_true(safe_error.get("error", {}).get("code") == -32002 and
                        safe_error["error"]["message"] == "safe mode active",
                        f"safe-mode error contract failed: {safe_error}")

            notification_methods = {message.get("method") for message in rpc.notifications}
            assert_true("wallpaper.active" in notification_methods and
                        "wallpaper.stopped" in notification_methods and
                        "wallpaper.crashed" in notification_methods and
                        "wallpaper.safeMode" in notification_methods,
                        f"missing wallpaper event: {notification_methods}")
            print("f2_integration: lifecycle/video-no-audio/web-iframe/watchdog/safe-mode assertions passed")
            return 0
        finally:
            if rpc:
                rpc.close()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=4)
            stdout, stderr = process.communicate(timeout=1)
            if process.returncode not in (0, -signal.SIGTERM):
                print("daemon stdout:\n" + stdout, file=sys.stderr)
                print("daemon stderr:\n" + stderr, file=sys.stderr)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"f2_integration: FAIL {exc}", file=sys.stderr)
        sys.exit(1)
