#!/usr/bin/env python3
"""Ensure daemon shutdown logging can identify the next SIGTERM sender."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/daemon/main.cpp").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require("SA_SIGINFO" in SOURCE, "daemon signal handler does not request siginfo")
require("si_pid" in SOURCE and "si_uid" in SOURCE,
        "shutdown log does not record sender pid/uid")
require("ANISPAPER_DAEMON_SIGNAL" in SOURCE,
        "shutdown log marker is missing")
require("clock_gettime" in SOURCE,
        "shutdown log has no signal timestamp")
print("signal_shutdown_contract: SIGTERM sender instrumentation present")
