## F4 — 2026-08-09 (resuelto en F8)

- **Issue**: `libwallpaperengine` no está instalada en esta sesión (`pkg-config
  wallpaperengine` no existe y no se encontró una biblioteca del motor en las
  rutas del probe F0).
- **Impact**: los wallpapers `scene` no tienen render nativo ni soporte para
  extraer `scene.pkg`; se presentan mediante su `preview.jpg` (o el fallback
  instalado si falta) y permanecen debajo de Plasma a través del bridge SHM.
- **Comportamiento actual**: `status.get` expone `renderer="scene-static"`,
  `sceneNativeSupported=false` y el badge `scene sin soporte nativo`; no se
  intenta cargar una ABI de libwallpaperengine desconocida.
- **Fix futuro**: integrar una versión comprobada de libwallpaperengine con un
  contexto offscreen aislado y entonces añadir el manejo de `scene.pkg`.
- **Dependencia pendiente (no instalada automáticamente)**:
  `yay -S linux-wallpaperengine-git`; después reconstruir con
  `cmake -S . -B build -DENABLE_SCENE=ON && cmake --build build`.

## F8 — 2026-08-09 — RESUELTO: scenes animados

- **Estado**: ✅ RESUELTO. Los wallpapers `type:"scene"` se renderizan
  animados mediante el motor `almamu/linux-wallpaperengine` vendorizado en
  `third_party/linux-wallpaperengine` (ver `third_party/ANISPAPER.md` y
  `docs/scene-renderer.md`).
- **Backend**: proceso hijo aislado `anis-paper-scene-engine` (ventana GLFW
  X11 oculta, nunca mapeada; el motor renderiza a su FBO interno y leemos
  píxeles RGBA). La ruta normal usa `SceneTransport` SHM de triple buffer
  hacia el daemon y luego el bridge SHM; JPEG/base64 queda sólo como fallback
  de transporte. `plasmashell` sigue siendo un display pasivo.
- **Validación**: renderer/SM/pantalla con hashes de píxeles cambiantes;
  3 capturas Spectacle con diff de crops (56.8%/31.8% píxeles cambiantes)
  en `artifacts/scene-animation-proof/`.
- **Notas conocidas**: el coste exacto extremo a extremo depende de la escena
  y del output; hay que medirlo por output antes de cambiar de nuevo la ruta
  SHM. Escena con soporte requerido: `wallpaper_engine/assets` de Steam
  (autodetectado) o `ANISPAPER_WE_ASSETS`.

## F9 — 2026-08-11 — contenido Workshop inválido

- **Estado**: la pasada completa del daemon sobre los 312 items `scene`
  físicos terminó con **306 nativos, 6 fallbacks y 0 crashes**. Los fallbacks
  son seguros y muestran la imagen estática disponible; no activan modo
  seguro.
- **Proyectos sin `scene.json`**: `steam:2461209923`, `steam:2721071666`,
  `steam:2859799851`, `steam:3105530285` y `steam:3658118263`.
- **Proyecto con manifiesto vacío**: `steam:3439763465` (error JSON de entrada
  vacía).
- **Acción necesaria**: verificar o volver a descargar esos seis Workshop
  items si se quiere animación nativa. AnisPaper los mantiene fail-closed en
  fallback estático en vez de reiniciar el renderer.

## Greeter Plasma Login — activación pendiente de autorización

- Esta instalación usa **Plasma Login**, no SDDM. El daemon detecta ese gestor
  y usa la acción oficial KAuth `org.kde.kcontrol.kcmplasmalogin.save` al
  instalar una captura para el greeter.
- `sddm.snapshot` desde la UI exige `requireActive:true` y el output elegido:
  no usa otro monitor ni el fallback. La respuesta confirma
  `manager=plasmalogin`, `scope=display-manager-greeter` y la captura física
  1920×1080 de ese output.
- `sddm.installTheme` usa KAuth de forma asíncrona para que una solicitud de
  autenticación no bloquee el socket ni los renderers. Aplicar una captura al
  login escribe configuración del sistema y requiere autenticación
  administrativa; en esta sesión de desarrollo el agente no mostró el diálogo,
  por lo que la instalación real quedó sin cambios y debe ejecutarse desde la
  UI en una sesión con agente de autenticación activo.
- La ruta cambia el greeter de inicio/cierre, no el lock screen de la sesión
  activa. Si la autorización se cancela, el daemon permanece activo y devuelve
  un error explícito; no se deja un tema parcial.

## P0 — cursor cruzado / selección interactiva de KWin (2026-08-13)

- **Causa confirmada**: un parche de rendimiento consultaba
  `org.kde.KWin.queryWindowInfo` cada 500 ms. En KWin 6.7 esa llamada abre el
  selector interactivo de ventana; devolvía `org.kde.KWin.Error.UserCancel` y
  dejaba el cursor en forma de cruz, bloqueando clicks.
- **Corrección**: se eliminó el timer, la llamada D-Bus, el estado de throttle y
  toda pausa basada en esa API. El daemon no inicia herramientas de selección ni
  grabbers; la auditoría encontró cero `xprop`, `xwininfo`, `xdotool`, `slop`,
  `xkill`, `XGrabPointer` o `XGrabKeyboard` en el código ejecutable.
