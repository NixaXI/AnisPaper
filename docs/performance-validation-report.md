# AnisPaper — validación de rendimiento y estabilidad

Estado: validación consolidada 2026-08-13. El resumen histórico de la primera
pasada se conserva más abajo; la actualización de 4 h y la comprobación final
de flicker están al final de este documento.

## Flicker

La captura previa `2026-08-13 03-03-34.mp4` mostró 156 frames a 60 FPS con
alternancia repetida entre wallpaper y el fallback oscuro (`YAVG≈159` frente a
`YAVG≈28`). El productor continuaba publicando frames; el fallo estaba en el
consumidor QML, que descartaba la textura previa al cambiar la URL del frame.

Fix: `retainWhileLoading: true` en
`packaging/plasma/org.anispaper.frame/contents/ui/main.qml`. El smoke
`anispaper-f3-plugin-smoke` pasa y la recarga controlada dejó
`plasma-plasmashell.service` activo. No se reintrodujo ningún selector KWin.

## Cobertura de escenas

Campaña completa: [summary](../tools/f1/artifacts/wallpaper-validation-20260813-034349/summary.json)

- descubiertas/procesadas: 312/312
- PASS: 304
- CRASH: 5 (paquetes con `scene.json` ausente)
- INVALID: 3 (contenido faltante o JSON inválido)
- TIMEOUT: 0
- capturas JPEG de preview: 310; los dos sin captura fueron INVALID sin frame
- cleanup: 312/312 con `entryGone=true`, sin PIDs ni SHM residuales
- capturas: `../tools/f1/artifacts/wallpaper-validation-20260813-034349/captures/`

Las capturas son previews RPC del frame producido por el daemon. No se usó un
selector interactivo; una captura final limpia de Wayland no es fiable mientras
hay ventanas abiertas y queda documentada como pendiente de una sesión sin
ventanas.

## Videos

Catálogo: 219. Cobertura actual: muestra representativa 8/8 PASS (4K60, 4K30,
1440p, 1080p60/30, 720p30/60 y vertical), con secuencia avanzada, JPEG válido
y cleanup por caso. Detalle en
`../tools/f1/artifacts/wallpaper-validation-20260813-031854/videos-sampled.json`.

## Estado real preservado

DP-2 mantiene `scene` (~58 FPS, `hasFrame=true`) y HDMI-A-1 mantiene `video`
(~30 FPS, `hasFrame=true`), con PIDs estables, `crashes=0`, `safeMode=false` y
watchdog 0. Sólo permanecen los dos bridges físicos y el transporte scene de
DP-2.

## Soak

El sampler read-only corrió como `anispaper-memory-soak.service`, limitado a 6 h
y muestreo cada 30 s, pero fue detenido al regresar el usuario:

`../tools/f1/artifacts/overnight-memory-20260813-034038.csv`

Resumen final de la ventana disponible (40 muestras, 1173,6 s): PSS del daemon
104662→102859 kB, scene 84862→84860 kB, video 312914→320869 kB; SHM final 3
objetos y 41472128 bytes. No se declara ausencia de leak a largo plazo con esta
ventana corta.

## Follow-up de ciclo de vida y Gaming Mode

El daemon ahora procesa SIGTERM por un self-pipe y limpia sus renderers antes
de salir. La prueba `systemctl --user stop` dejó cero procesos y cero objetos
`anispaper-*` en `/dev/shm`; el `start` siguiente restauró un renderer por
output (DP-2 scene, HDMI-A-1 video) con frames válidos. El perfil aislado del
video midió aproximadamente 265 MiB PSS a 640x360, 270 MiB a 1920x1080,
280 MiB a 2560x1440 y 304 MiB a 3840x2160; quedó estable durante 5 s por caso.
Se probó y retiró un experimento `config=no` + `profile=fast` de libmpv: el
perfil repetido 1080p no mostró una caída material de memoria ni CPU.

Gaming Mode automático detectó `STEAM_COMPAT_APP_ID`, congeló `frameNo` en ambos
outputs manteniendo la última imagen, y reanudó al terminar el proceso. No se
inició SMITE desde esta validación.

El control A/B/C de plasmashell dio PSS 458.018 MiB con AnisPaper, 463.426 MiB
con el daemon detenido y 460.547 MiB al restaurarlo; no corresponde atribuir
todo el PSS de Plasma al wallpaper. La campaña de reemplazo estable hizo 20
ciclos por tipo en outputs reales, mantuvo 3 SHM (41.472.128 B) y restauró los
IDs originales. La ventana de 4 h posterior ya está cerrada; sus cifras
consolidadas se detallan a continuación.

## Actualización final — soak de 4 h, MP4 retirado y verificación de bridges

La captura `2026-08-13 03-03-34.mp4` fue inspeccionada cuadro a cuadro: sus 156
frames/2,6 s alternan repetidamente entre el wallpaper y `#0A0D14`. Era una
captura pre-fix. El archivo fue eliminado de `/home/merminik` a pedido; no hay
vídeos de captura en el árbol del proyecto. Los vídeos que aparecen en
`tests/` son nombres de fixtures generados temporalmente y no son esa captura.

