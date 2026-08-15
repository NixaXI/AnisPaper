# Investigación de memoria del renderer de vídeo

Estado: matriz consolidada 2026-08-13. Esta nota separa experimentos aislados
de producción y no convierte una ventana corta en un veredicto de soak largo.
Los valores están en kB PSS salvo que se indique lo contrario. `no consta`
significa que el harness antiguo no conservó una muestra numérica reproducible;
no se inventa un valor para completar la tabla.

## Matriz de hipótesis

| Test / hipótesis | Implementación y evidencia | Duración / FPS | PSS inicial → final | Estimación | Resultado | Estado |
|---|---|---:|---:|---:|---|---|
| Soak de producción original | Un vídeo real en 1920×1080, daemon + bridge + Plasma | 3,995 h / 30 FPS de muestreo | 280283 → 414558 | +33609,7 kB/h | Crecimiento sostenido del hijo vídeo; daemon, escena y SHM estables | **CONFIRMADA** |
| Hijo aislado sin daemon/IPC | Renderer-child ejecutado solo, sin loop del daemon | 7,5 min / 60 | no consta en CSV consolidado; +3947 kB | ≈31 MiB/h | El crecimiento sobrevive sin RPC, JPEG del parent ni FrameBridge | **APOYADA** |
| Reutilización de buffers/flip | `flipFrameInPlace()` + scratch de una fila; luego readback RGB888 | 4 min / 1080p | no consta en CSV final; pendiente ≈35 MiB/h | ≈35 MiB/h | Quita churn `QImageData::create` y baja CPU, pero no la pendiente | **RECHAZADA como cura** |
| `malloc_trim` | Experimento temporal en el child después de cada frame | ventana aislada corta | no consta | no consta | No eliminó la pendiente; se retiró | **RECHAZADA** |
| Publicación de frames desactivada | Child sin publicar al protocolo/bridge | ventana aislada corta | no consta | no consta | El crecimiento persistió; no es backpressure del consumidor | **RECHAZADA** |
| JPEG/base64/IPC excluidos | Guard temporal de publicación JPEG; child aislado | 75 s / 60 | 264596 → 265145 | ≈26,3 MiB/h | La pendiente persiste sin encode/decode ni IPC | **RECHAZADA** |
| 1 FPS | Timer del child reducido para separar trabajo por frame | ventana aislada | no consta | casi plano frente a 60 FPS | La tasa de render correlaciona con el crecimiento; no es una solución compatible con vídeo normal | **APOYADA** |
| `glReadPixels` omitido | Render aislado sin readback a QImage | ventana aislada | no consta | pendiente persistente | Saltar la transferencia GPU→CPU no la elimina | **RECHAZADA** |
| `mpv_render_context_render` omitido | Harness aislado que no llama al render de libmpv | 420,4 s / 60 | 263157 → 263328 | ≈1,47 MiB/h | Queda prácticamente plano; es la separación causal más fuerte | **CONFIRMADA** |
| `report_swap` | Child aislado con drenaje de eventos y `mpv_render_context_report_swap()` | 420,4 s / 60 | 274864 → 278300 | ≈29,5 MiB/h | El API no corrige la pendiente | **RECHAZADA** |
| FPS adaptativo a `container-fps` | `FILE_LOADED` consulta `container-fps` y limita el timer | soak posterior 3,995 h | 280283 → 414558 | +33609,7 kB/h | Higiene de pacing; no corrige la retención | **RECHAZADA como cura** |
| Loop/reload y reemplazos | 20 ciclos vídeo A→B→C→A y 20 de escenas | 20 reemplazos/fase | sin crecimiento monotónico; rango dependiente del contenido | no aplica | No hay fuga por reemplazo/lifecycle; la fuga aparece en render continuo | **RECHAZADA** |
| `mpv_render_context_update()` | Llamada temporal antes de renderizar | ventana aislada corta | no consta | sin mejora medible | No redujo el crecimiento y se retiró | **RECHAZADA** |
| Drenaje de eventos mpv | `pumpEvents()` consume `mpv_wait_event(..., 0)` | soak posterior 3,995 h | 280283 → 414558 | +33609,7 kB/h | Evita cola de eventos, pero no evita la retención de render | **APOYADA como higiene; RECHAZADA como cura** |
| Drenaje + `report_swap` | Combinación del harness `/tmp/anispaper-video-report-swap.csv` | 420,4 s / 60 | 274864 → 278300 | ≈29,5 MiB/h | La combinación tampoco aplana la pendiente | **RECHAZADA** |

| Reproducer mínimo con `loop-file=inf` | Harness aislado, callback + `MPV_RENDER_UPDATE_FRAME`, vídeo de 14,7 s en bucle | 540,1 s útiles / 60 | PSS 260864 → 270465 kB; Private_Dirty 240924 → 245640 kB | +62,5 MiB/h PSS; +30,7 MiB/h privado | La retención reaparece sin daemon, SHM, JPEG, base64 ni Plasma al recargar el archivo | **CONFIRMADA en libmpv/render loop** |

## Reproducer mínimo independiente