- **Validación**: stop dejó cero hijos; start/restart restauró exactamente un
  `anis-paper-scene-engine` en DP-2 y un `anis-paperd --renderer-child --type
  video` en HDMI-A-1. Los cambios de wallpaper reemplazaron cada hijo sin
  duplicados y el servicio quedó estable.

## Rendimiento de juegos — estado 2026-08-13

- Gaming Mode (`auto`/`on`/`off`) pausa ambos renderers y conserva el frame del
  bridge. La detección automática observó el SMITE 2 real (AppID 2437170) por
  sus procesos Proton, sin usar KWin ni selectores. Tras reiniciar con el juego
  ya abierto, una carrera de `ready` del scene child fue corregida: no se arma
  el watchdog mientras la pausa es intencional y `lastFrame()` devuelve el
  fallback del bridge. Resultado: ambos outputs `hasFrame=true`,
  `safeMode=false`, `crashes=0`.
- La muestra real disponible no expone un contador FPS (MangoHud no está
  instalado), por lo que el FPS de SMITE no se declara numéricamente. La carga
  de AnisPaper sí cayó a ~0–3% por proceso y `plasmashell` a ~1% mientras el
  juego consumía ~644–685% en la convención de `top` por core.
- `preview.frame` acepta límites opcionales y la UI pide 960×540; sin límites
  mantiene la respuesta 1920×1080 para compatibilidad. El proveedor Plasma
  mantiene el mmap por output y sólo lo recrea al detectar una secuencia nueva
  tras reinicio del bridge. El copy a `QImage` sigue siendo deliberado para
  evitar frames rasgados.

## P1 — crecimiento de memoria en vídeo libmpv/OpenGL — abierto 2026-08-13

- **Medición**: el soak posterior de 4 h pasó de 280283 kB a 414558 kB PSS en
  el único `anis-paperd --renderer-child --type video` activo
  (33609,7 kB/h). El daemon, la escena y los bridges permanecieron estables.
- **Aislamiento**: un hijo sin loop de archivo creció 3947 kB en 7,5 min
  (≈31 MiB/h); saltar `glReadPixels` no eliminó el crecimiento y saltar
  `mpv_render_context_render` sí lo dejó prácticamente plano en la prueba
  aislada. La evidencia apunta a una retención dentro de la ruta de render de
  libmpv/OpenGL, no a JPEG/base64, al bridge SHM ni al loop de reproducción.
- **Mapa de memoria**: en una ejecución aislada de 105 s el PSS subió
  272913→273802 kB y el heap 42960→43788 kB, mientras que los mapas de
  `libmpv`, FFmpeg y libplacebo permanecieron constantes. Desactivar el tcache
  de glibc empeoró el crecimiento y jemalloc elevó el plateau a ~376 MiB; no
  se adoptó ningún allocator alternativo.
- **API de actualización**: el estado final registra un callback de libmpv y
  sólo renderiza cuando `mpv_render_context_update()` devuelve
  `MPV_RENDER_UPDATE_FRAME`. En el reproducer mínimo mantuvo el plateau cuando
  el archivo no se recargaba; en el soak integrado de 15 min redujo la pendiente
  de +33,6 a +8,54 MiB/h y bajó renders redundantes. No es una cura: con
  `loop-file=inf` el reproducer mínimo, sin AnisPaper, subió +62,5 MiB/h PSS
  (+30,7 MiB/h privado) en 540 s útiles. La retención restante queda atribuida
  a libmpv/Mesa/AMD y sigue requiriendo seguimiento upstream.
- **Mitigación instalada**: el hijo ahora intercambia las filas RGBA con un
  scratch de una sola fila reutilizado (`VideoRenderer::flipFrameInPlace()`),
  en lugar de pedir a Qt un `QImage::flip()` por frame. Esto reduce churn
  temporal y conserva el formato/orientación, pero la prueba 1080p de 4 min
  quedó en ~35 MiB/h: no elimina la retención de libmpv y por eso el issue
  sigue abierto.
- **Perfil de allocations**: tcmalloc observó ~3,5 GB acumulados en un minuto
  a 640×360, con ~235 MB vivos; el mayor conjunto vivo fue `av_malloc` de
  libavutil (~208 MB). El cambio de filas evita una copia explícita de imagen,
  pero no puede liberar esa retención del decoder/render de libmpv.
- **Reducción aplicada**: el readback del FBO usa `RGB888` con stride compacto
  cuando el output lo permite; sólo cae a RGBA8888 para strides no compactos.
  El perfil aislado dejó de mostrar `QImageData::create` como allocation
  dominante y el RPC real siguió devolviendo JPEG válido 1920×1080 en HDMI-A-1
  y DP-2. La pendiente PSS del hijo sigue siendo del orden de decenas de MiB/h,
  así que no se confunde esta reducción de churn con una cura del leak.
