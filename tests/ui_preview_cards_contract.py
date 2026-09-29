#!/usr/bin/env python3
"""Static contract: catalog/workshop cards show the whole wallpaper preview.

Workshop preview.jpg files are almost always square. A 16:10 card with
object-fit:cover (and an opaque title bar) crops them to a top strip —
the opposite of Wallpaper Engine's installed grid, where each tile is
square and the full preview sits on a dark letterbox.
"""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HTML = (ROOT / "src/ui/web/index.html").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def rule_body(selector: str) -> str:
    pattern = re.escape(selector) + r"\{([^}]+)\}"
    match = re.search(pattern, HTML)
    require(match is not None, f"missing CSS rule for {selector}")
    return re.sub(r"\s+", "", match.group(1))


gcard = rule_body(".gcard")
require(
    "aspect-ratio:1/1" in gcard or "aspect-ratio:1 / 1" in gcard.replace(" ", ""),
    "catalog cards are not square (Wallpaper Engine tiles are 1:1)",
)
require(
    "aspect-ratio:16/10" not in gcard,
    "catalog cards still use a 16:10 crop that hides square Workshop previews",
)

thumb_img = rule_body(".gcard .thumb img")
require(
    "object-fit:contain" in thumb_img,
    "card thumbnails still cover-crop instead of showing the full wallpaper",
)
require(
    "object-fit:cover" not in thumb_img,
    "card thumbnails still cover-crop the preview",
)
require(
    "30%" not in thumb_img,
    "card thumbnails still pin object-position to the top 30% of the preview",
)

info = rule_body(".gcard .info")
require(
    "background:#101418" not in info,
    "opaque title bar still covers the bottom of the wallpaper preview",
)

require(
    "center 30%/cover" not in HTML,
    "filmstrip thumbs still use cover + top-30% crop",
)
require(
    re.search(r"center\s*/\s*contain", HTML) is not None,
    "filmstrip thumbs do not letterbox with background-size:contain",
)

print("ui_preview_cards_contract: square tiles, contain, overlay title, no 30% crop")
