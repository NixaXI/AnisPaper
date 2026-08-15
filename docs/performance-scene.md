# Performance pipeline `scene` — anis-paper-scene-engine → Plasma

Escena de benchmark: `steam:1155012801` (Steam Workshop 431960), salida `HDMI-A-1`
1920x1080, `--fps 60` pedido al engine. Mediciones con `ANISPAPER_SCENE_PROFILE=1`
(child) y `ANISPAPER_PROFILE=1` (daemon), ventanas de 120 frames.

## FASE 1 — BASELINE (arquitectura JPEG/base64/JSON, ~26 FPS)

### Child `anis-paper-scene-engine` (avg ms / p95 ms, n=120 por ventana)

| Etapa                | avg   | p95   | notas                                              |
|----------------------|-------|-------|----------------------------------------------------|
| period (engine loop) | 37.9  | 44.4  | ≈ **26.4 FPS** producidos                          |
| readback             | 6.7   | 12.7  | `glFinish()` + `glReadPixels` RGB + alloc 6MB/frame|
| resample             | 5.9   | 7.8   | loop CPU nearest-neighbor float x 6.2M px + flip   |
| jpeg encode q82      | 6.5   | 8.5   | libjpeg + allocs                                   |
| base64 encode        | 0.5   | 0.6   |                                                    |
| ipc-write (stdout)   | 0.7   | 2.2   | JSON-lines con megabytes                           |
| callback total       | 20.2  | 26.6  |                                                    |

El resto del periodo (~17 ms) es render/update GPU del engine + swap.
CPU child ≈ 47 %, RAM ≈ 207 MB.

### Parent `anis-paperd`

| Etapa        | avg   | p95  |
|--------------|-------|------|
| period       | 37.8  | 49   | ≈ 26.3 FPS recibidos |
| json-parse   | 0.7   | 1.1  |
| b64-decode   | 2.2   | 2.9  |
| jpeg-decode  | 14.2  | 20.9 | ← etapa más cara del parent |
| (post) convertToFormat(RGBA8888) + copia al SHM bridge | no medido | |

CPU daemon ≈ 71 %, RAM ≈ 161 MB. Cada `frame_` pasa
`JPEG bytes → QImage RGB → convert RGBA8888 → copia al SHM` = 3+ copias.

### Plugin Plasma (`org.anispaper.frame`)

- `Timer { interval: 33 }` → **techo ~30 FPS aunque el engine dé más**.
- Cada tick recarga `image://anispaper/<output>?f=N` con `cache:false`,
  `asynchronous:false` → carga síncrona en GUI thread de plasmashell.
- Provider: **por request hace `shm_open` + `mmap` + `QImage::copy()` (8.3 MB)
  + `munmap` + `close`** → syscall storm + copia completa por frame.
- plasmashell ≈ **107 % CPU**, RAM 458 MB.

### Síntesis del bottleneck (~38 ms/frame end-to-end)

1. Pipeline productor limitado por callback CPU de 20 ms (readback sync +
   resample CPU + JPEG + base64) sumado al render del engine.
2. Parent añade latencia y CPU altísima (jpeg-decode 14 ms + b64 2 ms + parse).
3. Plugin capado a ~30 Hz por el Timer y con copias/syscalls por request.

Impacto observable adicional: SMITE cae a ~38 FPS con el scene activo
(competencia GPU/CPU por el pipeline ineficiente).

---

## FASE 2+ — OPTIMIZACIÓN (resultados al final del documento)

Plan aplicado (ver secciones de cambios más abajo):

1. SHM binario triple-buffer child→parent (RGBA8888), JSON solo para control.
2. `glBlitFramebuffer` GPU-side para crop+flip+scale del FBO del wallpaper a un
   scratch FBO del tamaño exacto del target (elimina resample CPU).
3. Readback asíncrono con ring de 3 PBOs + `glFenceSync` (elimina `glFinish`).
4. Buffers reutilizados, cero allocs grandes por frame.
5. Parent: para Scene nativo con geometría RGBA idéntica, una sola `memcpy`
   transport→bridge SHM directa; si la geometría o metadata no coincide,
   conserva la ruta `QImage` con conversión/escalado. JSON solo para líneas de
   control pequeñas.