`tools/perf/mpv_render_repro.cpp` contiene únicamente `QGuiApplication`, un
`QOffscreenSurface`, un contexto OpenGL, un FBO y libmpv. No enlaza el daemon,
`IsolatedRenderer`, FrameBridge, SHM, JPEG, base64, RPC ni QML. El sampler
reproducible es `tools/perf/minimal_mpv_soak.py`; ambos escriben sólo el CSV
indicado en `/tmp` y terminan el child con timeout/cleanup acotado.

Build reproducible (no usa CMake ni instala nada):

```sh
c++ -std=c++20 -O2 -fPIC -pie -Wall -Wextra -Wpedantic \
  tools/perf/mpv_render_repro.cpp \
  $(pkg-config --cflags --libs Qt6OpenGL Qt6Gui Qt6Core) -lmpv \
  -o /tmp/anispaper-mpv-render-repro
QT_QPA_PLATFORM=xcb DISPLAY=:0 \
  /tmp/anispaper-mpv-render-repro \
  --file '/mnt/DiscoE/SteamLibrary/steamapps/workshop/content/431960/2974744285/video 1080K6mNMVhgHVM.mp4' \
  --seconds 600 --fps 60 --update-driven
```

| Control | Ventana | PSS útil inicial → final | Pendiente | Lectura |
|---|---:|---:|---:|---|
| 1920×1080, readback RGBA, 60 llamadas/s | 9 min tras warm-up | 262002 → 270899 kB | ≈59307 kB/h (≈57,9 MiB/h) | Reproduce la pendiente sin AnisPaper |
| 1920×1080, readback RGBA, 1 llamada/s | 120 s tras warm-up | 267228 → 267231 kB | ≈90 kB/h | La pendiente depende fuertemente de la frecuencia de render |
| 1920×1080, callback + `MPV_RENDER_UPDATE_FRAME` | 60 s útiles tras warm-up | 272009 → 272019 kB | ≈600 kB/h (0,59 MiB/h) | El filtro de flags conserva el plateau; no se renderiza sin frame nuevo |
| 1920×1080, callback + `MPV_RENDER_UPDATE_FRAME` + loop | 540,1 s útiles tras warm-up | PSS 260864 → 270465 kB | ≈62,5 MiB/h PSS (30,7 MiB/h privado) | El loop/decode de libmpv vuelve a crecer aun sin AnisPaper |

Artefactos: `/tmp/anispaper-mpv-minimal-10m.csv`,
`/tmp/anispaper-mpv-minimal-1fps.csv`,
`/tmp/anispaper-mpv-minimal-update-flags-2m.csv` y
`/tmp/anispaper-mpv-minimal-loop-10m.csv`. La primera muestra de cada CSV se
descarta porque ocurre antes de que Qt/libmpv terminen de reservar sus mapas.
El reproducer emitió frames y terminó sin crash; `Cannot load libcuda.so.1` es
el diagnóstico esperado al usar `hwdec=no`, no una falla del test.

## Lectura causal

El control decisivo es el mismo child con y sin
`mpv_render_context_render()`: al omitirlo la pendiente casi desaparece;
`glReadPixels`, JPEG/base64, IPC, `malloc_trim`, drenaje de eventos y
`report_swap` no la explican. El perfil tcmalloc complementario observó unos
208 MiB vivos en `av_malloc` de libavutil, mientras que los mapas de libmpv,
FFmpeg y libplacebo permanecieron esencialmente constantes. La hipótesis más
fuerte es una retención interna de la ruta de render/decoder de libmpv, o una
interacción Mesa/AMD, no una fuga de ownership de AnisPaper.

El control en bucle refuerza esa atribución: el proceso mínimo, sin AnisPaper ni
IPC, subió 62,5 MiB/h de PSS (30,7 MiB/h privado) al repetir el mismo archivo.
El callback `MPV_RENDER_UPDATE_FRAME` se conserva como mitigación de renders
redundantes, pero no se presenta como cura de la retención del decoder.

No se adopta un reinicio periódico ni se omite el render en producción: eso
ocultaría el problema, podría introducir flicker y dañaría vídeo normal. El
readback RGB888 y el flip por filas sí quedan porque reducen allocations y CPU
sin cambiar el contrato ni la calidad.

## Evidencia y límites

- Producción: `tools/f1/artifacts/overnight-memory-20260813-postfix-4h.csv`.
- Control de `report_swap`: `/tmp/anispaper-video-report-swap.csv` (15 muestras,
  30 s; 7 min 0,4 s).
- Control sin render mpv: `/tmp/anispaper-video-skip-mpv.csv` (15 muestras,
  30 s; 7 min 0,4 s).
- La ventana de 4 h no prueba una meseta; por eso el issue P1 permanece abierto
  en `docs/known-issues.md`.
- La prueba sin `mpv_render_context_render` es causal para el crecimiento, pero
  no es un candidato funcional: produce un frame que no se actualiza.
- El soak integrado de 15 min del worker nuevo se conserva aparte en
  `/tmp/anispaper-video-child-update-flags-15m.csv`; se cuenta sólo cuando el
  proceso termina con `child_rc=0` y la muestra posterior descarta el warm-up.
  En 840,1 s útiles quedó PSS 284752 → 286793 kB (+8,54 MiB/h): mejora clara
  frente a +33,6 MiB/h, pero todavía no es una meseta. Es una mitigación de
  trabajo redundante, no el cierre del problema P1.
