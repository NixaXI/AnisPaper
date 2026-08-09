# AnisPaper F0 — probe del sistema

Fecha de ejecución: 2026-08-09 (sesión local). Este memo es el límite de
evidencia para F0: no extrapola el resultado a otros monitores, usuarios ni
sesiones. No contiene valores de claves ni el contenido de proyectos Steam.

## Resultado y decisión

La sesión detectada es **Wayland/KDE**. La estrategia que sigue vigente para
F1–F3 es **Bridge SHM** (daemon aislado renderiza offscreen y entrega frames al
plugin Plasma). Es la ruta que conserva el principio de que `plasma-shell` sólo
muestra frames estáticos y que iconos/carpetas quedan por encima.

La ejecución real posterior, lanzada mediante `systemd --user`, verificó el
protocolo y la configuración de una surface `background`; no verificó su orden
Z ni su visibilidad en el escritorio. La captura contemporánea no contiene el
marcador `F0 RUN` ni el run-ID que el log afirma haber pintado. Por tanto,
**layer-shell directo queda NO HABILITADO como fallback en esta
sesión/configuración**. Sólo una prueba futura que correlacione el mismo run-ID
en log y captura, con iconos/carpetas encima, puede cambiar ese estado.

Bridge SHM continúa como estrategia primaria y única aceptada. No se diseñó ni
implementó bridge, daemon, plugin Plasma, renderer, UI o instalación en F0.

| Área | Estado | Evidencia local |
|---|---|---|
| Sesión | Detectado | `XDG_SESSION_TYPE=wayland`, `XDG_CURRENT_DESKTOP=KDE` |
| KWin | Detectado | `kwin 6.7.4` |
| Acceso al display Wayland | Verificado | Ejecución real: `event=connect status=ok` |
| Global `zwlr_layer_shell_v1` | Verificado | Anunciado por KWin en versión 5; el cliente enlazó versión 4 |
| Surface background real | Verificado técnicamente | `configured layer=background width=1422 height=754`; mismo run-ID en request/configure del log |
| Marcador visual + iconos encima | Fallido / no verificado | Captura contemporánea sin `F0 RUN` ni run-ID; el orden Z no se puede afirmar |
| Steam Workshop 431960 | Detectado | 2 referencias de biblioteca; inventario raw de 1090 `project.json` (véase aclaración de aliases) |
| `mpv` / libmpv | Detectado | binario y `/usr/lib/libmpv.so` presentes |
| `linux-wallpaperengine` | No detectado | binario y biblioteca no hallados en rutas del probe |
| `v4l2loopback` | Detectado | módulo 0.15.4 disponible vía `modinfo` |
| `pkexec` | Detectado | `/usr/bin/pkexec` presente; no se usó |
| `steamcmd` | No detectado | ausente de `PATH`; no se intentó descarga |
| API Steam | No configurada | Ninguna de las variables de entorno permitidas por el probe está configurada; su valor jamás se lee |

## Scaffold reutilizable

El único binario F0 previsto es `anispaper-layer-shell-probe`. Es C++20 y usa
`wayland-client`; CMake genera el cliente para el protocolo vendorizado
`wlr-layer-shell-unstable-v1.xml` con `wayland-scanner`.

Cuando el socket sea accesible, el binario:

1. Enumera globals y anuncia la versión de `zwlr_layer_shell_v1`.
2. Admite `--output NAME` y enumera los outputs disponibles; sin esa opción deja
   que el compositor seleccione el destino.
3. Crea una `wl_surface` de capa `background`, anclada a los cuatro bordes,
   sin interactividad de teclado, y pinta un fondo ANIS STAR con `F0 RUN` y un
   run-ID único y legible.
4. Registra líneas `ANISPAPER_PROBE event=...`, tiene timeout finito
   (`--timeout-ms`, 100–60000 ms) y destruye todos los objetos Wayland al salir.

El script no destructivo `tools/probe/inventory.py` hace inventario sin ejecutar
VDF ni JSON. Tokeniza `libraryfolders.vdf`, acepta sólo valores de `path`
absolutos, recorre exactamente
`steamapps/workshop/content/431960/*/project.json` de cada biblioteca y publica
sólo rutas, IDs, conteos y tipos. Limita cada JSON a 5 MiB. Para API Steam sólo
declara `configured`/`not configured`; nunca carga ni imprime un secreto.

## Inventario Steam y dependencias observado

Fuente: `python3 tools/probe/inventory.py`.