6. Plugin: watcher C++ event-driven (16 ms, solo recarga cuando cambia
   `frameNo` real del header SHM) + provider mapeo cacheado.
7. Fullscreen pause reactivado (gaming-friendly): el engine pausa solo cuando
   hay una ventana fullscreen (X11 detector sobre XWayland).
8. `scene.targetFps` configurable (30/60), pacing del engine existente
   (sin busy-loop).

Puntos 7 y 8 quedan pendientes documentados abajo.

---

## FASE 2 — RESULTADOS (transporte binario child→daemon implementado)

### Cambios aplicados

**Child `src/scene_engine/main.cpp`**
- `SceneTransport`: POSIX shm `/anispaper-scene-<pid>`, header propio `ANST`
  v1 (64 bytes, `static_assert`) + 3 slots RGBA8888, publicación
  latest-frame-wins con atomic acquire/release (GCC builtins) y reclaim del
  nombre si existe uno stale (`EEXIST`).
- `AsyncReadback`: scratch FBO (GL_RGBA8) + ring de 3 PBOs con fences;
  `glBlitFramebuffer` hace crop/flip/scale GPU desde el FBO del wallpaper
  usando el rect UV de `getTextureUVs()` (el `renderVFlip()` del engine se
  resuelve invirtiendo la Y fuente del blit). `frontReady()` sondea sin
  bloquear y descarta fences rotos; `consume()` = map + memcpy, **sin
  `glFinish` en ningún punto**.
- Callback reescrito: tras 10 frames de warmup crea readback + transport,
  emite `{"event":"transport",...}` y luego `ready`. Por frame: captura
  submitSeq → poll del PBO más antiguo → memcpy al slot shm (o heap de
  fallback) → publish + línea JSON enana `{"event":"frame","shm":true,"seq":N}`.
- Fallback JPEG/base64 conservado (`ANISPAPER_SCENE_JPEG=1` o fallo de
  transporte) con la ruta RGBA→RGB + `jpegEncode`; `pixelHash` debug intacto.
- Profiler `ANISPAPER_SCENE_PROFILE=1` con etapas renombradas:
  `submit/wait/map+copy/ipc-write` (jpeg/b64 solo reportan si el fallback
  está activo).

**Parent `src/renderers/isolated_renderer.{h,cpp}` + `src/bridge/frame_protocol.h`**
- `frame_protocol.h` comparte `SceneTransportHeader` + helpers de carga
  atómica (`loadSceneFrameNo` / `loadSceneWriteIndex`) entre child y daemon.
- `IsolatedRenderer::openSceneTransport(name)`: valida magic/versión/formato,
  consistencia stride/size vs `fstat`, y exige layout idéntico a un
  `QImage::Format_RGBA8888` propio (`bytesPerLine == stride`). `frame_` se
  reemplaza por esa imagen privada y writable: ninguna copia implícita se
  muta mientras consumidores la retienen.
- `copySceneTransportFrame()`: lee seq, copia el slot `writeIndex` y
  verifica seq tras la copia (reintenta 1 vez si el writer corrió).
- Mensaje `{"event":"transport"}` abre el shm; mensajes `{"event":"frame"}`
  con `shm:true` solo disparan la copia (sin JSON con payload).
- `stop()` cierra el mapping (sin fd/colgados); el destructor de
  `IsolatedRenderer` sigue sin tocar GL.
- Bridges existentes (`video`/`web`/`gif`) intocados: siguen el camino JSON.

**Plugin Plasma**
- `frame_image_provider.cpp`: rama rápida en `requestImage` cuando
  `actual == cached.frameNo` → re-sirve la copia cacheada sin repetir el
  memcpy de 8.3 MB por tick redundante (antes copiaba en CADA poll).
- `main.qml`: Timer 33 ms → **16 ms** (hasta 60 Hz de display; los ticks sin
  frame nuevo ahora cuestan solo open/mmap/header).

