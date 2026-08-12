#pragma once

// AnisPaper: CEF-based web wallpaper support is intentionally NOT vendored.
// Web wallpapers are handled by AnisPaper's own Qt WebEngine renderer.
// This stub keeps the engine sources compiling for scene/video types.

namespace WallpaperEngine::Application {
class WallpaperApplication;
}

namespace WallpaperEngine::WebBrowser {
class WebBrowserContext {
public:
    explicit WebBrowserContext (WallpaperEngine::Application::WallpaperApplication& wallpaperApplication);
    ~WebBrowserContext () = default;
};
} // namespace WallpaperEngine::WebBrowser
