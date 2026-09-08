#!/usr/bin/env python3
"""Contract: user Gaming Mode blacklist (e.g. Sober) is honored end to end."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DAEMON = (ROOT / "src/daemon/main.cpp").read_text(encoding="utf-8")
MANAGER = (ROOT / "src/renderers/renderer_manager.cpp").read_text(encoding="utf-8")
MANAGER_H = (ROOT / "src/renderers/renderer_manager.h").read_text(encoding="utf-8")
RPC_H = (ROOT / "src/ui/rpc_client.h").read_text(encoding="utf-8")
RPC_CPP = (ROOT / "src/ui/rpc_client.cpp").read_text(encoding="utf-8")
BRIDGE = (ROOT / "src/ui/web/bridge.js").read_text(encoding="utf-8")
INDEX = (ROOT / "src/ui/web/index.html").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


# Settings plumbing: stored, validated, exposed over settings.get/set.
require("gamingBlacklist" in DAEMON,
        "daemon settings have no gamingBlacklist field")
require("parseGamingBlacklist" in DAEMON,
        "daemon does not validate the gaming blacklist")
require('"gamingBlacklist"' in DAEMON,
        "gamingBlacklist is not accepted by settings.set")
require("setGamingBlacklist" in DAEMON,
        "daemon never forwards the blacklist to the renderer manager")

# Detection: blacklist consulted for non-skipped processes only.
require("setGamingBlacklist" in MANAGER_H,
        "renderer manager has no blacklist setter")
require("gaming blacklist pattern" in MANAGER,
        "detection does not report blacklist matches")
require("steamGameRunning(gamingBlacklist_" in MANAGER,
        "detection does not consult the configured blacklist")

# UI: settings sheet field wired to the daemon and back.
require("gamingBlacklist" in RPC_H and "gamingBlacklistChanged" in RPC_H,
        "RpcClient does not expose the gaming blacklist")
require("setGamingBlacklist" in RPC_CPP,
        "RpcClient cannot save the gaming blacklist")
require('id="gamingBlacklist"' in INDEX,
        "settings sheet has no blacklist field")
require("anisOnBlacklist" in BRIDGE,
        "UI cannot send the blacklist to the daemon")
print("gaming_blacklist_contract: user patterns force Gaming Mode via settings")
