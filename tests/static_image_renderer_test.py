#!/usr/bin/env python3
"""Propagate the C++ static image renderer assertions to CTest."""

import os
import pathlib
import shutil
import stat
import subprocess
import sys
import tempfile

BINARY = pathlib.Path(sys.argv[1]).resolve()

executable = str(BINARY)
# Build trees may live on filesystems without exec bits (e.g. NTFS mounts).
# Copy the helper binary to a temp dir and give it the executable bit there.
if not os.access(executable, os.X_OK):
    with tempfile.TemporaryDirectory(prefix="anispaper-test-") as tmp:
        copied = pathlib.Path(tmp) / BINARY.name
        shutil.copy2(executable, copied)
        copied.chmod(copied.stat().st_mode | stat.S_IXUSR)
        result = subprocess.run([str(copied)], capture_output=True, text=True, timeout=60)
else:
    result = subprocess.run([executable], capture_output=True, text=True, timeout=60)

sys.stdout.write(result.stdout)
sys.stderr.write(result.stderr)
if result.returncode != 0:
    sys.exit(1)
if "ALL TESTS PASSED" not in result.stdout:
    print("FAIL: missing success marker", file=sys.stderr)
    sys.exit(1)