| Elemento | Resultado |
|---|---|
| `libraryfolders.vdf` | `/home/merminik/.local/share/Steam/steamapps/libraryfolders.vdf` |
| Biblioteca 0 | `/home/merminik/.local/share/Steam` |
| Biblioteca 1 | `/mnt/DiscoE/SteamLibrary` |
| `project.json` AppID 431960 (referencias raw) | 1090 |
| Tipos (valor exacto de `type`) | `Scene`: 102; `Video`: 42; `Web`: 10; `missing`: 4; `scene`: 522; `video`: 396; `web`: 14 |
| `steamcmd` | ausente; prueba `workshop_download_item` anónima **no intentada** |
| `mpv`, libmpv | presentes |
| `v4l2-ctl`, módulo v4l2loopback | presentes; módulo `0.15.4` |
| `pkexec` | presente, no ejecutado |
| `linux-wallpaperengine` (bin/lib) | ausente |
| Captura | `spectacle` presente; `grim` ausente |

La prueba anónima de Steam queda explícitamente pendiente: sólo se podrá hacer
si `steamcmd` aparece y existe aprobación para una descarga pública, acotada y
no destructiva. No se hizo ninguna llamada de red ni se cambió configuración del
sistema.

### Aclaración posterior F1: aliases físicos y catálogo normalizado

El inventario anterior se conserva como evidencia histórica raw. Las dos rutas
`content/431960` canonicalizan al mismo directorio físico: las 1090 referencias
son dos recorridos de los mismos 545 proyectos, no 1090 instalaciones distintas.
F1 deduplica la raíz `content` por path canónico antes de recorrerla, por lo que
el catálogo esperado tiene **545 IDs Steam únicos**. Con el parser F1, que
conserva assets declarados aunque aún no existan (por ejemplo `scene.json` de un
scene package), los tipos normalizados esperados son: `scene=312`, `video=219`,
`web=12`, `unknown=2`. Esta tabla no reemplaza ni reescribe los valores raw de
arriba: distingue conteos de referencias y catálogo físico normalizado.

## Comandos ejecutados y resultado exacto

```sh
python3 -m py_compile tools/probe/inventory.py
python3 tools/probe/inventory.py
```

Resultado del preflight dentro del sandbox (no sustituye la prueba real):

```text
ANISPAPER_F0_INVENTORY version=1
session.type=wayland
session.desktop=KDE
session.kwin_wayland.output=kwin 6.7.4
session.wayland_info.status=failed
session.wayland_info.output=failed to create display: Operation not permitted
steam.library_count=2
steam.project_json_count=1090
steam.project_types={"Scene": 102, "Video": 42, "Web": 10, "missing": 4, "scene": 522, "video": 396, "web": 14}
steam.steamcmd=absent
steam.anonymous_workshop_test=not attempted: steamcmd unavailable
steam.api_key=not configured
dependency.binary.mpv=present
dependency.binary.pkexec=present
dependency.binary.v4l2-ctl=present
dependency.libmpv=present
dependency.libwallpaperengine=absent
dependency.v4l2loopback.status=ok
```

El build CMake solicitado se intentó literalmente:

```sh
cmake -S . -B build/f0 && cmake --build build/f0
```

Resultado exacto del entorno: `/usr/bin/bash: line 1: cmake: command not found`.
Por eso no se puede afirmar un build CMake verde aquí. El scaffold sí se validó
sin instalar nada con las herramientas de desarrollo disponibles:

```sh
wayland-scanner client-header src/probe/protocols/wlr-layer-shell-unstable-v1.xml /tmp/wlr-layer-shell-unstable-v1-client-protocol.h
wayland-scanner private-code src/probe/protocols/wlr-layer-shell-unstable-v1.xml /tmp/wlr-layer-shell-unstable-v1-protocol.c
cc -I/tmp $(pkg-config --cflags wayland-client) -c /tmp/wlr-layer-shell-unstable-v1-protocol.c -o /tmp/wlr-layer-shell-unstable-v1-protocol.o
cc $(pkg-config --cflags wayland-client) -c src/probe/protocols/xdg-popup-stub.c -o /tmp/xdg-popup-stub.o
c++ -std=c++20 -Wall -Wextra -Wpedantic -I/tmp src/probe/main.cpp /tmp/wlr-layer-shell-unstable-v1-protocol.o /tmp/xdg-popup-stub.o $(pkg-config --cflags --libs wayland-client) -o /tmp/anispaper-layer-shell-probe
/tmp/anispaper-layer-shell-probe --help
/tmp/anispaper-layer-shell-probe --timeout-ms 1000
```