### Medido (mismo benchmark, con el daemon viejo y su engine coexistiendo)

Child (`ANISPAPER_SCENE_PROFILE=1`, ventanas de 120):

| Etapa      | avg   | p95   | notas                                        |
|------------|-------|-------|----------------------------------------------|
| period     | 21.4  | 25.7  | **≈ 46-47 FPS con 2 engines compartiendo GPU** |
| submit     | 0.19  | 0.42  | blit + readPixels→PBO + fence                |
| wait       | 0.19  | 0.55  | non-blocking, sin `glFinish`                 |
| map+copy   | 3.5   | 7.6   | memcpy 8.3 MB PBO→shm (incluye contención)   |
| ipc-write  | 0.03  | 0.07  | JSON de control solamente (era 0.7-2.2)      |
| callback   | 3.9   | 8.2   | **~10× mejor que FASE 1 (20.2/26.6)**        |

Con un único engine en GPU el presupuesto de 16.7 ms por frame queda sobrado
(callback ~4 ms) → objetivo de 60 FPS alcanzable; la lectura de 46-47 FPS en
esta medición está limitada por el segundo engine corriendo a la vez.

Checklist runtime: shm creado/limpiado en `/dev/shm` (SIGTERM incluido),
sin fugas de FBO/PBO visibles, ~700 frames publicados en los primeros 16 s.

### FASE 3 — Despliegue y medición end-to-end (HECHO)

Despliegue: binarios a `~/.local/bin` (backups `anis-paperd-pre-f6` /
`apse-pre-f6`), plugin `.so` + `main.qml` a
`~/.local/share/plasma/wallpapers/org.anispaper.frame/...`, daemon vía
`systemctl --user restart anispaper.service`, wallpaper restaurado con
JSON-RPC `wallpaper.apply` (`steam:1155012801` en `HDMI-A-1`).

**Plugin Plasma — regresión detectada y corregida (FrameWatcher push):**
- Primer intento (`Timer { interval: 16 }` → 60 requests/s): cada tick
  cambiaba `source` → `QQuickImage` rehacía request + textura incluso cuando
  NO había frame nuevo → plasmashell se disparó a ~**277 % CPU** (peor que el
  107 % de FASE 1).
- El `QImage::copy()` fuera (conclusión del análisis: el QML rebuild es el
  coste, cueste o no el shm).
- Solución: clase **`FrameWatcher`** (QObject) en el plugin; poll cada 16 ms
  de **solo los 32 bytes del header ANIS** vía `shm_open`+`pread`+`close`
  (~3 syscalls, ~5 µs; sin mapeo persistente → sobrevive a recreación del
  bridge tras restart del daemon), `frameNoChanged` solo cuando cambia la
  secuencia. `main.qml`: `frameNo: watcher.frameNo` (binding push), Timer
  eliminado. La vía rápida de `requestImage` (`actual == cached.frameNo`)
  queda como red de seguridad.

**Resultados end-to-end reales (estable):**

| Métrica                     | FASE 1 (JSON) | FASE 3 (shm) |
|-----------------------------|---------------|--------------|
| FPS bridge/plasma           | ~26           | **~47-49**   |
| child callback avg/p95      | 20.2/26.6 ms  | **2.8/5.3 ms** |
| parent period avg           | 37.8 ms       | **21.2 ms**  |
| parent b64-decode           | 2.2 ms        | **0 (eliminado)** |
| parent jpeg-decode          | 14.2 ms       | **0 (eliminado)** |
| parent shm-copy             | —             | **2.9 ms**   |
| CPU `anis-paperd`           | ~71 %         | **~16 %**    |
| CPU `anis-paper-scene-engine`| ~47 %        | **~24 %**    |
| CPU plasmashell             | ~107 % @30 Hz | **~170 % @48 Hz** (≈3.4 %/frame, igual coste unitario; +60 % frames) |

Tests: `ctest` → 5/6 verde (`f2_renderer_child` preexistente: falta
`libcuda.so.1` — ruta `video` no tocada).

