# 🌟 ANISPAPER · STAR EDITION — PROMPT DE EJECUCIÓN (Codex CLI)

> Wallpaper engine nativo para KDE Plasma (CachyOS/Arch) con tema ANIS STAR,
> compatible con TODOS los tipos de wallpaper de Wallpaper Engine, a prueba de
> crashes de plasma-shell. Modo: autónomo. Decide con este documento.

## 0. PRINCIPIO SAGRADO DE ARQUITECTURA
1. `plasma-shell` SOLO muestra frames estáticos/tontos servidos por nuestro daemon.
   NUNCA renderiza video/scene/web/app en su propio proceso.
2. Todo renderer vive en proceso aislado con watchdog + backoff; tras 3 crashes
   seguidos → "modo seguro" (frame estático del preview.jpg) + aviso en UI.
3. Los iconos/carpetas del escritorio DEBEN verse siempre por encima del wallpaper.
4. Nada de ventanas de video flotantes por encima del escritorio (el hack que el
   usuario odia).

## 1. STACK
- **UI**: Electron + React + TS + Tailwind (reutiliza el design system ANIS STAR de
  AnisIDE: colores #0A0D14/#101622/#FFD000/#00C2FF/#FF6B8B, chamfer, HUD, JetBrains
  Mono, avatar anis_star.webp).
- **Daemon `anis-paperd`**: C++20 + Qt6 (Core/Gui/OpenGL/WebEngine) + wayland-client
  (protocolo wlr-layer-shell) + xcb. JSON-RPC sobre socket unix
  `$XDG_RUNTIME_DIR/anispaper.sock`.
- **Plugin Plasma `org.anispaper.frame`**: QML + mínimo C++ que lee frames de SHM.
  Instalación a nivel USUARIO en `~/.local/share/plasma/wallpapers/` (SIN sudo).
- **systemd user unit** `anispaper.service` (autostart).

## 2. FASE 0 — PROBE DEL SISTEMA (obligatoria antes de diseñar F2/F3)
Ejecuta y documenta en `docs/probe.md`:
- Sesión: X11 o Wayland (`echo $XDG_SESSION_TYPE`).
- Versión de KWin y soporte de `zwlr_layer_shell_v1` (prueba real: crea una surface
  de prueba layer=background y verifica con captura si los iconos del escritorio
  quedan POR ENCIMA).
- Rutas Steam: parsea `libraryfolders.vdf` y
  `steamapps/workshop/content/431960/*/project.json` (AppID 431960).
- Disponibilidad de: `mpv`/libmpv, `linux-wallpaperengine` (lib y binario, AUR),
  `v4l2loopback`, `pkexec`.
- Con el probe, elige estrategia por tabla (§3) y justifícala en el memo.

## 3. TABLA DE ESTRATEGIAS (sesión × tipo)
| Sesión | Estrategia primaria | Fallback |
|---|---|---|
| Wayland/KWin | **Bridge SHM**: daemon renderiza offscreen → memfd por monitor → plugin Plasma QML muestra frames | layer-shell directo si el probe confirmó iconos visibles |
| X11 | **Directo**: ventana propia con `_NET_WM_WINDOW_TYPE_DESKTOP` (técnica xwinwrap, XCB reparent a root) | Bridge SHM |
| tipo `application` en Wayland | best-effort: ventana hija + reglas KWin (keep below, sin borde, todos los escritorios) con WARN en UI | marcar "no soportado en bridge" + modo seguro |

## 4. RENDERERS POR TIPO (daemon, todos offscreen → pixels RGBA)
- `video`: libmpv offscreen (mpv_render_context + FBO → read pixels). Props: volumen,
  velocidad, loop.
- `scene`: linkear `libwallpaperengine` si el probe la halló; si no, binario hijo con
  flag offscreen si existe; si no → modo seguro + badge "scene sin soporte nativo".
- `web`: QtWebEngine offscreen (QWebEngineView::grab a 30fps, JS habilitado,
  `--disable-gpu` si hace falta). Crash de Chromium NO mata al daemon.
- `application`: X11 reparent; Wayland best-effort (§3).
- Watchdog por hijo: restart backoff 1s/3s/9s; contador de crashes expuesto por socket.

## 5. PUENTE DE FRAMES (memfd por monitor)
```
Nombre shm: /anispaper-<output-sanitizado>
struct FrameHeader { u32 magic='ANIS'; u32 width; u32 height; u64 frameNo;
                     u64 timestampNs; u32 stride; }  // seguido RGBA8
```
Plugin QML: `Image { source: "image://anispaper/<output>?f=" + frameNo }` con un
QQuickImageProvider que mapea el shm y sirve el frame actual (~30fps, timer).
Config del plugin por pantalla: `output=<nombre QScreen>`. Si no hay frame → muestra
`fallback.jpg` del tema (nunca negro, nunca crash).

## 6. API DEL SOCKET (JSON-RPC, única fuente de verdad)
`catalog.list` / `catalog.addFolder` / `catalog.refresh`
`monitor.list`  → [{name, geometry, currentWallpaperId}]
`wallpaper.apply {id, output}` / `wallpaper.stop {output}`
`preview.frame {output}` → jpeg base64 (para el preview vivo de la UI)
`status.get` → por output: {renderer, fps, crashes, safeMode}
`sddm.snapshot {destPath}` → vuelca el frame actual a PNG
`settings.get/set` (fps cap, volumen default, cuota de retries)