Resultado:

```text
Usage: /tmp/anispaper-layer-shell-probe [--output NAME] [--timeout-ms N]
Creates one zwlr_layer_shell_v1 background surface with a visible F0 run ID.
ANISPAPER_PROBE event=start run_id=19FE4A694E2-2 timeout_ms=1000 requested_output=auto
ANISPAPER_PROBE event=connect status=failed errno=1 message=Operation_not_permitted
```

El ID es por ejecución y por ello cambia en futuras pruebas. Ese intento fue
bloqueado antes de que el tester pudiera anunciar protocolo, configurar la
surface o producir una captura; describe sólo el preflight sandbox.

## Ejecución real KDE/Wayland y resultado visual

El padre lanzó el binario compilado manualmente mediante `systemd-run --user`; la
evidencia persistida está en `artifacts/probe-desktop-visible/`. El inventario
de esa ejecución sí pudo consultar Wayland (`session.wayland_info.status=ok`).
El log completo se conserva en `layer-shell.txt`; sus líneas relevantes son:

```text
ANISPAPER_PROBE event=start run_id=19FE4ABC97B-3C25D timeout_ms=8000 requested_output=auto
ANISPAPER_PROBE event=connect status=ok
ANISPAPER_PROBE event=global protocol=zwlr_layer_shell_v1 status=advertised version=5
ANISPAPER_PROBE event=output status=available name=HDMI-A-1
ANISPAPER_PROBE event=output status=available name=DP-2
ANISPAPER_PROBE event=surface status=requested layer=background protocol_version=4 run_id=19FE4ABC97B-3C25D
ANISPAPER_PROBE event=surface status=configured layer=background width=1422 height=754 run_id=19FE4ABC97B-3C25D
ANISPAPER_PROBE event=finish status=timeout_clean configured=1 run_id=19FE4ABC97B-3C25D
```

Esto verifica acceso al compositor, anuncio de protocolo y entrega de
`configure` para una surface de fondo. El cliente limita deliberadamente el
bind a v4 aunque KWin anuncia v5, porque el XML vendorizado es v4.

La inspección visual primaria de `desktop.png` (3841×1080, RGBA) fue negativa:
el marcador `F0 RUN` y `19FE4ABC97B-3C25D` no aparecen. Las superficies normales
ya existentes permanecen visibles. También se intentó
`org.kde.KWin.showDesktop(true)` antes de capturar y se restauró a `false`
después; no produjo una escena visual correlacionada. No se deduce de ello un
orden Z concreto ni una causa de composición. En particular, no prueba que los
iconos estén encima de la surface ni que la surface sea visible al usuario.

Correlación del archivo de captura:

```text
path=artifacts/probe-desktop-visible/desktop.png
sha256=5d15dbcff6430cff36e070794a49bea5acdf9c84a2033aac995bcba3a55eba75
dimensions=3841x1080 RGBA
```

Conclusión operativa: el fallback layer-shell directo sigue **NO HABILITADO**.
Bridge SHM es la estrategia primaria y la única aceptada para Wayland/KWin en
esta sesión/configuración.

## Repetición de la prueba visual

En una sesión Wayland sin el bloqueo del sandbox, después de que CMake esté
instalado, ejecutar:

```sh
cmake -S . -B build/f0 && cmake --build build/f0
bash tools/probe/run-f0.sh --binary build/f0/anispaper-layer-shell-probe --timeout-ms 8000 --capture spectacle
```

El script guarda `inventory.txt`, `layer-shell.txt` y, si la captura tiene
éxito, `desktop.png` en `artifacts/probe/`. La captura se realiza dos segundos
después de iniciar el probe, antes de esperar su salida. La aceptación visual
es válida sólo si `layer-shell.txt` contiene `status=configure_received` y
`status=buffer_committed layer=background` (y finaliza con
`buffer_flushed=1`) para el mismo run-ID visible en `desktop.png`, y los
iconos/carpetas del escritorio aparecen encima en ese mismo monitor y sesión.
El estado `presentation=not_observable` no afirma visibilidad: si falta
cualquiera de esas piezas, el fallback layer-shell continúa no habilitado.

El runner llama al inventario mediante `python3` (no depende del bit ejecutable
del archivo). Si `grim` o `spectacle` falla, espera igualmente al probe y
conserva `inventory.txt` y `layer-shell.txt` antes de devolver el error.
