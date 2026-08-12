# Scene renderer — backend offscreen de Wallpaper Engine

## Resumen

Los wallpapers de Wallpaper Engine con `"type": "scene"` se renderizan
**animados** usando el motor upstream `almamu/linux-wallpaperengine`, en un
proceso hijo **aislado** lanzado por `anis-paperd`. `plasmashell` nunca ejecuta
shaders ni el motor: sólo dibuja frames sencillos llegados por SHM (diseño F0).

```
wallpaper scene (steamapps/workshop/content/431960/<id>)
   + scene.pkg / scene.json / materials / shaders / particles
        ↓
wallpaperengine-core (vendored, third_party/linux-wallpaperengine)
        ↓  render OpenGL a ventana GLFW X11 *oculta* (nunca mapped)
FBO interno del wallpaper (wallpaper->getWallpaperFramebuffer)
        ↓ glReadPixels + muestreo UV (misma lógica que --screenshot)
frame RGB 1920x1080
        ↓ libjpeg (q82) + base64
protocolo hijo JSON-lines en stdout (igual que video/web)
        ↓
IsolatedRenderer (daemon)  →  FrameBridge (SHM /anispaper-<output>)
        ↓
org.anispaper.frame (plugin Plasma)  →  QML Image  →  escritorio
```

## Binarios

| Binario | Rol |
| --- | --- |
| `anis-paper-scene-engine` | Child offscreen del motor; sin Qt. Flags: `--file <projectDir> --width --height --fps --scaling fill|fit|stretch` |
| `anis-paperd` | Selecciona el child vía `SceneRenderer::nativeSupported()` (binario junto al daemon o `ANISPAPER_SCENE_ENGINE_BIN`); `IsolatedRenderer` lo gestiona con watchdog/backoff igual que video/web |

## Detección y dependencias

El child necesita las **assets de Wallpaper Engine**
(`steamapps/common/wallpaper_engine/assets`, para shaders base y el VFS de
combinado/bloom). Resolución automática:

1. `ANISPAPER_WE_ASSETS` (override explícito, si set)
2. `<steamapps>/common/wallpaper_engine/assets` derivado del path del project
3. `~/.local/share/Steam/steamapps/common/wallpaper_engine/assets`

Si no se encuentra → `fatal` del child → fallback `SceneRenderer`
(static) vía watchdog existente.

El contexto GL es GLX/X11 sobre XWayland (`DISPLAY`; si falta, el daemon
inyecta `:0` en el entorno del hijo). La ventana GLFW se crea con
`GLFW_VISIBLE FALSE` y **nunca** se invoca `showWindow()` (parche
`settings.render.offscreen` de AnisPaper sobre upstream). No hay ventana
flotante ni KWin dialog: `xwininfo -root -tree` no muestra ninguna ventana
del motor.

## Ciclo de vida y protocolo

- El daemon spawnea al hijo con cwd neutro y `ANISPAPER_SCENE_ENGINE_BIN`
  opcional; si el child muere, el watchdog (3 crashes ⇒ backoff 1/3/9s) lo
  reinicia y, tras stable 60s, resetea el contador. Ante fallos crónicos la
  cadena de fallback `SceneRenderer`/`StaticImageRenderer` sigue existiendo
  (sólo para errores reales).
- Comandos stdin: `{"command":"pause"|"resume"|"stop"}` (mismo protocolo que
  los hijos Qt).
- Eventos stdout: `ready`, `frame` (JPEG+base64, máx. ~4 MiB/línea) y
  `fatal`. El daemon decodifica y publica el bridge SHM con el `scaleMode`
  vigente (`cover→fill`, `fit→fit`, `stretch→stretch`).

## scaleMode

No hay re-scaling en QML: el motor produce la imagen final a la resolución
física del wl_output; el child manda `--scaling <scaledMode>` al motor
(`fill`/`fit`/`stretch`) y el bridge marca `scaleMode` igual.

## Debugging

- `ANISPAPER_SCENE_DEBUG=1`: cada 30 frames el child emite por stderr
  `scene-engine frame=N pixelHash=<fnv64>` — si `pixelHash` no cambia entre
  dos líneas, el scene está congelado.
- `journalctl --user -u anispaper.service` muestra stderr del child
  (warnings de GLSL/parámetros del scene).
- La validación Ripple directamente: `tools/f8` style checks — aplicar,
  muestrear `/dev/shm/anispaper-<output>` (offset 12 frameNo, 28 stride,
  32 píxeles RGBA) y hacer 3 capturas `spectacle -f -o` con diff de crops.

## Limitaciones conocidas

- **Throughput**: ~21–25 FPS extremo a extremo a 1080p (codificación JPEG +
  JSON/base64 + SHM). Suficiente para uso actual; mejora futura razonable —
  transportar frames RGBA/XRGB8888 por un segundo SHM daemon⇄child o DRI
  dmabuf, sin cortar por ahora el diseño JSON.
- Scenes con sistemas de audio: el child pasa `--silent` (la app ya controla
  el audio por el daemon).
- Scenes muy GPU-pesados compiten con el escritorio; si el child se queda
  sin renders por CPU, el FPS baja pero el pipeline no se desincroniza.
- Web wallpapers **no** pasan por este motor (stub sin CEF): los dirige el
  WebRenderer Qt existente.
