## F4 — 2026-08-09

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
