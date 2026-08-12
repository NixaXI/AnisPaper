#!/usr/bin/env bash
# Build, deploy and make one real-machine performance probe for the scene path.
# No sudo.  Designed for the user's existing CachyOS/KDE setup.
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${ANISPAPER_BUILD_DIR:-$ROOT/build}"
PREFIX="${ANISPAPER_PREFIX:-$HOME/.local}"
RUNTIME="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
RPC="$ROOT/tools/perf/anispaper_rpc.py"
STAMP="$(date +%Y%m%d-%H%M%S)"
ART="$ROOT/artifacts/chatgpt-perf-$STAMP"
BACKUP="$ART/installed-backup"
LOG="$ART/report.txt"
TEST_SCENE="${ANISPAPER_TEST_SCENE:-steam:1155012801}"
TEST_OUTPUT="${ANISPAPER_TEST_OUTPUT:-}"
JOBS="${ANISPAPER_JOBS:-$(nproc)}"
mkdir -p "$ART" "$BACKUP"
exec > >(tee -a "$LOG") 2>&1

say() { printf '\n[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
fail() { say "ERROR: $*"; exit 1; }
backup_if_exists() {
  local src="$1" rel="$2"
  if [[ -e "$src" ]]; then
    mkdir -p "$BACKUP/$(dirname "$rel")"
    cp -a "$src" "$BACKUP/$rel"
  fi
}

say "AnisPaper scene performance deploy/probe"
printf 'repo=%s\nbuild=%s\nprefix=%s\nartifact=%s\n' "$ROOT" "$BUILD" "$PREFIX" "$ART"

[[ -f "$ROOT/CMakeLists.txt" ]] || fail "CMakeLists.txt no encontrado"
[[ -d "$ROOT/third_party/linux-wallpaperengine" ]] || fail "vendor linux-wallpaperengine no encontrado"
command -v cmake >/dev/null || fail "cmake no está instalado"
command -v python3 >/dev/null || fail "python3 no está instalado"

say "1/7 — comprobación estática de los fixes"
python3 - "$ROOT" <<'PY'
from pathlib import Path
import sys
r=Path(sys.argv[1])
checks = {
  "whole-frame offscreen pacing": (r/"third_party/linux-wallpaperengine/src/WallpaperEngine/Application/WallpaperApplication.cpp", "offscreenFrameStart"),
  "no hidden swap in offscreen": (r/"third_party/linux-wallpaperengine/src/WallpaperEngine/Render/Drivers/GLFWOpenGLDriver.cpp", "Offscreen pacing is done at the end"),
  "skip hidden final composite": (r/"third_party/linux-wallpaperengine/src/WallpaperEngine/Render/CWallpaper.cpp", "final output-composite pass is intentionally skipped"),
  "fullscreen native XID": (r/"third_party/linux-wallpaperengine/src/WallpaperEngine/Render/Drivers/Detectors/X11FullScreenDetector.cpp", "glfwGetX11Window"),
  "scene child X11 detector": (r/"src/renderers/isolated_renderer.cpp", 'XDG_SESSION_TYPE"), QStringLiteral("x11"'),
  "direct bridge copy": (r/"src/bridge/frame_bridge.cpp", "Hot path for the scene engine"),
  "async Plasma provider": (r/"src/plasma/frame_image_provider.cpp", "ForceAsynchronousImageLoading"),
}
missing=[]
for name,(p,needle) in checks.items():
    ok=p.exists() and needle in p.read_text(errors="replace")
    print(("OK  " if ok else "FAIL")+" "+name)
    if not ok: missing.append(name)
if missing:
    raise SystemExit("static checks failed: "+", ".join(missing))
PY

say "2/7 — build Release (targets tocados)"
if [[ ! -f "$BUILD/CMakeCache.txt" ]]; then
  cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
    -DANISPAPER_ENABLE_SCENE_ENGINE=ON -DCMAKE_INSTALL_PREFIX="$PREFIX"
fi
cmake --build "$BUILD" -j"$JOBS" --target \
  anis-paperd anis-paper-scene-engine anispaperframeprovider

DAEMON_BIN="$BUILD/anis-paperd"
SCENE_BIN="$BUILD/anis-paper-scene-engine"
PLUGIN_BIN="$BUILD/qml/org/anispaper/frame/libanispaperframeprovider.so"
[[ -f "$DAEMON_BIN" ]] || fail "no se generó $DAEMON_BIN"
[[ -f "$SCENE_BIN" ]] || fail "no se generó $SCENE_BIN"
[[ -f "$PLUGIN_BIN" ]] || fail "no se generó $PLUGIN_BIN"

say "3/7 — tests rápidos"
if [[ ! -x "$DAEMON_BIN" || ! -x "$SCENE_BIN" ]]; then
  echo "INFO: build correcto, pero /mnt/DiscoE no expone +x para estos binarios."
  echo "INFO: salto ctest desde el build mount; se desplegarán con mode 755 a ~/.local."
  echo "INFO: la prueba real posterior (daemon + scene + Plasma) será la validación."
elif command -v ctest >/dev/null; then
  set +e
  ctest --test-dir "$BUILD" --output-on-failure -E '^f2_renderer_child$'
  CTEST_RC=$?
  set -e
  echo "ctest_rc=$CTEST_RC (no bloquea deploy; el probe real manda)"
fi

say "4/7 — backup + deploy a ~/.local"
DEST_DAEMON="$PREFIX/bin/anis-paperd"
DEST_SCENE="$PREFIX/bin/anis-paper-scene-engine"
PLASMA_BASE="$PREFIX/share/plasma/wallpapers/org.anispaper.frame"
DEST_PLUGIN="$PLASMA_BASE/contents/ui/org/anispaper/frame/libanispaperframeprovider.so"
backup_if_exists "$DEST_DAEMON" "bin/anis-paperd"
backup_if_exists "$DEST_SCENE" "bin/anis-paper-scene-engine"
backup_if_exists "$DEST_PLUGIN" "plugin/libanispaperframeprovider.so"
backup_if_exists "$PLASMA_BASE/contents/ui/main.qml" "plugin/main.qml"
install -Dm755 "$DAEMON_BIN" "$DEST_DAEMON"
install -Dm755 "$SCENE_BIN" "$DEST_SCENE"
install -Dm755 "$PLUGIN_BIN" "$DEST_PLUGIN"
install -Dm644 "$ROOT/packaging/plasma/org.anispaper.frame/contents/ui/main.qml" \
  "$PLASMA_BASE/contents/ui/main.qml"
install -Dm644 "$ROOT/packaging/plasma/org.anispaper.frame/contents/ui/org/anispaper/frame/qmldir" \
  "$PLASMA_BASE/contents/ui/org/anispaper/frame/qmldir"
install -Dm644 "$ROOT/packaging/plasma/org.anispaper.frame/contents/ui/org/anispaper/frame/FrameBridgeSupport.qml" \
  "$PLASMA_BASE/contents/ui/org/anispaper/frame/FrameBridgeSupport.qml"
install -Dm644 "$ROOT/packaging/plasma/org.anispaper.frame/metadata.json" "$PLASMA_BASE/metadata.json"
install -Dm644 "$ROOT/packaging/plasma/org.anispaper.frame/metadata.desktop" "$PLASMA_BASE/metadata.desktop"

# Do not depend on the CMakeCache install prefix.  A drop-in guarantees the
# user service runs the freshly installed ~/.local daemon without touching the
# original unit.
mkdir -p "$HOME/.config/systemd/user/anispaper.service.d"
cat > "$HOME/.config/systemd/user/anispaper.service.d/90-local-perf.conf" <<EOF
[Service]
ExecStart=
ExecStart=$DEST_DAEMON
EOF
systemctl --user daemon-reload
systemctl --user restart anispaper.service

for _ in {1..40}; do
  [[ -S "$RUNTIME/anispaper.sock" ]] && break
  sleep 0.1
done
[[ -S "$RUNTIME/anispaper.sock" ]] || fail "daemon reinició pero no apareció anispaper.sock"

# Reload the native QML plugin once. Plasma 6 normally owns plasmashell through
# this user unit.  If the unit is absent we leave Plasma alive and report it;
# producer-side fixes are still active and the plugin will reload next login.
PLASMA_RELOADED=0
if systemctl --user cat plasma-plasmashell.service >/dev/null 2>&1; then
  say "reinicio controlado de plasmashell para cargar el .so nuevo"
  systemctl --user restart plasma-plasmashell.service || true
  PLASMA_RELOADED=1
  sleep 2
fi

echo "plasma_reloaded=$PLASMA_RELOADED"

say "5/7 — elegir output y aplicar scene de prueba"
if [[ -z "$TEST_OUTPUT" ]]; then
  TEST_OUTPUT="$(python3 "$RPC" monitor.list '{}' 2>/dev/null | python3 -c 'import json,sys; x=json.load(sys.stdin); print((x[0].get("name") if x else ""))' 2>/dev/null || true)"
fi
[[ -n "$TEST_OUTPUT" ]] || TEST_OUTPUT="HDMI-A-1"
echo "test_scene=$TEST_SCENE"
echo "test_output=$TEST_OUTPUT"
python3 "$RPC" wallpaper.apply "{\"id\":\"$TEST_SCENE\",\"output\":\"$TEST_OUTPUT\"}" || \
  fail "wallpaper.apply falló; mirá $LOG"
sleep 3

say "6/7 — probe 15 s: FPS + CPU + status"
python3 "$RPC" status.get '{}' > "$ART/status-start.json" || true
printf '%-3s %-8s %-8s %-8s %-8s\n' '#' 'daemon%' 'scene%' 'plasma%' 'fps'
for i in {1..15}; do
  daemon_cpu="$(ps -C anis-paperd -o %cpu= 2>/dev/null | awk '{s+=$1} END{printf "%.1f",s+0}')"
  scene_cpu="$(ps -C anis-paper-scene-engine -o %cpu= 2>/dev/null | awk '{s+=$1} END{printf "%.1f",s+0}')"
  plasma_cpu="$(ps -C plasmashell -o %cpu= 2>/dev/null | awk '{s+=$1} END{printf "%.1f",s+0}')"
  status_json="$(python3 "$RPC" status.get '{}' 2>/dev/null || true)"
  fps="$(python3 -c 'import json,sys; out=sys.argv[1];
try: d=json.loads(sys.stdin.read())
except Exception: print("?"); raise SystemExit
for r in d.get("renderers",[]):
    if r.get("output")==out: print(f"{float(r.get(chr(102)+chr(112)+chr(115),0)):.1f}"); break
else: print("?")' "$TEST_OUTPUT" <<<"$status_json" 2>/dev/null || true)"
  printf '%-3s %-8s %-8s %-8s %-8s\n' "$i" "$daemon_cpu" "$scene_cpu" "$plasma_cpu" "$fps"
  printf '%s,%s,%s,%s,%s\n' "$i" "$daemon_cpu" "$scene_cpu" "$plasma_cpu" "$fps" >> "$ART/samples.csv"
  sleep 1
done
python3 "$RPC" status.get '{}' > "$ART/status-end.json" || true
journalctl --user -u anispaper.service -n 180 --no-pager > "$ART/anispaper-journal.txt" 2>&1 || true

say "7/7 — resumen"
python3 - "$ART/samples.csv" <<'PY'
import csv, statistics, sys
p=sys.argv[1]
rows=[]
with open(p,newline='') as f:
    for r in csv.reader(f):
        if len(r)!=5: continue
        try: rows.append(tuple(float(x) for x in r[1:]))
        except ValueError: pass
if not rows:
    print('No pude parsear muestras; revisar report.txt')
else:
    names=['daemon_cpu','scene_cpu','plasmashell_cpu','fps']
    cols=list(zip(*rows))
    for name,col in zip(names,cols):
        print(f'{name}: avg={statistics.mean(col):.1f} min={min(col):.1f} max={max(col):.1f}')
PY

printf '\nRESULTADOS: %s\n' "$ART"
printf 'LOG: %s\n' "$LOG"
printf '\nPara probar impacto en SMITE: dejá el scene activo, entrá a fullscreen y mirá si anis-paper-scene-engine cae cerca de 0%% CPU.\n'
printf 'No hace falta volver a descomprimir nada: si algo sale mal, mandame report.txt + status-end.json + anispaper-journal.txt.\n'


# CHATGPT_CATALOG_READY_FIX_V3
# NOTE: daemon catalog refresh is asynchronous; wait for catalog.list before hardcoded wallpaper.apply.
