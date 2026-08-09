# AnisPaper UI

## Arranque en Wayland + AMD

`npm run dev` y `npm run start` detectan una sesión Wayland y ejecutan Electron
mediante XWayland con estos flags de compatibilidad:

```text
--disable-gpu
--disable-software-rasterizer
--use-gl=desktop
--ozone-platform=x11
```

La prueba real del 2026-08-09 se ejecutó desde la sesión KDE/Wayland con
`WAYLAND_DISPLAY=wayland-0`. Resultado literal relevante:

```text
[AnisPaper] Electron flags: --disable-gpu --disable-software-rasterizer --use-gl=desktop --ozone-platform=x11
[AnisPaper] Electron listo
[AnisPaper] Creando ventana...
```

Por ello no se activaron los fallbacks de `BrowserWindow.offscreen` ni un
launcher que fuerce `XDG_SESSION_TYPE=x11`: ambos quedan innecesarios y no se
modificó la lógica de la ventana ni el contrato del daemon.

En una sesión X11 los flags no se añaden y Electron conserva su configuración
normal. El runtime tampoco duplica una variante de flag que el usuario pase de
forma explícita.
