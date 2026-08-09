#!/usr/bin/env python3
"""Small F1 JSON-lines client; intended only for local fixture verification."""
import argparse, json, socket

p = argparse.ArgumentParser()
p.add_argument("--socket", required=True)
p.add_argument("request", nargs="+", help="JSON-RPC objects")
a = p.parse_args()
s = socket.socket(socket.AF_UNIX)
s.connect(a.socket)
for text in a.request:
    raw = (text + "\n").encode()
    # Deliberate fragmentation validates the daemon's line accumulator.
    s.sendall(raw[:3]); s.sendall(raw[3:])
s.settimeout(2)
data = b""
try:
    while True:
        data += s.recv(65536)
        if data.count(b"\n") >= len(a.request): break
except socket.timeout:
    pass
for line in data.splitlines(): print(json.dumps(json.loads(line), sort_keys=True))
