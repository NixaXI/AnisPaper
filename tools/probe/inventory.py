#!/usr/bin/env python3
"""Non-destructive F0 environment inventory.

Only paths, availability flags, counts, and public project metadata types are
reported. The parser never evaluates VDF content and API-key values are never
read or emitted.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from collections import Counter
from pathlib import Path
from typing import Any


MAX_PROJECT_JSON_BYTES = 5 * 1024 * 1024


def command_result(command: list[str], timeout: int = 5) -> dict[str, Any]:
    try:
        completed = subprocess.run(command, stdin=subprocess.DEVNULL, text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   timeout=timeout, check=False)
    except FileNotFoundError:
        return {"status": "not-found", "command": command}
    except subprocess.TimeoutExpired:
        return {"status": "timeout", "command": command}
    output = (completed.stdout + completed.stderr).strip()
    return {"status": "ok" if completed.returncode == 0 else "failed",
            "command": command, "returncode": completed.returncode,
            "output": output[:1200]}


def vdf_tokens(source: str) -> list[str]:
    """Tokenize Valve KeyValues safely: strings/braces/comments only, no eval."""
    tokens: list[str] = []
    position = 0
    token = re.compile(r'''\s*(?://[^\n]*(?:\n|$)|"((?:\\.|[^"\\])*)"|([{}]))''')
    while position < len(source):
        match = token.match(source, position)
        if match is None:
            # Treat malformed remainder as data, never as shell syntax.
            break
        position = match.end()
        if match.group(1) is not None:
            tokens.append(bytes(match.group(1), "utf-8").decode("unicode_escape"))
        elif match.group(2) is not None:
            tokens.append(match.group(2))
    return tokens


def steam_libraries(vdf_path: Path) -> tuple[list[Path], str | None]:
    if not vdf_path.is_file():
        return [], "libraryfolders.vdf not found"
    try:
        source = vdf_path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        return [], f"cannot read libraryfolders.vdf: {error}"
    tokens = vdf_tokens(source)
    libraries: list[Path] = []
    for index in range(len(tokens) - 1):
        if tokens[index].lower() == "path" and tokens[index + 1] not in {"{", "}"}:
            candidate = Path(tokens[index + 1]).expanduser()
            if candidate.is_absolute() and candidate not in libraries:
                libraries.append(candidate)
    return libraries, None


def project_inventory(libraries: list[Path]) -> tuple[list[dict[str, str]], Counter[str], list[str]]:
    projects: list[dict[str, str]] = []
    types: Counter[str] = Counter()
    errors: list[str] = []
    for library in libraries:
        content = library / "steamapps" / "workshop" / "content" / "431960"
        try:
            entries = sorted(content.iterdir()) if content.is_dir() else []
        except OSError as error:
            errors.append(f"{content}: {error}")
            continue
        for entry in entries:
            project = entry / "project.json"
            if not project.is_file():
                continue
            try:
                if project.stat().st_size > MAX_PROJECT_JSON_BYTES:
                    errors.append(f"{project}: skipped (> {MAX_PROJECT_JSON_BYTES} bytes)")
                    continue
                parsed = json.loads(project.read_text(encoding="utf-8", errors="replace"))
                wallpaper_type = str(parsed.get("type", "missing")) if isinstance(parsed, dict) else "invalid-root"
            except (OSError, json.JSONDecodeError) as error:
                errors.append(f"{project}: {error}")
                wallpaper_type = "unreadable"
            types[wallpaper_type] += 1
            projects.append({"library": str(library), "workshop_id": entry.name, "type": wallpaper_type})
    return projects, types, errors


def first_libraryfolder_file(home: Path) -> Path:
    native = home / ".local" / "share" / "Steam" / "steamapps" / "libraryfolders.vdf"
    if native.exists():
        return native
    return home / ".steam" / "steam" / "steamapps" / "libraryfolders.vdf"


def availability() -> dict[str, Any]:
    binaries = {name: shutil.which(name) is not None for name in
                ("mpv", "v4l2-ctl", "pkexec", "steamcmd", "linux-wallpaperengine", "wayland-info", "grim", "spectacle")}
    libmpv = any(Path(candidate).exists() for candidate in
                 ("/usr/lib/libmpv.so", "/usr/lib64/libmpv.so", "/usr/local/lib/libmpv.so"))
    wallpaperengine_lib = any(
        any(Path(directory).glob("*wallpaperengine*.so*"))
        for directory in ("/usr/lib", "/usr/lib64", "/usr/local/lib")
        if Path(directory).is_dir()
    )
    v4l2 = command_result(["modinfo", "v4l2loopback"])
    return {"binaries": binaries, "libmpv": libmpv,
            "libwallpaperengine": wallpaperengine_lib,
            "v4l2loopback_module": v4l2}


def collect() -> dict[str, Any]:
    home = Path.home()
    vdf = first_libraryfolder_file(home)
    libraries, steam_error = steam_libraries(vdf)
    projects, types, project_errors = project_inventory(libraries)
    api_configured = any(bool(os.environ.get(name)) for name in
                         ("ANISPAPER_STEAM_API_KEY", "STEAM_WEB_API_KEY"))
    return {
        "session": {
            "XDG_SESSION_TYPE": os.environ.get("XDG_SESSION_TYPE", "unset"),
            "XDG_CURRENT_DESKTOP": os.environ.get("XDG_CURRENT_DESKTOP", "unset"),
            "WAYLAND_DISPLAY": "configured" if os.environ.get("WAYLAND_DISPLAY") else "unset",
            "kwin_wayland": command_result(["kwin_wayland", "--version"]),
            "wayland_info": command_result(["wayland-info"]),
        },
        "steam": {
            "libraryfolders_vdf": str(vdf),
            "libraries": [str(path) for path in libraries],
            "parse_error": steam_error,
            "project_count": len(projects),
            "project_types": dict(sorted(types.items())),
            "project_errors": project_errors,
            "steamcmd": shutil.which("steamcmd") is not None,
            "anonymous_workshop_test": "not attempted: steamcmd unavailable" if shutil.which("steamcmd") is None else "not attempted: requires explicit approved bounded test",
            "api_key": "configured" if api_configured else "not configured",
        },
        "dependencies": availability(),
    }


def text_report(data: dict[str, Any]) -> str:
    lines = ["ANISPAPER_F0_INVENTORY version=1"]
    session = data["session"]
    lines.append(f"session.type={session['XDG_SESSION_TYPE']}")
    lines.append(f"session.desktop={session['XDG_CURRENT_DESKTOP']}")
    lines.append(f"session.wayland_display={session['WAYLAND_DISPLAY']}")
    for name in ("kwin_wayland", "wayland_info"):
        result = session[name]
        lines.append(f"session.{name}.status={result['status']}")
        if result.get("output"):
            lines.append(f"session.{name}.output={result['output'].replace(chr(10), ' | ')}")
    steam = data["steam"]
    lines.append(f"steam.libraryfolders_vdf={steam['libraryfolders_vdf']}")
    lines.append(f"steam.library_count={len(steam['libraries'])}")
    for index, library in enumerate(steam["libraries"]):
        lines.append(f"steam.library.{index}={library}")
    lines.append(f"steam.project_json_count={steam['project_count']}")
    lines.append("steam.project_types=" + json.dumps(steam["project_types"], sort_keys=True))
    lines.append(f"steam.steamcmd={'present' if steam['steamcmd'] else 'absent'}")
    lines.append(f"steam.anonymous_workshop_test={steam['anonymous_workshop_test']}")
    lines.append(f"steam.api_key={steam['api_key']}")
    if steam["parse_error"]:
        lines.append(f"steam.parse_error={steam['parse_error']}")
    for error in steam["project_errors"]:
        lines.append(f"steam.project_error={error}")
    dependencies = data["dependencies"]
    for name, present in sorted(dependencies["binaries"].items()):
        lines.append(f"dependency.binary.{name}={'present' if present else 'absent'}")
    lines.append(f"dependency.libmpv={'present' if dependencies['libmpv'] else 'absent'}")
    lines.append(f"dependency.libwallpaperengine={'present' if dependencies['libwallpaperengine'] else 'absent'}")
    module = dependencies["v4l2loopback_module"]
    lines.append(f"dependency.v4l2loopback.status={module['status']}")
    if module.get("output"):
        lines.append("dependency.v4l2loopback.output=" + module["output"].replace("\n", " | "))
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="emit structured JSON instead of key=value text")
    arguments = parser.parse_args()
    data = collect()
    print(json.dumps(data, indent=2, sort_keys=True) if arguments.json else text_report(data))
    return 0


if __name__ == "__main__":
    sys.exit(main())
