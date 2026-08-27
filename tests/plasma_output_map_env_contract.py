#!/usr/bin/env python3
"""Ensure the boot mapping helper receives a Wayland Qt environment."""

from pathlib import Path
import sys


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) == 2 else Path(__file__).parents[1]
    source = (root / "src/daemon/plasma_wallpaper_activator.cpp").read_text(encoding="utf-8")
    required = (
        "QProcessEnvironment::systemEnvironment()",
        "process.setProcessEnvironment(environment)",
        'QStringLiteral("QT_QPA_PLATFORM")',
        'QStringLiteral("wayland")',
        'QStringLiteral("WAYLAND_DISPLAY")',
        'entryList(QStringList{QStringLiteral("wayland-*")',
    )
    missing = [token for token in required if token not in source]
    if missing:
        print("plasma output-map environment contract: FAIL")
        for token in missing:
            print(f"- missing: {token}")
        return 1
    print("plasma output-map environment contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