Después del fix (`retainWhileLoading: true`), la lectura real durante 10 s de
los dos bridges físicos produjo 923 headers válidos, 0 inválidos, 0 descensos
de `frameNo` y sólo los marcadores busy esperados (43 en DP-2, 26 en
HDMI-A-1). El estado RPC quedó `watchdog.count=0`, `safeMode=false`,
`hasFrame=true`, con un renderer por output. Esto verifica que el productor y
el transporte no generan una textura regresiva; no se declara una captura
compositor final sin ventanas.

El soak posterior de 4 h sí terminó y no confirmó una meseta del hijo de vídeo:

| Proceso | PSS inicial | PSS final | Crecimiento/h |
|---|---:|---:|---:|
| daemon | 108096 kB | 108091 kB | -1,25 kB/h |
| scene | 82746 kB | 83183 kB | 109,4 kB/h |
| vídeo | 280283 kB | 414558 kB | 33609,7 kB/h |
| plasmashell | 413490 kB | 414009 kB | 129,9 kB/h |

Los artefactos son
`tools/f1/artifacts/overnight-memory-20260813-postfix-4h.csv` y
`tools/f1/artifacts/overnight-memory-20260813-postfix-final.txt`. El inventario
SHM permaneció exactamente en 3 objetos/41472128 bytes y no hubo acumulación en
los 20 reemplazos por tipo. Un hijo de vídeo aislado sin loop también creció
3947 kB en 7,5 min (aprox. 31 MiB/h), por lo que el crecimiento está dentro de
la ruta de render de libmpv/OpenGL, no en el loop de archivo ni en el bridge.

Conclusión honesta: Gaming Mode, limpieza de procesos/SHM y el fix de flicker
están verificados; la fuga de memoria del render de vídeo sigue abierta y no
se aplicó un reinicio periódico ni una reducción de calidad que pudiera
introducir flicker. El cambio de bombeo de eventos y ajuste de FPS a la tasa
nativa se conserva por corrección/pacing, pero el soak demuestra que no es una
solución de memoria.

La mitigación final de churn del vídeo está instalada: `VideoRenderer` hace el
flip vertical intercambiando filas mediante un scratch reutilizable, sin crear
un `QImageData` completo por frame. Build, cuatro tests focalizados, instalación
y restart del servicio pasaron. La prueba aislada 1920×1080 de 4 min quedó en
~35 MiB/h, prácticamente igual al soak de 4 h; por eso se reporta como ahorro
de allocations temporales, no como corrección del leak de libmpv.

Además, el readback usa `RGB888` compacto cuando el stride lo permite, evitando
la conversión RGBA→RGB del encoder. El perfil de heap dejó de mostrar
`QImageData::create` como allocation dominante; el hijo real continuó entregando
JPEGs válidos en ambos outputs. La retención de `av_malloc`/libmpv permanece
abierta.

La muestra CPU posterior al deploy (top por core, 8 s) fue daemon 46,3%, vídeo
29,7%, scene 13,0% y plasmashell 50,5%; el vídeo bajó frente a la muestra
anterior de 35,6% sin cambiar el contrato RPC/SHM.

El muestreo real posterior de 120 s (`/tmp/anispaper-video-rgb-postdeploy.csv`)
pasó de 288175 a 289235 kB PSS (≈31,8 MiB/h), coherente con la pendiente del
soak largo: la mejora RGB reduce trabajo/allocations, pero no elimina la
retención de libmpv.

Se intentó iniciar un soak final de 2 h con el build RGB, pero la detección
automática encontró un juego Proton real (NIKKE) y dejó ambos renderers en
Gaming Mode (`gamingPaused=true`). Se detuvo a los 60 s para no contaminar la
métrica ni desactivar el modo de juego del usuario; no se presenta como soak
activo de vídeo.

### Verificación posterior al último deploy

Se reinstaló el build que conserva el readback `RGB888` cuando el stride es
compacto y el flip por filas reutilizable, y se reinició únicamente
`anispaper.service`. El `real-check` posterior quedó estable en generación 2:
catálogo 545, `watch_count=798`, `watch_failures=0`, ambos outputs
`HDMI-A-1`/`DP-2`, `watchdog.count=0` y `safeMode=false`. La lectura de
`preview.frame` devolvió JPEG válido `1920x1080` en ambos outputs (firmas
`FFD8`/`FFD9`).

Durante la comprobación NIKKE seguía activo: Gaming Mode estaba pausando ambos
renderers y preservando el último frame. Dos consultas `status.get` separadas
por 2 s mantuvieron `frameNo=1`, `hasFrame=true` y `gamingPaused=true` en las
dos salidas. Esto descarta un flicker del productor durante la pausa, pero no
se presenta como una captura limpia del compositor mientras el juego está
activo.

El `ctest --test-dir build --output-on-failure` final pasó 6 pruebas nativas y
dejó 5 integraciones omitidas por las restricciones del sandbox (AF_UNIX/GUI);
las cuatro pruebas focales del cambio RGB pasaron 4/4.
