#!/usr/bin/env python3
"""Regression contract: fullscreen alone must not pause scene wallpapers."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ISOLATED = (ROOT / "src/renderers/isolated_renderer.cpp").read_text(encoding="utf-8")
MANAGER = (ROOT / "src/renderers/renderer_manager.cpp").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require('ANISPAPER_SCENE_NO_FULLSCREEN_PAUSE' in ISOLATED,
        "scene child still relies on generic fullscreen pause")
require('!steamAppId.isEmpty() && validSteamAppId(steamAppId) && knownGameRuntime' in MANAGER,
        "SteamAppId detection is missing an executable/runtime evidence gate")
require('isNonGameHelper' in MANAGER,
        "gaming mode still treats python/node/Steam helpers as games")
print("game_pause_contract: fullscreen pause is delegated to evidence-based gaming mode")
