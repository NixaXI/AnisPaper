# Third-party notices

AnisPaper itself is licensed under GPL-3.0-or-later (see `LICENSE`).

It includes or integrates third-party open-source components.

Important components include:

- `linux-wallpaperengine` — Wallpaper Engine rendering foundation used by the Scene renderer.
  Upstream: https://github.com/Almamu/linux-wallpaperengine — License: GPL-3.0-or-later.
  It is statically linked into `anis-paper-scene-engine`, which is why this
  repository as a whole is distributed under GPL-3.0-or-later.
- GLFW
- dependencies vendored by `linux-wallpaperengine`, each retaining its own license file.

The authoritative license text for each bundled dependency is the license/copying/notice file stored alongside that dependency under `third_party/`.

Do not remove third-party license files or upstream attribution.
