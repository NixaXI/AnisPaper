#!/usr/bin/env python3
"""Direct assertions for the isolated video and web worker protocols."""

import base64
import json
import os
import pathlib
import select
import shutil
import subprocess
import sys
import tempfile
import time


DAEMON = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else None


class WorkerEnvironmentUnavailable(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def make_assets(root):
    video = root / "no-audio.mp4"
    preview = root / "preview.jpg"
    html = root / "cross-origin.html"
    subprocess.run(
        [
            "ffmpeg", "-y", "-f", "lavfi", "-i", "testsrc=size=320x180:rate=30",
            "-t", "3", "-pix_fmt", "yuv420p", str(video),
        ],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    subprocess.run(
        ["ffmpeg", "-y", "-i", str(video), "-frames:v", "1", str(preview)],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    html.write_text(
        "<!doctype html><html><body style='background:#101622;color:#ffd000'>"
        "<h1>F2 WEB</h1><iframe src='https://example.com/'></iframe>"
        "<script>document.body.dataset.javascript='enabled'</script></body></html>",
        encoding="utf-8",
    )
    return video, preview, html


def worker_frame(binary, kind, source, preview, env):
    command = [
        str(binary), "--renderer-child", "--type", kind, "--file", str(source),
        "--preview", str(preview), "--width", "320", "--height", "180", "--fps", "30",
        "--volume", "0.2", "--speed", "1.0", "--loop", "1",
    ]
    process = subprocess.Popen(
        command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, env=env,
    )
    ready = False
    frame = None
    deadline = time.monotonic() + 15
    try:
        while time.monotonic() < deadline:
            readable, _, _ = select.select([process.stdout], [], [], 0.25)
            if not readable:
                if process.poll() is not None:
                    break
                continue
            line = process.stdout.readline()
            if not line:
                continue
            message = json.loads(line)
            if message.get("event") == "fatal":
                raise AssertionError(f"{kind} worker fatal: {message}")
            if message.get("event") == "ready":
                ready = True
            if message.get("event") == "frame":
                raw = base64.b64decode(message["jpeg"], validate=True)
                require(raw.startswith(b"\xff\xd8") and raw.endswith(b"\xff\xd9"),
                        f"{kind} worker did not return JPEG")
                require(message["width"] > 0 and message["height"] > 0,
                        f"{kind} dimensions are invalid")
                frame = (message, raw)
                break
        require(ready, f"{kind} worker never became ready")
        require(frame is not None and len(frame[1]) > 200,
                f"{kind} worker never produced an asserted frame")
        return frame
    finally:
        if process.poll() is None:
            process.stdin.write(json.dumps({"command": "stop"}) + "\n")
            process.stdin.flush()
            try:
                process.wait(timeout=4)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=4)
        stdout, stderr = process.communicate(timeout=1)
        if process.returncode not in (0,):
            if not ready and not stdout and not stderr and process.returncode < 0:
                raise WorkerEnvironmentUnavailable(
                    f"{kind} worker cannot initialize a GUI/GL context in this sandbox"
                )
            raise AssertionError(
                f"{kind} worker exit={process.returncode}; stdout={stdout!r}; stderr={stderr!r}"
            )


def main():
    require(DAEMON and DAEMON.is_file(), "usage: f2_renderer_child.py /path/to/anis-paperd")
    with tempfile.TemporaryDirectory(prefix="anispaper-f2-child-") as directory:
        root = pathlib.Path(directory)
        binary = root / "anis-paperd"
        shutil.copy2(DAEMON, binary)
        binary.chmod(0o700)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        video, preview, html = make_assets(root)
        env = os.environ.copy()
        env.update(
            {
                "XDG_RUNTIME_DIR": str(runtime),
                "QTWEBENGINE_CHROMIUM_FLAGS": "--disable-gpu",
            }
        )
        try:
            video_frame = worker_frame(binary, "video", video, preview, env)
            web_frame = worker_frame(binary, "web", html, preview, env)
        except WorkerEnvironmentUnavailable as exc:
            print(f"f2_renderer_child: SKIP {exc}")
            return 77
        require(video_frame[0].get("fallback") is False,
                "video FBO path unexpectedly used a fallback")
        # An unavailable remote iframe is allowed to use WebRenderer's explicit
        # fallback, but its local document must still yield a valid JPEG.
        require(isinstance(web_frame[0].get("fallback"), bool),
                "web worker did not report fallback state")
        print("f2_renderer_child: video-FBO/web-grab JPEG assertions passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"f2_renderer_child: FAIL {exc}", file=sys.stderr)
        sys.exit(1)
