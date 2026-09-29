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
FBO interno del wallpaper
        ↓ glBlitFramebuffer + readback asíncrono PBO
triple-buffer POSIX SHM privado del child (RGBA8888)
        ↓ copia validada al FrameBridge SHM público /anispaper-<output>
        ↓ watcher Plasma + QML Image + provider con mmap/cache por output
escritorio
```

JPEG/base64/JSON-lines se conserva como fallback de transporte; el camino
normal de escenas nativas publica los frames por SHM. El FrameBridge es la
frontera que consume el plugin Plasma.

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

En modo offscreen, el child captura desde el FBO propio del wallpaper y omite
el composite, clear y `glfwSwapBuffers()` del backbuffer GLFW oculto de
640×480. Ese swap no forma parte de la imagen publicada y agregaba una espera
extra por frame. El fence del PBO se sigue enviando a la cola GL con
`glFlush()` para que el readback asíncrono avance sin depender del swap oculto.
La ruta visible upstream conserva su swap habitual.

## Ciclo de vida y protocolo

- El daemon spawnea al hijo con cwd neutro y `ANISPAPER_SCENE_ENGINE_BIN`
  opcional; si el child muere, el watchdog (3 crashes ⇒ backoff 1/3/9s) lo
  reinicia y, tras stable 60s, resetea el contador. Ante fallos crónicos la
  cadena de fallback `SceneRenderer`/`StaticImageRenderer` sigue existiendo
  (sólo para errores reales).
- Comandos stdin: `{"command":"pause"|"resume"|"stop"}` (mismo protocolo que
  los hijos Qt).
- Eventos stdout: `transport` anuncia el SHM privado, `ready`, `frame` pequeño
  (notificación de secuencia; JPEG/base64 sólo en fallback) y `fatal`. El
  daemon valida/copia el frame y publica el bridge SHM con el `scaleMode`
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

- **Throughput** depende de la escena, GPU y compositor. Las cifras históricas
  de ~21–25 FPS pertenecen al transporte JPEG/base64 previo; no describen la
  ruta SHM actual. Ver la medición de hardware y sus límites en
  [`performance-scene.md`](performance-scene.md).
- Scenes con sistemas de audio: el child pasa `--silent` (la app ya controla
  el audio por el daemon).
- Scenes muy GPU-pesados compiten con el escritorio; si el child se queda
  sin renders por CPU, el FPS baja pero el pipeline no se desincroniza.
- En una RX 6600 con dos monitores 1080p, cap 60 y el swap oculto omitido,
  AnisPaper midió ~59 FPS publicados por salida con sensor GPU en 27 W. Es una
  medición de una sesión concreta y anterior a confirmar presentación por KWin;
  detalles y límites en [`performance-scene.md`](performance-scene.md).
- Web wallpapers **no** pasan por este motor (stub sin CEF): los dirige el
  WebRenderer Qt existente.
