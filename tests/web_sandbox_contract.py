#!/usr/bin/env python3
"""Static contract: web wallpapers stay local-only and render offscreen."""

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
require("webGLEnabled: true" in WEB,
        "web renderer disables WebGL")
require("QQuickRenderControl" in WEB and "setRenderTarget" in WEB,
        "web renderer no longer composites into its own offscreen target")
require("->grab()" not in WEB and "toDataURL" not in WEB,
        "web renderer went back to widget grabs or canvas readback")
require("innerWidth * innerHeight" in WEB,
        "web page may parse before Chromium knows the viewport size")
require("permission.deny()" in WEB or "grantFeaturePermission(origin, feature, false)" in WEB,
        "web renderer no longer denies page permission requests")
require("download.cancel()" in WEB,
        "web renderer no longer cancels downloads")
require("Math.min(960" not in WEB and "Math.min(540" not in WEB,
        "web capture still downscales the page below native size")
require("childWidth > 1280" not in ISOLATED and "spec.width > 1280" not in CHILD,
        "web child is still capped below the physical output")
require('flags += "--disable-gpu"' not in CHILD and
        "flags += \\\"--disable-gpu\\\"" not in CHILD,
        "renderer child still forces --disable-gpu onto web wallpapers")
require("WebRTC" in CHILD,
        "web child chromium flags no longer disable WebRTC")
print("web_sandbox_contract: local-only main frame, offscreen render control, native size, no forced --disable-gpu")
