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
- Aplicar una captura al login escribe configuración del sistema y requiere
  autenticación administrativa. No se ejecuta automáticamente. La ruta cambia
  el greeter de inicio/cierre, no el lock screen de la sesión activa.
