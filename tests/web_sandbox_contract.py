#!/usr/bin/env python3
"""Static contract: web wallpapers stay local-only and keep WebGL readable."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WEB = (ROOT / "src/renderers/web_renderer.cpp").read_text(encoding="utf-8")
CHILD = (ROOT / "src/renderers/renderer_child.cpp").read_text(encoding="utf-8")
ISOLATED = (ROOT / "src/renderers/isolated_renderer.cpp").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require("QWebEngineUrlRequestInterceptor" in WEB,
        "web renderer is missing a URL interceptor")
require("info.block(true)" in WEB,
        "web interceptor does not block non-local requests")
require("LocalContentCanAccessRemoteUrls" in WEB and
        "false" in WEB.split("LocalContentCanAccessRemoteUrls", 1)[1][:120],
        "web renderer still allows local pages to hit the network")
require("WebGLEnabled, true" in WEB,
        "web renderer disables WebGL")
require("__anispaperCapture" in WEB,
        "web renderer no longer readbacks wallpaper canvases")
require("preserveDrawingBuffer" in WEB,
        "WebGL canvases are not preserved for readback")
require('flags += "--disable-gpu"' not in CHILD and
        "flags += \"--disable-gpu\"" not in CHILD,
        "renderer child still forces --disable-gpu onto web wallpapers")
require("WebRTC" in CHILD,
        "web child chromium flags no longer disable WebRTC")
require('spec_.type == QStringLiteral("web")' in ISOLATED and
        "1920" in ISOLATED,
        "web children are no longer capped below 4K capture cost")
print("web_sandbox_contract: local-only interceptor, WebGL readback, no forced --disable-gpu")
