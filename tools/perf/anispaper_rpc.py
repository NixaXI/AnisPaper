#!/usr/bin/env python3
"""Tiny JSON-RPC client for AnisPaper's newline-delimited Unix socket."""
from __future__ import annotations
import argparse, json, os, socket, sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("method")
    ap.add_argument("params", nargs="?", default="{}")
    ap.add_argument("--socket", dest="socket_path", default=None)
    ns = ap.parse_args()
    runtime = os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
    path = ns.socket_path or os.path.join(runtime, "anispaper.sock")
    try:
        params = json.loads(ns.params)
    except json.JSONDecodeError as exc:
        print(f"invalid params JSON: {exc}", file=sys.stderr)
        return 2
    request = {"jsonrpc": "2.0", "id": 1, "method": ns.method, "params": params}
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(4.0)
            s.connect(path)
            s.sendall(json.dumps(request, separators=(",", ":")).encode() + b"\n")
            buf = b""
            while b"\n" not in buf:
                chunk = s.recv(65536)
                if not chunk:
                    raise RuntimeError("daemon closed socket")
                buf += chunk
        response = json.loads(buf.split(b"\n", 1)[0])
    except Exception as exc:
        print(f"RPC failed ({path}): {exc}", file=sys.stderr)
        return 3
    if "error" in response:
        print(json.dumps(response, ensure_ascii=False, indent=2))
        return 4
    print(json.dumps(response.get("result"), ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
