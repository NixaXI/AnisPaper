#!/usr/bin/env bash
# AnisPaper one-click local installer.
#
# Builds from source into $HOME/.local (no sudo for the install itself) and
# enables the user systemd service.  Dependency installation is attempted only
# on known distributions and can be skipped with ANISPAPER_SKIP_DEPS=1.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ANISPAPER_BUILD_DIR:-$REPO_ROOT/build}"
PREFIX="${ANISPAPER_PREFIX:-$HOME/.local}"
BUILD_TYPE="${ANISPAPER_BUILD_TYPE:-Release}"

log() { printf '\n\033[1;33m==>\033[0m %s\n' "$*"; }

detect_distro() {
  if [[ -r /etc/os-release ]]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    echo "${ID:-unknown}"
  else
    echo unknown
  fi
}

install_deps_arch() {
  sudo pacman -S --needed --noconfirm \
    base-devel cmake pkgconf \
    qt6-base qt6-declarative qt6-webengine \
    kauth \
    wayland \
    mpv libjpeg-turbo \
    mesa glew freeglut \
    sdl2 lz4 ffmpeg libpulse freetype2 dbus \
    libx11 libxrandr libxinerama libxcursor libxi libxxf86vm
}

install_deps_debian() {
  sudo apt-get update
  sudo apt-get install -y \
    build-essential cmake pkg-config ninja-build \
    qt6-base-dev qt6-declarative-dev qt6-webengine-dev \
    libkf6auth-dev \
    libwayland-dev wayland-protocols \
    libmpv-dev libjpeg-turbo8-dev \
    libglew-dev freeglut3-dev \
    libsdl2-dev liblz4-dev libpulse-dev libfreetype-dev libdbus-1-dev \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
    libxxf86vm-dev
}

main() {
  log "AnisPaper installer (repo: $REPO_ROOT)"

  local distro
  distro="$(detect_distro)"
  if [[ "${ANISPAPER_SKIP_DEPS:-0}" != "1" ]]; then
    case "$distro" in
      arch|cachyos|endeavouros|manjaro)
        log "Installing build dependencies (pacman)"
        install_deps_arch
        ;;
      debian|ubuntu|linuxmint|pop)
        log "Installing build dependencies (apt)"
        install_deps_debian
        ;;
      *)
        log "Unknown distro '$distro': skipping dependency install."
        log "If the CMake configure fails, install the packages listed in README.md."
        ;;
    esac
  else
    log "Skipping dependency installation (ANISPAPER_SKIP_DEPS=1)"
  fi

  log "Configuring ($BUILD_TYPE, prefix: $PREFIX)"
  cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX"

  log "Building ($(nproc) jobs) — the first Scene build can take a while"
  cmake --build "$BUILD_DIR" -j"$(nproc)"

  log "Installing into $PREFIX"
  cmake --install "$BUILD_DIR"

  log "Enabling the user daemon"
  systemctl --user daemon-reload
  systemctl --user enable --now anispaper.service

  cat <<EOF

AnisPaper installed.

  1. Select the "AnisPaper Frame" wallpaper type in Plasma (once).
  2. Launch "AnisPaper" (anis-paper-ui) and apply a wallpaper.
  3. Logs: journalctl --user -u anispaper.service -f

Re-run this script any time to update. Uninstall steps are in README.md.
EOF
}

main "$@"
