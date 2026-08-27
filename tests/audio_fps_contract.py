#!/usr/bin/env python3
"""Static contract checks for the renderer audio/FPS policy.

These checks intentionally fail when the known hard mutes or the 30 FPS
defaults are reintroduced.  Runtime sink output is validated separately when
the desktop session is available.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VIDEO = (ROOT / "src/renderers/video_renderer.cpp").read_text(encoding="utf-8")
WEB = (ROOT / "src/renderers/web_renderer.cpp").read_text(encoding="utf-8")
SCENE = (ROOT / "src/scene_engine/main.cpp").read_text(encoding="utf-8")
ISOLATED = (ROOT / "src/renderers/isolated_renderer.cpp").read_text(encoding="utf-8")
DAEMON = (ROOT / "src/daemon/main.cpp").read_text(encoding="utf-8")
RENDERER = (ROOT / "src/renderers/renderer.h").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require('setOption("audio", "no")' not in VIDEO,
        "video renderer still hard-mutes libmpv")
require('setOption("audio", "auto")' in VIDEO,
        "video renderer does not explicitly enable libmpv audio")
require('"--silent"' not in SCENE,
        "scene child still passes the upstream --silent flag")
require('"--volume"' in ISOLATED,
        "scene child does not propagate the renderer volume")
require('PlaybackRequiresUserGesture' in WEB and
        'setAudioMuted' in WEB,
        "web renderer does not explicitly enable audible autoplay")
require('int fpsCap=60' in DAEMON,
        "daemon default FPS cap remains 30")
require('int fps = 60' in RENDERER,
        "renderer default FPS remains 30")
require('applyPlayback' in RENDERER,
        "renderer contract is missing live volume/FPS applyPlayback")
require('setPlaybackOptions' in DAEMON,
        "daemon settings.set does not push live volume/FPS to renderers")
MAIN = (ROOT / "src/ui/qml/Main.qml").read_text(encoding="utf-8")
require('volumePercent' in MAIN and 'fpsCap' in MAIN,
        "Qt UI does not expose volume and FPS controls")
print("audio_fps_contract: audio enabled, scene volume, web autoplay and 60 FPS defaults present")
