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
require("ResourceTypeMainFrame" in WEB,
        "web interceptor no longer pins the main document to the local project")
require("WebGLEnabled, true" in WEB,
        "web renderer disables WebGL")
require("__anispaperCapture" in WEB,
        "web renderer no longer readbacks wallpaper canvases")
require("preserveDrawingBuffer" in WEB,
        "WebGL canvases are not preserved for readback")
require("Math.max(tw / best.width, th / best.height)" in WEB,
        "web capture no longer cover-crops portrait Spine canvases")
require("Math.min(960" not in WEB and "Math.min(540" not in WEB,
        "web capture still downscales the page below native size")
require("childWidth > 1280" not in ISOLATED and "spec.width > 1280" not in CHILD,
        "web child is still capped below the physical output")
require('flags += "--disable-gpu"' not in CHILD and
        "flags += \\\"--disable-gpu\\\"" not in CHILD,
        "renderer child still forces --disable-gpu onto web wallpapers")
require("WebRTC" in CHILD,
        "web child chromium flags no longer disable WebRTC")
print("web_sandbox_contract: local-only main frame, cover capture, native size, no forced --disable-gpu")