- **CPU**: después del deploy, la muestra de 8 s bajó el hijo de vídeo a
  29,7% de CPU (antes 35,6% en la misma convención de `top`); plasmashell
  quedó en 50,5%. La mejora es de coste de conversión, no de la retención
  `av_malloc`.
- **Decisión**: no se introduce un reinicio periódico, una reducción de
  resolución/FPS ni un cambio del contrato SHM: cualquiera de ellos podría
  causar flicker o degradar escenas/vídeos. El problema queda explícito para
  una futura actualización de libmpv/driver o un renderer GPU distinto.
- **Estado seguro**: Gaming Mode pausa sin destruir procesos y conserva el
  último frame; el inventario final sigue en 3 objetos SHM/41472128 bytes.

## P0.5 — flicker al recrear un bridge — estado 2026-08-13

- La ruta afectada era el consumidor Plasma, no el productor: al observar un
  `frameNo` menor después de `wallpaper.apply`/reinicio, el provider sustituía
  el último `QImage` válido por `fallback()` durante un tick. Eso podía verse
  como un flash aunque el nuevo bridge ya estuviera vivo.
- El provider ahora conserva siempre la imagen válida anterior durante el
  reset y sólo la reemplaza cuando observa una secuencia nueva completa. El
  smoke test F3 comprueba explícitamente que el tick de reset conserva el color
  anterior y que el siguiente frame nuevo se publica.
- Se descartó `sourceSize` dinámico (binding circular con `implicitWidth`) y
  `asynchronous:true` durante la investigación; el QML queda con
  `asynchronous:false`. No se modificó `FrameHeader`, el nombre SHM ni el
  protocolo de publicación busy/frameNo.
- Validación posterior: aplicar ambos wallpapers dejó PIDs estables, frames
  DP-2 ~58 FPS y HDMI-A-1 ~30 FPS, `hasFrame=true`, `crashes=0`,
  `safeMode=false`; `/dev/shm` contiene sólo los dos bridges y el transporte
  scene activo. No se reinició KWin ni se usaron APIs de selección.
- La primera instalación se comprobó en un `QQuickView` aislado; después se
  hizo una recarga controlada de `plasma-plasmashell.service` para cargar el
  módulo en la sesión real, sin reiniciar KWin.

### P0.5 visual validation — 2026-08-13

- La captura `2026-08-13 03-03-34.mp4` mostró el fallo anterior: 156 frames a
  60 FPS, alternando entre wallpaper y el rectángulo `#0A0D14` (transiciones
  repetidas; no era una pausa del daemon). El archivo de captura fue eliminado
  del repositorio después de analizarlo, a pedido del usuario.
- El `Image` del wallpaper ahora usa `retainWhileLoading: true`, por lo que
  conserva la última textura válida durante la carga de la siguiente URL de
  frame. El provider y el QML smoke siguen pasando, y se recargó una sola vez
  `plasma-plasmashell.service` para cargar el cambio. El servicio quedó activo;
  ambos renderers mantienen `hasFrame=true`, sin crashes ni safe mode.
- Campaña sintética completa: `tools/f1/artifacts/wallpaper-validation-20260813-031854/`.
  Las 312 escenas fueron procesadas sin tocar DP-2/HDMI-A-1: 304 PASS, 5 CRASH
  por `scene.json` ausente en el paquete, y 3 INVALID por contenido faltante o
  JSON inválido. Cada caso terminó con `entryGone=true`, sin PIDs ni SHM
  residuales; el barrido final dejó sólo el transporte activo de DP-2.
- En la medición posterior de headers, los marcadores `frameNo=0` fueron
  únicamente escrituras busy esperadas: DP-2 publicó 364 muestras sin ninguna
  regresión de secuencia y HDMI-A-1 383, también sin regresiones. El estado
  final conserva un solo renderer por output, `hasFrame=true`, watchdog 0 y
  sólo `/dev/shm/anispaper-scene-366657` más los dos bridges físicos.
- Videos: el catálogo contiene 219. La muestra representativa (4K60, 4K30,
  1440p25, 1080p60/30, 720p30/60 y vertical) fue 8/8 PASS en output sintético;
  cada preview fue JPEG válido, la secuencia avanzó y el cleanup quedó limpio.
  Detalle: `tools/f1/artifacts/wallpaper-validation-20260813-031854/videos-sampled.json`.
- El primer soak lanzado desde la shell quedó truncado por el entorno tras
  cuatro muestras y no se cuenta como resultado. El soak válido de 4 h está
  cerrado en `tools/f1/artifacts/overnight-memory-20260813-4h.csv` y el
  posterior en `tools/f1/artifacts/overnight-memory-20260813-postfix-4h.csv`;
  ambos muestrearon cada 30 s sin cambiar settings ni wallpapers.
- La campaña con capturas quedó en
  `tools/f1/artifacts/wallpaper-validation-20260813-034349/`: 312/312
  procesadas, 310 JPEG de preview guardados (los dos casos sin captura fueron
  INVALID sin frame), 304 PASS, 5 CRASH y 3 INVALID; todos los outputs
  sintéticos desaparecieron y no quedaron PIDs/SHM residuales.
