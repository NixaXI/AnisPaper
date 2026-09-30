#!/usr/bin/env python3
"""Exercise delayed environment import without touching a real user manager."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile

WRAPPER = Path(__file__).resolve().parents[1] / "packaging/systemd/anis-paperd-session-wrapper"


def scenario(name, initial, manager, delayed=False, sockets=1, success=True):
    with tempfile.TemporaryDirectory(prefix="anispaper-session-") as temp:
        root = Path(temp)
        runtime = root / "runtime"
        runtime.mkdir()
        binaries = root / "bin"
        binaries.mkdir()
        auth = runtime / "xauth_fixture"
        auth.write_text("cookie")
        manager = dict(manager, XDG_RUNTIME_DIR=str(runtime), XAUTHORITY=str(auth))
        metadata = root / "manager.json"
        metadata.write_text(json.dumps(manager))
        (binaries / "systemctl").write_text(
            "#!/usr/bin/env python3\nimport json,os,pathlib\n"
            "p=pathlib.Path(os.environ['MOCK_CALLS'])\n"
            "n=int(p.read_text())+1 if p.exists() else 1\np.write_text(str(n))\n"
            "d=json.loads(pathlib.Path(os.environ['MOCK_ENV']).read_text())\n"
            "if os.environ['MOCK_DELAY']=='1' and n<4:d.pop('DISPLAY',None)\n"
            "for k,v in d.items():print(k+'='+v)\n")
        for binary, body in (("sleep", "exit 0"), ("loginctl", "exit 1")):
            (binaries / binary).write_text("#!/bin/sh\n" + body + "\n")
        for binary in binaries.iterdir():
            binary.chmod(0o700)
        live = []
        for number in range(sockets):
            sock = socket.socket(socket.AF_UNIX)
            sock.bind(str(runtime / f"wayland-{number}"))
            live.append(sock)
        environment = {"PATH": f"{binaries}:/usr/bin:/bin", "XDG_RUNTIME_DIR": str(runtime),
                       "MOCK_ENV": str(metadata), "MOCK_CALLS": str(root / "calls"),
                       "MOCK_DELAY": "1" if delayed else "0", **initial}
        child = "import json,os;print(json.dumps({k:os.environ.get(k) for k in ('DISPLAY','WAYLAND_DISPLAY','QT_QPA_PLATFORM','ANISPAPER_SESSION_TYPE')}))"
        try:
            result = subprocess.run(["bash", str(WRAPPER), sys.executable, "-c", child],
                                    env=environment, text=True, capture_output=True, timeout=8)
        finally:
            for sock in live:
                sock.close()
        if not success:
            assert result.returncode != 0, (name, result.stdout, result.stderr)
            return
        assert result.returncode == 0, (name, result.stderr)
        values = json.loads(result.stdout)
        if delayed:
            assert int((root / "calls").read_text()) >= 4
        return values


late = scenario("late import", {}, {"XDG_SESSION_TYPE": "wayland", "WAYLAND_DISPLAY": "wayland-0", "DISPLAY": ":81"}, delayed=True)
assert late["DISPLAY"] == ":81" and late["QT_QPA_PLATFORM"] == "wayland"
native = scenario("no XWayland", {"XDG_SESSION_TYPE": "wayland", "WAYLAND_DISPLAY": "wayland-0"}, {"XDG_SESSION_TYPE": "wayland", "WAYLAND_DISPLAY": "wayland-0"})
assert not native["DISPLAY"] and native["QT_QPA_PLATFORM"] == "wayland"
x11 = scenario("X11 with stale Wayland socket", {"XDG_SESSION_TYPE": "x11", "DISPLAY": ":82"}, {"XDG_SESSION_TYPE": "wayland", "DISPLAY": ":99"})
assert x11["DISPLAY"] == ":82" and x11["QT_QPA_PLATFORM"] == "xcb" and x11["WAYLAND_DISPLAY"] is None
scenario("ambiguous sockets", {"XDG_SESSION_TYPE": "wayland"}, {"XDG_SESSION_TYPE": "wayland"}, sockets=2, success=False)
print("session_wrapper: PASS")
