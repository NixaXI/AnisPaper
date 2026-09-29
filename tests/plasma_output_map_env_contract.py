#!/usr/bin/env python3
"""Ensure output-map follows the real session and has both display backends."""

from pathlib import Path
import sys


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) == 2 else Path(__file__).parents[1]
    activator = (root / "src/daemon/plasma_wallpaper_activator.cpp").read_text(encoding="utf-8")
    helper = (root / "src/daemon/plasma_output_map_helper.cpp").read_text(encoding="utf-8")
    wrapper = (root / "packaging/systemd/anis-paperd-session-wrapper").read_text(encoding="utf-8")
    required = {
        "activator": ("QProcessEnvironment::systemEnvironment()",
                      "process.setProcessEnvironment(environment)",
                      'QStringLiteral("xcb")', 'QStringLiteral("wayland")',
                      'QStringLiteral("WAYLAND_DISPLAY")'),
        "helper": ("rawX11OutputOrder", "_KDE_SCREEN_INDEX", "rawKWinOutputOrder"),
        "session wrapper": ("ANISPAPER_SESSION_TYPE", "XDG_SESSION_TYPE", "unset WAYLAND_DISPLAY"),
    }
    sources = {"activator": activator, "helper": helper, "session wrapper": wrapper}
    missing = [f"{name}: {token}" for name, tokens in required.items()
               for token in tokens if token not in sources[name]]
    if missing:
        print("plasma output-map environment contract: FAIL")
        for token in missing:
            print(f"- missing: {token}")
        return 1
    print("plasma output-map environment contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
