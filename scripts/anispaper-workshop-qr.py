#!/usr/bin/env python3
"""Detached Wallpaper Engine workshop QR downloader. Survives AnisPaper UI exit."""
from __future__ import annotations

import argparse
import os
import re
import select
import subprocess
import sys
import time
from pathlib import Path


def extract_qr(text: str) -> str | None:
    best = None
    best_rows = 0
    start = 0
    lowered = text
    while True:
        idx = lowered.find("sign in with this QR code:", start)
        if idx < 0:
            idx = lowered.lower().find("the qr code has changed:", start)
        if idx < 0:
            break
        lines = lowered[idx:].splitlines()[1:]
        rows: list[str] = []
        for ln in lines:
            ln = ln.replace("\r", "").rstrip()
            if any(c not in " \t█▀▄" for c in ln) and ln.strip() and "█" not in ln:
                break
            if "█" in ln or (rows and not ln.strip()):
                rows.append(ln)
        while rows and not rows[0].strip():
            rows.pop(0)
        while rows and not rows[-1].strip():
            rows.pop()
        if len(rows) > best_rows:
            indents = [len(r) - len(r.lstrip(" ")) for r in rows if r.strip()]
            indent = min(indents) if indents else 0
            trimmed = [r[indent:] if len(r) >= indent else r for r in rows]
            best = "\n".join(trimmed)
            best_rows = len(rows)
        start = idx + 1
    return best if best_rows >= 25 else None


def ascii_to_png(ascii_qr: str, png: Path) -> str | None:
    rows = ascii_qr.split("\n")
    width = (max(len(r) for r in rows) + 1) // 2
    height = len(rows)
    scale, pad = 8, 16
    try:
        from PIL import Image
    except ImportError:
        Image = None  # type: ignore
    raw = png.with_name("steam-qr-raw.png")
    if Image is not None:
        img = Image.new("RGB", (width * scale + pad * 2, height * scale + pad * 2), "white")
        px = img.load()
        for y, row in enumerate(rows):
            for mx in range(width):
                cx = mx * 2
                if cx < len(row) and row[cx] != " ":
                    for dy in range(scale):
                        for dx in range(scale):
                            px[pad + mx * scale + dx, pad + y * scale + dy] = (0, 0, 0)
        img.save(raw)
    else:
        return None
    try:
        out = subprocess.check_output(["zbarimg", "-q", str(raw)], text=True).strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    if not out.startswith("QR-Code:"):
        return None
    payload = out.split("QR-Code:", 1)[-1].strip()
    try:
        subprocess.check_call(
            ["qrencode", "-s", "12", "-m", "4", "-o", str(png), payload]
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        png.write_bytes(raw.read_bytes())
        return payload if payload.startswith("https://") else None
    try:
        subprocess.Popen(
            ["xdg-open", str(png)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
    except OSError:
        pass
    return payload if payload.startswith("https://") else None


def isolated_env(home: Path) -> dict[str, str]:
    env = os.environ.copy()
    for key in (
        "LD_LIBRARY_PATH",
        "LD_PRELOAD",
        "QT_PLUGIN_PATH",
        "QT_QPA_PLATFORM_PLUGIN_PATH",
        "QTWEBENGINEPROCESS_PATH",
        "STEAM_COMPAT_CLIENT_INSTALL_PATH",
        "SteamAppId",
        "SteamGameId",
        "SteamOverlayGameId",
    ):
        env.pop(key, None)
    home.mkdir(parents=True, exist_ok=True)
    (home / "run").mkdir(exist_ok=True)
    (home / "xdg-data").mkdir(exist_ok=True)
    (home / "xdg-config").mkdir(exist_ok=True)
    env["HOME"] = str(home)
    env["XDG_RUNTIME_DIR"] = str(home / "run")
    env["XDG_DATA_HOME"] = str(home / "xdg-data")
    env["XDG_CONFIG_HOME"] = str(home / "xdg-config")
    return env


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--id", required=True)
    parser.add_argument("--dest", required=True)
    parser.add_argument("--bin", required=True)
    parser.add_argument("--log", required=True)
    parser.add_argument("--png", required=True)
    parser.add_argument("--stop", required=True)
    parser.add_argument("--home", required=True)
    parser.add_argument("--link", required=True)
    args = parser.parse_args()

    dest = Path(args.dest)
    stop = Path(args.stop)
    png = Path(args.png)
    log_path = Path(args.log)
    link_path = Path(args.link)
    dest.mkdir(parents=True, exist_ok=True)
    png.parent.mkdir(parents=True, exist_ok=True)
    pid_path = log_path.with_name("qr.pid")
    pid_path.write_text(str(os.getpid()) + "\n")
    project = dest / "project.json"
    env = isolated_env(Path(args.home))
    last_qr = None
    viewer_started = False

    def start_viewer() -> None:
        nonlocal viewer_started
        if viewer_started:
            return
        viewer = Path(__file__).with_name("anispaper-qr-viewer.py")
        alt = Path(args.bin).parent / "anispaper-qr-viewer.py"
        script = viewer if viewer.is_file() else alt
        if not script.is_file():
            return
        try:
            subprocess.Popen(
                [sys.executable, str(script), str(png)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            viewer_started = True
        except OSError:
            pass

    def _term(_signum, _frame):
        raise SystemExit(0)

    import signal as signalmod
    signalmod.signal(signalmod.SIGTERM, _term)
    try:
        while not project.is_file() and not stop.is_file():
            with log_path.open("ab", buffering=0) as log:
                proc = subprocess.Popen(
                    [
                        args.bin,
                        "-app",
                        "431960",
                        "-pubfile",
                        args.id,
                        "-qr",
                        "-remember-password",
                        "-os",
                        "windows",
                        "-dir",
                        str(dest),
                        "-loginid",
                        "4319601",
                    ],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    env=env,
                    cwd=str(Path(args.bin).parent),
                )
                buf = b""
                while proc.poll() is None and not project.is_file() and not stop.is_file():
                    assert proc.stdout is not None
                    ready, _, _ = select.select([proc.stdout], [], [], 0.4)
                    if not ready:
                        continue
                    chunk = proc.stdout.read(4096)
                    if not chunk:
                        time.sleep(0.2)
                        continue
                    log.write(chunk)
                    buf += chunk
                    text = buf.decode("utf-8", "replace")
                    text = re.sub(r"\x1b\[[0-9;]*m", "", text)
                    art = extract_qr(text)
                    if art and art != last_qr:
                        last_qr = art
                        payload = ascii_to_png(art, png)
                    if payload:
                        link_path.write_text(payload + "\n")
                        start_viewer()
                leftover = b""
                if proc.stdout is not None:
                    leftover = proc.stdout.read() or b""
                if leftover:
                    log.write(leftover)
                if stop.is_file() or project.is_file():
                    if proc.poll() is None:
                        proc.terminate()
                        try:
                            proc.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            proc.kill()
                    break
                if proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
            time.sleep(1)
    finally:
        try:
            pid_path.unlink()
        except FileNotFoundError:
            pass
    return 0 if project.is_file() else 1


if __name__ == "__main__":
    sys.exit(main())
