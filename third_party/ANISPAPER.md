# Vendored third-party components (AnisPaper)

## `linux-wallpaperengine`

Upstream: https://github.com/Almamu/linux-wallpaperengine
License: GPL-3.0 (see `linux-wallpaperengine/LICENSE`)

Used as the real rendering engine for Wallpaper Engine `type:"scene"`
wallpapers. It is **compiled as a static library (`wallpaperengine-core`)** and
linked only into `anis-paper-scene-engine`, the isolated offscreen child
process — never into Plasma.

Vendoring differences vs upstream (see `linux-wallpaperengine/CMakeLists.txt.orig`
for the original build script):

- Library-only build; the upstream standalone binary/`main.cpp` is not built.
- CEF/web wallpaper support removed (`WebBrowserContext` is a stub that throws
  if a web project is ever constructed; `CWallpaper` refuses `Web` types with a
  clear error). Web wallpapers keep using AnisPaper's Qt WebEngine renderer.
- Wayland/layer-shell drivers, tests and Catch2 excluded; only X11/GLFW is
  built to keep the offscreen hidden-window path.
- `DPMS/GLFWOpenGLDriver` registers itself through
  `__attribute__((constructor))`; the child must link the library with
  `-Wl,--whole-archive` (see top-level `CMakeLists.txt`).

AnisPaper patches applied on the vendored sources (all marked `AnisPaper:` in
code):

- `ApplicationContext.h`: added `settings.render.offscreen` flag.
- `GLFWWindowOutput.cpp`: skip `driver.showWindow()` when offscreen, so the
  GLFW window is created but never mapped (no floating window).
- `WallpaperApplication.{h,cpp}`: `getRenderContext()` and
  `setFrameCallback()` — invoked after every dispatched frame, used to read
  the wallpaper FBO and emit protocol frames.

## `glfw`

Upstream: https://github.com/glfw/glfw (3.4 branch), zlib.
Vendored because the host lacked the system glfw package and sudo-based AUR
installation was not available; it is built X11-only as a static library.