### Pendiente real
- Techo visible ~48 FPS es el presupuesto de **render GPU del engine**
  (periodo 20.5 ms con 60 pedidos); el transporte ya sobra (callback 2.8 ms).
  Subir a 60 = optimizar el render de la escena/cap `fpsCap`, no el pipeline.
- 7/8 del plan: fullscreen-pause y `scene.targetFps` configurable.
- Micro-opts conocidas: PBO persistente / memcpy partido (bajar los 2.5-3.5 ms
  de `map+copy`); mapeo cacheado del bridge en el provider (hoy `shm_open`+
  `mmap` por frame NUEVO ~49/s; ya gratis en ticks sin cambio).
- Re-medir impacto en juego (SMITE era ~38 FPS): con daemon 16 % + child 24 %
  y sin readback sync, se espera recuperación notable.

---

## FASE 4 — revisión ChatGPT sobre snapshot del 2026-08-09 (pendiente de hardware real)

La revisión del código encontró que la conclusión «~48 FPS = techo de GPU» no
estaba demostrada todavía. Había un **doble presupuesto de frame** en el camino
offscreen:

1. `GLFWOpenGLDriver::dispatchEventQueue()` dormía hasta completar 16.67 ms a
   `--fps 60`.
2. `WallpaperApplication::render()` ejecutaba **después** el callback AnisPaper
   (PBO/SHM), medido en ~2.8 ms avg, más el resto del trabajo posterior.

Por construcción el periodo real era aproximadamente `16.67 ms + callback +
overhead`, lo que encaja con el techo observado de ~47–49 FPS. El parche mueve
el pacing offscreen al final del frame completo: el driver ya no duerme en modo
offscreen y `WallpaperApplication::render()` usa un deadline que incluye render
+ callback. La ruta upstream normal conserva su pacing original.

Cambios adicionales de esta revisión:

- Offscreen: se omite el segundo composite del wallpaper FBO hacia el backbuffer
  GLFW oculto y también `glfwSwapBuffers()`/clear del backbuffer; se mantiene
  `glFlush()` sin espera.
- El PBO hace `glFlush()` tras publicar el fence, necesario al eliminar el swap
  oculto.
- Daemon: el slot triple-buffer del child se expone como `QImage` read-only y el
  bridge hace la **única** copia grande directamente al SHM de Plasma cuando el
  layout ya es RGBA8888 1:1.
- Provider Plasma: carga forzada asíncrona y fd/mmap persistente por output para
  sacar la copia de 1080p del GUI thread y eliminar open/fstat/mmap/munmap por
  cada frame nuevo.
- Fullscreen X11: corregido el uso de `GLFWwindow*` como si fuese un XID y un
  `XFree(children)` equivocado que dejaba un use-after-free. Se usa
  `glfwGetX11Window()`, EWMH + geometría, y cache de 200 ms.
- El child scene se lanza con `XDG_SESSION_TYPE=x11` porque su backend real es
  GLFW/X11 sobre XWayland. Así el factory de linux-wallpaperengine selecciona
  el detector X11 para juegos Proton/XWayland en vez de un detector Wayland que
  no forma parte de este build.

### Hipótesis a validar en el equipo real

Con el mismo scene `steam:1155012801`, a 1920x1080 y target 60:

- Si `render + callback < 16.67 ms`, el producer debería acercarse a 60 FPS en
  lugar de quedar artificialmente en ~48.
- Al entrar a un juego XWayland fullscreen, el child debería pausar en <=~250 ms
  y liberar la mayor parte de su CPU/GPU hasta salir de fullscreen.
- Si el escritorio llega a ~60 FPS pero `plasmashell` sigue consumiendo demasiado,
  el siguiente cuello queda en el upload de textura de Qt Quick, no en el
  transporte child→daemon.

No se anotan cifras AFTER aquí porque este snapshot no puede reproducir el
stack CachyOS/KWin/AMD del equipo del usuario. Usar `tools/perf/deploy-and-probe.sh`
para build/deploy + una única medición en hardware real.

---

## FASE 5 — copia directa Scene transport → FrameBridge (2026-08-11)

