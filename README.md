<div align="center">

<img src="assets/banner-anispaper.svg" alt="AnisPaper banner" width="100%" />

<br>

<img src="assets/anis-star.png" alt="Anis Star" width="280" />

# AnisPaper

**Live wallpapers for KDE Plasma 6** — without stuffing the heavy renderer inside `plasmashell`.

[![Platform](https://img.shields.io/badge/Platform-Linux-111111?style=for-the-badge&logo=linux&logoColor=FFD54A)](#)
[![Desktop](https://img.shields.io/badge/Desktop-KDE%20Plasma%206-1D99F3?style=for-the-badge&logo=kde&logoColor=white)](#)
[![Session](https://img.shields.io/badge/Session-Wayland-222222?style=for-the-badge)](#)
[![Scene](https://img.shields.io/badge/Scene-Working-FFD54A?style=for-the-badge)](#)
[![Video](https://img.shields.io/badge/Video-Working-FFD54A?style=for-the-badge)](#)
[![Web](https://img.shields.io/badge/Web-Working-FFD54A?style=for-the-badge)](#)

[Website](https://nixaxi.github.io/AnisPaper/) · [Releases](https://github.com/NixaXI/AnisPaper/releases) · [Issues](https://github.com/NixaXI/AnisPaper/issues)

</div>

---

## Demo

<div align="center">

<img src="assets/anispaper-demo.gif"
     alt="AnisPaper running a live wallpaper on KDE Plasma 6"
     width="900" />

</div>

> Experimental. Built for testers who care about Plasma staying alive when a wallpaper misbehaves.

---

## What it does

AnisPaper splits the stack so the shell does not do the expensive work:

| Piece | Role |
| --- | --- |
| **anis-paper-ui** | Qt Quick control room (no Electron) |
| **anis-paperd** | Daemon / catalog / monitor mapping |
| Isolated renderers | Scene · video · web |
| **AnisPaper Frame** | Thin Plasma plugin — shows frames via shared memory |

Heavy rendering stays in separate processes. Plasma mostly displays the current frame.

On the original setup, Wallpaper Engine Scene projects have hit about **58–60 FPS at 1080p** when the scene allows it.

```text
Wallpaper Engine Scene / Video / Web
        ↓
isolated renderer
        ↓
shared-memory transport
        ↓
AnisPaper Frame → KDE Plasma
```

---

## Status

| Feature | Status |
| --- | --- |
| KDE Plasma 6 / Wayland | Working |
| Multi-monitor | Working |
| Steam library discovery | Working |
| Wallpaper Engine **Scene** | Working (experimental) |
| **Video** (mpv, isolated) | Working |
| **Web** on the desktop | Working |
| Packaged installer / AppImage | In progress |
| Gaming Mode | Rough — see [open issues](https://github.com/NixaXI/AnisPaper/issues) |

AnisPaper does **not** ship Steam Workshop content or Wallpaper Engine proprietary assets. You need your own Steam + Wallpaper Engine install for Scene Workshop items.

---

## Install

### Packaged build

Check [Releases](https://github.com/NixaXI/AnisPaper/releases) for tagged archives (`v0.2.0` is a Linux x86_64 tarball). An **AppImage** is in the works.

### Build from source (Arch / CachyOS)

```bash
sudo pacman -S --needed \
  base-devel cmake pkgconf \
  qt6-base qt6-declarative qt6-webengine \
  kauth wayland \
  mpv libjpeg-turbo \
  mesa glew freeglut \
  sdl2 lz4 ffmpeg libpulse freetype2 dbus \
  libx11 libxrandr libxinerama libxcursor libxi libxxf86vm

git clone https://github.com/NixaXI/AnisPaper.git
cd AnisPaper

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"

cmake --build build -j"$(nproc)"
cmake --install build
```

Other distros: use CMake’s missing-package errors as the source of truth for package names.

### Start the daemon

```bash
systemctl --user daemon-reload
systemctl --user enable --now anispaper.service
systemctl --user status anispaper.service
```

Logs:

```bash
journalctl --user -u anispaper.service -f
```

### Use it

1. Open **AnisPaper** from the app menu, or run `anis-paper-ui`.
2. Desktop → Configure Desktop and Wallpaper → wallpaper type → **AnisPaper Frame**.
3. Pick a wallpaper in the AnisPaper UI and apply it to an output (`DP-1`, `HDMI-A-1`, …).

Local install lands under `~/.local` (binaries, Plasma plugin, user systemd unit). No `sudo` for that prefix.

---

## Steam / Wallpaper Engine

AnisPaper looks for `libraryfolders.vdf` in common places (`~/.steam/...`, `~/.local/share/Steam/...`) and can span multiple libraries.

If Steam lives somewhere odd:

```bash
systemctl --user edit anispaper.service
```

```ini
[Service]
Environment="ANISPAPER_STEAM_VDF=/path/to/steamapps/libraryfolders.vdf"
```

Then reload and restart the user service.

---

## Troubleshooting

| Symptom | Things to try |
| --- | --- |
| AnisPaper Frame missing | `ls ~/.local/share/plasma/wallpapers/org.anispaper.frame` — reopen wallpaper settings |
| Daemon won’t start | `systemctl --user status anispaper.service` + journal; `which anis-paperd` |
| Steam wallpapers missing | Confirm VDF path / `ANISPAPER_STEAM_VDF`; Workshop download finished |
| Scene `renderer unavailable` | Logs; `ls ~/.local/bin/anis-paper-scene-engine`; try another scene; re-download Workshop item |
| Noisy UI renderer errors | Known cleanup area — note if the wallpaper still draws |

When filing a bug, include distro, Plasma version, Wayland/X11, GPU/driver, outputs, wallpaper type/ID, and recent journal lines. Don’t attach copyrighted Workshop project files.

---

## Contributing

Bugs, patches, packaging help, and Plasma-edge cases are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md).

Prefer small, reviewable PRs. AI-assisted patches are fine if you say so and still own the diff (test it, no secrets, no blind dumps).

Help wanted especially around: more GPUs, Scene compatibility reports, packaging, and Gaming Mode edge cases.

---

## Uninstall

```bash
systemctl --user disable --now anispaper.service

rm -f ~/.local/bin/anis-paperd \
      ~/.local/bin/anis-paper-scene-engine \
      ~/.local/bin/anispaper-plasma-output-map
rm -rf ~/.local/share/plasma/wallpapers/org.anispaper.frame
rm -f ~/.local/share/systemd/user/anispaper.service
rm -rf ~/.local/libexec/anispaper ~/.local/share/anispaper

systemctl --user daemon-reload
```

---

## License / third-party

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The Scene path vendors/customizes [Almamu/linux-wallpaperengine](https://github.com/Almamu/linux-wallpaperengine) — keep those licenses intact.

Wallpaper Engine / Steam Workshop assets stay under their own terms and are **not** redistributed here. AnisPaper is independent and not affiliated with Valve, Steam, Wallpaper Engine, or KDE.

---

<div align="center">

**Make the desktop move. Keep Plasma alive.**

[Website](https://nixaxi.github.io/AnisPaper/) · [Releases](https://github.com/NixaXI/AnisPaper/releases) · [Issues](https://github.com/NixaXI/AnisPaper/issues)

</div>