## 7. CATÁLOGO
- Parsea `project.json`: `title, type, file, preview, tags, general.properties`.
- Miniaturas: `preview.jpg` del workshop; si no existe, primer frame del renderer.
- Soporta carpetas custom además del workshop (el usuario quiere "ver carpetas"):
  explorador de carpetas en UI que lista subcarpetas con project.json o videos sueltos
  (mp4/webm/mkv auto-tipo `video`).

## 8. UI — MISMO LAYOUT QUE WALLPAPER ENGINE, SKIN ANIS STAR
```
┌ TopBar: ★ AnisPaper · buscador · selector de monitor · ⚙ ┐
├ LeftNav: Instaladas | Descubrir | Favoritas | Carpetas   │
├ Grid central: cards (miniatura, título, badge de tipo    │
│  VIDEO/SCENE/WEB/APP, ★ favorito, badge ⚠ si safeMode)   │
├ Panel derecho: PREVIEW VIVO (preview.frame), controles   │
│  (play/pausa, volumen, velocidad), botones:              │
│  [★ Aplicar a <monitor>] [📸 Capturar para SDDM]          │
└ StatusBar: daemon ok · sesión X11/Wayland · estrategia · │
  fps · crashes · "plasma intacto 😌"                      │
```
- Selector de monitor aplica por pantalla (Plasma soporta wallpaper distinto por
  screen; el plugin lee su config `output`).
- Toasts HUD estilo AnisIDE para errores ("renderer crasheó 3 veces → modo seguro").

## 9. MÓDULO SDDM (captura del wallpaper actual)
1. `sddm.snapshot` guarda PNG actual en `~/.config/anispaper/sddm-background.png`.
2. Genera tema `anis-star`: copia el tema SDDM activo (o `breeze`), reemplaza
   `Background=` en `theme.conf` por nuestra captura, mantiene Main.qml funcional.
3. Helper `anispaper-sddm-install` que con `pkexec`: copia el tema a
  `/usr/local/share/sddm/themes/anis-star/` y escribe
   `/etc/sddm.conf.d/anis-star.conf` con `[Theme] Current=anis-star`.
4. Botón en UI "Actualizar SDDM" → snapshot + helper + toast de éxito.

## 10. FASES Y DoD (commits por fase)
- **F0** probe + scaffold + `docs/probe.md` con estrategia elegida.
- **F1** daemon: catálogo + socket + monitor.list (sin render aún). DoD: `catalog.list`
  devuelve tus wallpapers reales de Steam.
- **F2** renderers video + web con watchdog y modo seguro. DoD: `preview.frame`
  devuelve jpeg vivo de un wallpaper `web` que antes crasheaba plasma.
- **F3** bridge SHM + plugin Plasma usuario. DoD: escritorio Wayland muestra el video
  DEBAJO de los iconos; mata al renderer a mano → plasma vivo, frame congelado y
  auto-restart.
- **F4** scene + application según tabla. DoD: scene reproduce o cae a modo seguro
  con badge, sin crash de shell.
- **F5** UI Electron completa (layout §8) cableada al socket.
- **F6** SDDM (§9) + packaging: AppImage UI, `anispaper.service` user unit, plugin,
  README con captura del escritorio con iconos visibles.
Cada fase: `build` verde + prueba real en tu sesión + commit
`feat(anispaper): fase-N ...`.

## 11. CRITERIOS DE ACEPTACIÓN FINALES
- [ ] Aplicar un wallpaper `web` y uno `scene` "asesinos": plasma-shell NO crashea
      nunca; iconos/carpetas visibles encima.
- [ ] `kill -9` al renderer → watchdog lo revive; UI muestra el crash y el restart.
- [ ] Selector de monitor aplica wallpapers distintos por pantalla.
- [ ] Preview vivo en panel derecho sin screen-capture externo.
- [ ] "Capturar para SDDM" + reinicio → pantalla de login con tu wallpaper.
- [ ] Carpetas custom agregadas desde UI aparecen en el grid.
- [ ] Tema ANIS STAR consistente con AnisIDE.

## 12. FUERA DE ALCANCE (por ahora)
Store de Steam, suscripciones, sincronización con la app de Windows, audio global
mixer, temas de otras Nikkes.

## 13. STEAM EN TIEMPO REAL (watcher)
- El daemon observa con inotify: `libraryfolders.vdf` y `content/431960/*`.
- Cambios → refresh de catálogo + evento push `catalog.changed`
  (socket: `events.subscribe`). La UI actualiza el grid en vivo.

## 14. INTEGRACIÓN STEAM (Descubrir / Instalar)
- Tier 1: botón "Abrir en Steam" por card
  (`https://steamcommunity.com/sharedfiles/filedetails/?id=<id>`).
- Tier 2: caja "Instalar por ID" en Descubrir → steamcmd
  `+workshop_download_item 431960 <id>` → symlink a carpeta custom del catálogo.
  Probe en F0: ¿funciona anónimo? si no, `+login` interactivo una vez.
- Tier 3: tab Descubrir con búsqueda vía Steam Web API
  (`IPublishedFileService/QueryFiles`, key en settings, miniaturas remotas).
- Fuera de alcance: suscripción programática sin cliente Steam.

## AÑADIR AL PROBE (F0)
- ¿Existe `steamcmd`? ¿`workshop_download_item` anónimo funciona con un
  wallpaper público chico? ¿Hay API key configurada?

## CRITERIO DE ACEPTACIÓN NUEVO
- [ ] Suscribirse a algo en Steam → aparece en el grid < 2 s sin tocar nada.
- [ ] Instalar por ID desde la UI descarga y aplica el wallpaper.