Esta fase optimiza únicamente la copia del **padre** entre el transporte SHM
privado de una escena nativa y el bridge SHM público que consume Plasma. No
cambia `FrameHeader`, la resolución física, el formato RGBA8888, el plugin ni
el protocolo del child.

Antes de este cambio, cada frame estable hacía:

```text
PBO child → slot Scene SHM → QImage privado del daemon → FrameBridge SHM → Plasma
```

Ahora `IsolatedRenderer` entrega la vista del slot al `RendererManager` de
forma síncrona y `FrameBridge::publishSceneTransport()` copia directamente
ese slot al payload público. La publicación conserva el marcador ocupado
(`frameNo = 0`), comprueba el `frameNo` y `writeIndex` de origen antes y
después de la copia, y reintenta una vez ante una carrera. Si no obtiene un
snapshot estable, no publica una imagen inconsistente; las rutas QImage/JPEG
de fallback siguen disponibles. `preview.frame` usa un snapshot propio del
bridge bajo la misma comprobación de secuencia.

### Medición real

Dos scenes nativas distintas, una por salida (`DP-2` y `HDMI-A-1`), ambas a
1920×1080 físicos con `cover`, `--fps 60` solicitado. Cada valor es la media
de 16 muestras a un segundo de `top`; `%CPU` es la convención por core de
`top`, por lo que no representa un porcentaje del total de los 16 cores.

| Proceso | Antes | Después | Cambio |
|---|---:|---:|---:|
| sistema | 62.0 % | **51.2 %** | **−10.8 pp (−17.4 % relativo)** |
| `anis-paperd` | 60.5 % | **27.3 %** | **−33.2 pp (−54.9 % relativo)** |
| scene DP-2 | 30.8 % | 30.1 % | dentro de variación normal |
| scene HDMI-A-1 | 33.7 % | 28.1 % | dentro de variación normal |

En la comprobación final sin `ANISPAPER_PROFILE` ni
`ANISPAPER_SCENE_PROFILE`, ambos bridges estaban activos, con `hasFrame=true`,
`fallback=false`, `safeMode=false`, watchdog global `count=0`, y FPS observados
de 57.90 y 58.0. `preview.frame` devolvió JPEG válidos de 1920×1080 para ambas
salidas. `plasmashell` siguió vivo durante toda la prueba.

### Cobertura de seguridad

- `scene_transport_bridge_test`: estable, duplicados, índice inválido, carrera
  durante copia, reintento, wrap de secuencia, metadata incompatible, dos
  bridges, snapshot ocupado y ABI del header.
- `isolated_renderer_cleanup_test`: el transporte Scene sintético se libera e
  invocar `stop()` de nuevo es idempotente incluso si el `QProcess` ya estaba
  terminado; cubre la ruta que toma el watchdog tras un crash.
- `static_image_renderer_test` y smoke del plugin F3: verdes.
- Un crash real de un child scene fue recuperado por el watchdog mientras la
  otra salida siguió publicando; tras estabilidad el contador volvió a cero.

La siguiente zona a medir, si alguna vez hiciera falta, es el upload/copia de
Qt Quick en `plasmashell`; no se modificó aquí porque no había perfil de
símbolos que justificara un cambio adicional.

## F10 — 2026-08-13: fullscreen sin bloquear el daemon

Se corrigió el detector X11 del child (XID nativo de GLFW, liberación correcta
de la lista X11 y caché de 200 ms) y el child scene se fuerza a
`XDG_SESSION_TYPE=x11` porque su ventana oculta usa XWayland.

La consulta experimental a `org.kde.KWin.queryWindowInfo` fue retirada: en KWin
6.7 esa API es un selector interactivo y devolvía `org.kde.KWin.Error.UserCancel`;
llamarla desde un timer era la causa del cursor `+` y del bloqueo de clicks.
No existe actualmente un throttle fullscreen activo en el daemon. Gaming Mode
debe usar una fuente no interactiva y específica de juegos antes de reintroducir
la suspensión automática.

## Gaming Mode — 2026-08-13

