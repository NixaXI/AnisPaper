#include "WebBrowserContext.h"
#include <stdexcept>

using namespace WallpaperEngine::WebBrowser;

WebBrowserContext::WebBrowserContext (WallpaperEngine::Application::WallpaperApplication&) {
    throw std::runtime_error ("AnisPaper: web wallpaper support is provided by the Qt WebEngine renderer, not CEF");
}
