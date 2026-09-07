#!/usr/bin/env python3
"""Regression contract: fullscreen alone must not pause scene wallpapers."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ISOLATED = (ROOT / "src/renderers/isolated_renderer.cpp").read_text(encoding="utf-8")
MANAGER = (ROOT / "src/renderers/renderer_manager.cpp").read_text(encoding="utf-8")


BRIDGE = (ROOT / "src/ui/web/bridge.js").read_text(encoding="utf-8")
INDEX = (ROOT / "src/ui/web/index.html").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require('ANISPAPER_SCENE_NO_FULLSCREEN_PAUSE' in ISOLATED,
        "scene child still relies on generic fullscreen pause")
require('!steamAppId.isEmpty() && validSteamAppId(steamAppId) && knownGameRuntime' in MANAGER,
        "SteamAppId detection is missing an executable/runtime evidence gate")
require('isNonGameHelper' in MANAGER,
        "gaming mode still treats python/node/Steam helpers as games")
require('setGamingMode(on ? "auto" : "off")' in BRIDGE,
        "Gaming Mode switch off still maps to auto")
require("setGaming(on)" not in BRIDGE.split("gamingActiveChanged")[1][:400],
        "gamingActive still writes the Gaming Mode preference")
require("paintGamingActive" in INDEX and "paintGamingPref" in INDEX,
        "UI no longer separates gaming preference from live pause")
print("game_pause_contract: fullscreen pause is delegated to evidence-based gaming mode")