Se añadió un modo de protección para juegos con tres estados persistidos:
`auto` (por defecto), `on` y `off`. En `auto` el daemon sólo inspecciona
`/proc/*/{cmdline,environ}` buscando señales de Steam/Proton
(`SteamAppId`, `SteamGameId`, `STEAM_COMPAT_APP_ID`, rutas
`steamapps/common` o `compatdata`). No consulta KWin, no usa D-Bus y no lanza
selectores ni herramientas X11. Al detectar el juego, cada renderer recibe
`pause()` y conserva el último frame en su bridge; al salir recibe `resume()`.

Verificación de transición contra el servicio:

```sh
python3 tools/perf/anispaper_rpc.py settings.set '{"gamingMode":"on"}'
sleep 2
python3 tools/perf/anispaper_rpc.py status.get
python3 tools/perf/anispaper_rpc.py settings.set '{"gamingMode":"off"}'
sleep 3
python3 tools/perf/anispaper_rpc.py status.get
python3 tools/perf/anispaper_rpc.py settings.set '{"gamingMode":"auto"}'
```

Con `on`, `gaming.active=true`, `gamingPaused=true` en DP-2 y HDMI-A-1 y los
`frameNo` quedaron congelados con `hasFrame=true`. Con `off`, ambos avanzaron
de nuevo y el watchdog permaneció limpio. La detección automática también se
probó con un proceso efímero `env SteamAppId=999999 sleep 8`: activó el modo y
se desactivó al terminar.

Medición de 8 muestras (`top -b -n 8 -d 1`) en la misma sesión, dos salidas
activas:

| estado | plasmashell | anis-paperd | scene child | video child |
|---|---:|---:|---:|---:|
| activo | 282–309% | 58–63% | 22–27% | 61–70% |
| Gaming Mode | 0–1% | 0% | 2–3% | 0% |

La prueba de `SteamAppId` es una comprobación de detección, no una partida de
SMITE 2. En una segunda pasada sí se observó SMITE 2 real (AppID 2437170,
Proton/GE-Proton11-3): Gaming Mode automático quedó activo, ambos renderers
quedaron pausados, `hasFrame=true`, `safeMode=false`, y ocho muestras de `top`
mostraron `anis-paperd` 0–1%, scene 0–3%, video 0–2% y `plasmashell` 1%,
mientras el proceso del juego se mantuvo alrededor de 644–685% (`top` por
core). La sesión no tenía MangoHud ni un contador FPS exportado por el juego,
por lo que no se inventa un FPS numérico; la comprobación visual de FPS debe
hacerse dentro de SMITE con su overlay/contador de usuario.

También se corrigió una carrera de arranque: si el juego ya estaba abierto,
pausar el scene child antes de `ready` no arma el watchdog de startup; el límite
se arma al reanudar. Tras reiniciar el daemon con SMITE activo ambos outputs
quedaron `state=running`, `crashes=0`, `safeMode=false` y `hasFrame=true`.
Cuando SMITE terminó, la detección volvió a `gaming.active=false` sin reiniciar
los procesos: DP-2 reanudó a ~57.65 FPS y HDMI-A-1 a ~29.89 FPS, ambos con
`hasFrame=true` y `crashes=0`.

El pipeline normal sigue teniendo como costes principales el
`glReadPixels`/JPEG de vídeo y la copia+upload de cada frame en el proveedor
Plasma. El proveedor ahora mantiene el mmap por output (con remape al detectar
recreación/secuencia reiniciada) y conserva la copia privada anti-tearing; el
ABI SHM no cambió.

### Preview RPC — optimización incremental

`preview.frame` conserva la respuesta completa para clientes existentes, pero
acepta opcionalmente `maxWidth` y `maxHeight` (enteros 64–3840) y reduce con
`Qt::KeepAspectRatio` antes de JPEG/base64. La UI usa `960×540`, que coincide
con el panel visible y evita transportar 1920×1080 innecesariamente. En la
prueba RPC del servicio, la respuesta limitada fue `960×540` y la respuesta
heredada sin límites siguió siendo `1920×1080`; ambas devolvieron JPEG válido.
